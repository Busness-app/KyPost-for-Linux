#include "update/UpdateCheckController.h"

#include "../ExecutorShutdownGuard.h"
#include "domain/PairingStore.h"
#include "net/NetworkExecutor.h"
#include "stores/SecureStoreFile.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

// applyResult() is the real controller's state-transition logic -- the same
// function checkNow()'s network callback feeds into. Made public on the
// class for exactly this seam (see the header). No fake HTTP server needed:
// nothing here goes near the network.
//
// PairingStore/NetworkExecutor are still required by the constructor, so
// each test builds the same minimal, unpaired fixture other tests/app/**
// controller tests use (e.g. PgpQrControllerTest.cpp) -- an on-disk
// SecureStoreFile under a QTemporaryDir, never saved to. Declaration order
// matters here for the same reason RefreshFixture documents in
// MailControllerTest.cpp: the executor must outlive the controller, and
// ExecutorShutdownGuard (last member) ensures no queued work outlives the
// test's stack frame.
struct Fixture
{
    QTemporaryDir secureDir;
    SecureStoreFile secureStore{ secureDir.path() };
    PairingStore pairingStore{ secureStore }; // never saved -- not paired
    NetworkExecutor executor{ 3000 };
    UpdateCheckController controller{ pairingStore, executor };
    ExecutorShutdownGuard shutdownGuard{ executor };
};

namespace {
ClientVersionResult okResult(const QString& latestVersion, const QString& checkedAt = QStringLiteral("2026-08-25T00:00:00Z"))
{
    ClientVersionResult result;
    result.latestVersion = latestVersion;
    result.checkedAt = checkedAt;
    result.supported = true;
    return result;
}
} // namespace

class UpdateCheckControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void installedVersionIsTheCompiledInConstant();

    void aNewerLatestVersionSetsUpdateAvailableAndEmitsTheTransitionSignalOnce();
    void anEqualOrOlderLatestVersionLeavesUpdateAvailableFalseAndEmitsNoTransition();
    void aNetworkErrorLeavesLastKnownGoodValuesIntactAndEmitsNothing();
    void anUnsupportedServerLeavesLastKnownGoodValuesIntactAndEmitsNothing();
    void aSecondConsecutiveNewerResultDoesNotReEmitTheTransitionSignal();
};

// The left-hand side of the comparison must come from the build, not from a
// second hand-maintained copy. A constant that drifts from the tag makes a
// current install nag forever -- see the spec and server_version.go:12-28.
void UpdateCheckControllerTest::installedVersionIsTheCompiledInConstant()
{
    QCOMPARE(UpdateCheckController::compiledInVersion(), QStringLiteral(KYPOST_VERSION));
}

void UpdateCheckControllerTest::aNewerLatestVersionSetsUpdateAvailableAndEmitsTheTransitionSignalOnce()
{
    Fixture f;
    QSignalSpy changedSpy(&f.controller, &UpdateCheckController::changed);
    QSignalSpy becameAvailableSpy(&f.controller, &UpdateCheckController::updateBecameAvailable);

    // Comparison must run as isNewer(latest, installed) with the compiled-in
    // version on the left of "installed" -- swapped arguments would report
    // every install as up to date forever, or every install as behind
    // forever, and nothing else here would catch it.
    const QString installed = UpdateCheckController::compiledInVersion();
    const QStringList parts = installed.split(QLatin1Char('.'));
    QCOMPARE(parts.size(), 3);
    const QString newer = QStringLiteral("%1.%2.%3")
                               .arg(parts[0].toInt())
                               .arg(parts[1].toInt())
                               .arg(parts[2].toInt() + 1);

    f.controller.applyResult(okResult(newer, QStringLiteral("2026-08-25T12:00:00Z")));

    QCOMPARE(f.controller.latestVersion(), newer);
    QCOMPARE(f.controller.checkedAt(), QStringLiteral("2026-08-25T12:00:00Z"));
    QVERIFY(f.controller.updateAvailable());
    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(becameAvailableSpy.count(), 1);
}

void UpdateCheckControllerTest::anEqualOrOlderLatestVersionLeavesUpdateAvailableFalseAndEmitsNoTransition()
{
    Fixture f;
    QSignalSpy becameAvailableSpy(&f.controller, &UpdateCheckController::updateBecameAvailable);

    const QString installed = UpdateCheckController::compiledInVersion();

    f.controller.applyResult(okResult(installed)); // equal
    QVERIFY(!f.controller.updateAvailable());

    const QStringList parts = installed.split(QLatin1Char('.'));
    QCOMPARE(parts.size(), 3);
    const int lastPart = parts[2].toInt();
    const QString older = QStringLiteral("%1.%2.%3")
                               .arg(parts[0].toInt())
                               .arg(parts[1].toInt())
                               .arg(lastPart > 0 ? lastPart - 1 : 0);
    if (older != installed) {
        f.controller.applyResult(okResult(older));
        QVERIFY(!f.controller.updateAvailable());
    }

    QCOMPARE(becameAvailableSpy.count(), 0);
}

void UpdateCheckControllerTest::aNetworkErrorLeavesLastKnownGoodValuesIntactAndEmitsNothing()
{
    Fixture f;

    const QString installed = UpdateCheckController::compiledInVersion();
    const QStringList parts = installed.split(QLatin1Char('.'));
    const QString newer = QStringLiteral("%1.%2.%3")
                               .arg(parts[0].toInt())
                               .arg(parts[1].toInt())
                               .arg(parts[2].toInt() + 1);
    f.controller.applyResult(okResult(newer, QStringLiteral("2026-08-25T12:00:00Z")));
    QVERIFY(f.controller.updateAvailable());

    QSignalSpy changedSpy(&f.controller, &UpdateCheckController::changed);
    QSignalSpy becameAvailableSpy(&f.controller, &UpdateCheckController::updateBecameAvailable);

    ClientVersionResult failure;
    failure.error = NetworkError::Transport;
    f.controller.applyResult(failure);

    QCOMPARE(f.controller.latestVersion(), newer);
    QCOMPARE(f.controller.checkedAt(), QStringLiteral("2026-08-25T12:00:00Z"));
    QVERIFY(f.controller.updateAvailable());
    QCOMPARE(changedSpy.count(), 0);
    QCOMPARE(becameAvailableSpy.count(), 0);
}

void UpdateCheckControllerTest::anUnsupportedServerLeavesLastKnownGoodValuesIntactAndEmitsNothing()
{
    Fixture f;

    const QString installed = UpdateCheckController::compiledInVersion();
    const QStringList parts = installed.split(QLatin1Char('.'));
    const QString newer = QStringLiteral("%1.%2.%3")
                               .arg(parts[0].toInt())
                               .arg(parts[1].toInt())
                               .arg(parts[2].toInt() + 1);
    f.controller.applyResult(okResult(newer, QStringLiteral("2026-08-25T12:00:00Z")));
    QVERIFY(f.controller.updateAvailable());

    QSignalSpy changedSpy(&f.controller, &UpdateCheckController::changed);
    QSignalSpy becameAvailableSpy(&f.controller, &UpdateCheckController::updateBecameAvailable);

    ClientVersionResult unsupported;
    unsupported.supported = false; // 404 -- server too old for the endpoint
    f.controller.applyResult(unsupported);

    QCOMPARE(f.controller.latestVersion(), newer);
    QCOMPARE(f.controller.checkedAt(), QStringLiteral("2026-08-25T12:00:00Z"));
    QVERIFY(f.controller.updateAvailable());
    QCOMPARE(changedSpy.count(), 0);
    QCOMPARE(becameAvailableSpy.count(), 0);
}

void UpdateCheckControllerTest::aSecondConsecutiveNewerResultDoesNotReEmitTheTransitionSignal()
{
    Fixture f;
    QSignalSpy becameAvailableSpy(&f.controller, &UpdateCheckController::updateBecameAvailable);

    const QString installed = UpdateCheckController::compiledInVersion();
    const QStringList parts = installed.split(QLatin1Char('.'));
    const QString newer = QStringLiteral("%1.%2.%3")
                               .arg(parts[0].toInt())
                               .arg(parts[1].toInt())
                               .arg(parts[2].toInt() + 1);
    const QString evenNewer = QStringLiteral("%1.%2.%3")
                                   .arg(parts[0].toInt())
                                   .arg(parts[1].toInt())
                                   .arg(parts[2].toInt() + 2);

    f.controller.applyResult(okResult(newer, QStringLiteral("2026-08-25T12:00:00Z")));
    QVERIFY(f.controller.updateAvailable());
    QCOMPARE(becameAvailableSpy.count(), 1);

    // updateBecameAvailable is a transition (edge) signal, not a level
    // signal: it must not fire again while already available, even though
    // the reported version moved further ahead.
    f.controller.applyResult(okResult(evenNewer, QStringLiteral("2026-08-25T13:00:00Z")));
    QVERIFY(f.controller.updateAvailable());
    QCOMPARE(f.controller.latestVersion(), evenNewer);
    QCOMPARE(becameAvailableSpy.count(), 1);
}

QTEST_MAIN(UpdateCheckControllerTest)
#include "UpdateCheckControllerTest.moc"
