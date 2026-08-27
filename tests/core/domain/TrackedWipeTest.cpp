#include "domain/TrackedWipe.h"

#include "db/Database.h"
#include "domain/DevicePairing.h"
#include "domain/PairingStore.h"
#include "security/AppLockStore.h"
#include "security/DatabaseKeyStore.h"
#include "security/WipeTripwire.h"
#include "stores/CursorStore.h"
#include "stores/SecureStore.h"
#include "stores/SecureStoreFile.h"
#include "stores/SettingsStore.h"

#include <QFile>
#include <QHash>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

namespace {

// Removals silently fail -- a locked wallet, or no Secret Service provider.
// The wipe notices (LocalDataWipe reports pairingCleared/lockCleared false),
// which is what makes an INCOMPLETE wipe reachable in a test at all.
class UnremovableSecureStore : public SecureStore
{
public:
    bool set(const QString& key, const QString& value) override
    {
        m_values.insert(key, value);
        return true;
    }
    std::optional<QString> get(const QString& key) const override
    {
        const auto it = m_values.constFind(key);
        return it == m_values.constEnd() ? std::nullopt : std::optional<QString>(*it);
    }
    bool remove(const QString&) override { return false; }
    bool contains(const QString& key) const override { return m_values.contains(key); }

private:
    QHash<QString, QString> m_values;
};

// Everything LocalDataWipe needs, plus a seeded row so "the tables were
// wiped" is an observable change rather than a no-op.
struct WipeFixture
{
    QTemporaryDir dataDir;
    QTemporaryDir secureDir;
    QTemporaryDir settingsDir;
    Database database;
    std::unique_ptr<SecureStore> secureStore;
    std::unique_ptr<DatabaseKeyStore> databaseKeyStore;
    std::unique_ptr<PairingStore> pairingStore;
    std::unique_ptr<AppLockStore> appLockStore;
    std::unique_ptr<SettingsStore> settingsStore;
    std::unique_ptr<CursorStore> cursorStore;
    std::unique_ptr<LocalDataWipe> wipe;

    // `databasePath` empty keeps the session in memory, which is what main()
    // does under Hostile Location Protection. Passing a path is the ordinary
    // session, and the only shape in which "the wipe unlinked the file this
    // connection is bound to" is reachable at all.
    bool build(bool secureStoreRefusesRemovals, const QString& databasePath = QString())
    {
        if (!dataDir.isValid() || !secureDir.isValid() || !settingsDir.isValid())
            return false;
        if (!database.open(databasePath.isEmpty() ? QStringLiteral(":memory:") : databasePath))
            return false;

        QSqlQuery seed(database.handle());
        if (!seed.exec(QStringLiteral(
                "INSERT INTO emails (message_id, folder, at_utc) VALUES ('m1', 'INBOX', '2026-01-01')"))) {
            return false;
        }

        if (secureStoreRefusesRemovals)
            secureStore = std::make_unique<UnremovableSecureStore>();
        else
            secureStore = std::make_unique<SecureStoreFile>(secureDir.path());

        databaseKeyStore = std::make_unique<DatabaseKeyStore>(*secureStore);
        pairingStore = std::make_unique<PairingStore>(*secureStore);
        DevicePairing pairing;
        pairing.subscriberId = QStringLiteral("sub-1");
        pairing.deviceId = QStringLiteral("dev-1");
        pairing.deviceSecret = QStringLiteral("secret-1");
        if (!pairingStore->save(pairing))
            return false;

        appLockStore = std::make_unique<AppLockStore>(*secureStore);
        settingsStore = std::make_unique<SettingsStore>(settingsDir.filePath(QStringLiteral("settings.ini")));
        cursorStore = std::make_unique<CursorStore>(dataDir.filePath(QStringLiteral("cursors.ini")));
        wipe = std::make_unique<LocalDataWipe>(database, *databaseKeyStore, *pairingStore, *appLockStore,
                                                *settingsStore, *cursorStore, dataDir.path(), databasePath,
                                                QStringList{});
        return true;
    }

    bool emailsRemain()
    {
        QSqlQuery count(database.handle());
        if (!count.exec(QStringLiteral("SELECT COUNT(*) FROM emails")) || !count.next())
            return true; // cannot tell: report the unsafe answer
        return count.value(0).toInt() > 0;
    }
};

} // namespace

class TrackedWipeTest : public QObject
{
    Q_OBJECT

private slots:
    void aCompleteWipeLeavesNoMarkerBehind();
    void anIncompleteWipeKeepsTheMarkerArmed();
    void anInterruptedWipeIsFinishedOnTheNextLaunch();
    void nothingIsRetriedWhenNoWipeWasEverStarted();
    void aRecoveryThatStillFailsStaysArmed();
    void aFinishedRecoveryLeavesASessionWhoseWritesSurvive();
};

// The ordinary path: everything erased, marker gone, next launch has nothing
// to do.
void TrackedWipeTest::aCompleteWipeLeavesNoMarkerBehind()
{
    WipeFixture f;
    QVERIFY(f.build(/*secureStoreRefusesRemovals=*/false));

    WipeTripwire tripwire(f.dataDir.filePath(QStringLiteral("wipe-pending")));
    TrackedWipe tracked(tripwire, *f.wipe);

    const TrackedWipe::Outcome outcome = tracked.wipeEverything();

    QVERIFY(outcome.tripwireArmed);
    QVERIFY(outcome.erased());
    QVERIFY(outcome.tripwireDisarmed);
    QVERIFY(!tripwire.isArmed());
    QVERIFY(!f.emailsRemain());
}

// THE CASE THIS EXISTS FOR. The wipe runs, part of it fails, and the app
// relaunches anyway (correctly -- leaving the pre-wipe window open in front
// of whoever just failed ten PIN attempts would be worse). Before the
// tripwire the only record was a qCritical line in the journal, and the next
// launch came up looking perfectly ordinary on top of a pairing credential
// that was still in the keychain.
void TrackedWipeTest::anIncompleteWipeKeepsTheMarkerArmed()
{
    WipeFixture f;
    QVERIFY(f.build(/*secureStoreRefusesRemovals=*/true));

    WipeTripwire tripwire(f.dataDir.filePath(QStringLiteral("wipe-pending")));
    TrackedWipe tracked(tripwire, *f.wipe);

    const TrackedWipe::Outcome outcome = tracked.wipeEverything();

    QVERIFY(outcome.tripwireArmed);
    // The pairing credential could not be removed, so this is not a wipe.
    QVERIFY(!outcome.result.pairingCleared);
    QVERIFY(!outcome.erased());
    QVERIFY2(tripwire.isArmed(), "an incomplete wipe disarmed the tripwire and vanished without trace");
    QVERIFY(!outcome.tripwireDisarmed);
}

// A marker found at startup means the last wipe never reported completing --
// it failed, or the process died in the middle of it. Retrying is safe
// because every step of LocalDataWipe is idempotent.
void TrackedWipeTest::anInterruptedWipeIsFinishedOnTheNextLaunch()
{
    WipeFixture f;
    QVERIFY(f.build(/*secureStoreRefusesRemovals=*/false));

    // Arm it and erase nothing: the process died between the two.
    WipeTripwire tripwire(f.dataDir.filePath(QStringLiteral("wipe-pending")));
    QVERIFY(tripwire.arm());
    QVERIFY(f.emailsRemain());

    TrackedWipe tracked(tripwire, *f.wipe);
    const TrackedWipe::RecoveryOutcome recovery = tracked.recoverIfInterrupted();

    QVERIFY(recovery.wasInterrupted);
    QVERIFY(recovery.nowErased);
    QVERIFY2(!f.emailsRemain(), "the mail an interrupted wipe left behind survived the next launch");
    QVERIFY(!tripwire.isArmed());
}

void TrackedWipeTest::nothingIsRetriedWhenNoWipeWasEverStarted()
{
    WipeFixture f;
    QVERIFY(f.build(/*secureStoreRefusesRemovals=*/false));

    WipeTripwire tripwire(f.dataDir.filePath(QStringLiteral("wipe-pending")));
    TrackedWipe tracked(tripwire, *f.wipe);

    const TrackedWipe::RecoveryOutcome recovery = tracked.recoverIfInterrupted();

    QVERIFY(!recovery.wasInterrupted);
    QVERIFY(!recovery.nowErased);
    // An ordinary launch must not destroy the user's cached mail.
    QVERIFY2(f.emailsRemain(), "a launch with no armed tripwire wiped the profile anyway");
}

// The failure that broke the wipe is usually still there on the next launch
// -- a wallet that is still locked, a disk that is still read-only. Staying
// armed is what makes the launch after THAT try again.
void TrackedWipeTest::aRecoveryThatStillFailsStaysArmed()
{
    WipeFixture f;
    QVERIFY(f.build(/*secureStoreRefusesRemovals=*/true));

    WipeTripwire tripwire(f.dataDir.filePath(QStringLiteral("wipe-pending")));
    QVERIFY(tripwire.arm());

    TrackedWipe tracked(tripwire, *f.wipe);
    const TrackedWipe::RecoveryOutcome recovery = tracked.recoverIfInterrupted();

    QVERIFY(recovery.wasInterrupted);
    QVERIFY(!recovery.nowErased);
    QVERIFY(tripwire.isArmed());
    // What it COULD erase, it did: the retry is not all-or-nothing.
    QVERIFY(!f.emailsRemain());
}

// Recovery is the ONE wipeEverything() caller that does not relaunch: it runs
// at startup and returns straight into a full normal session. So the wipe's
// effect on the live database is load-bearing here in a way it is not for the
// ten-failed-PIN or account-replacement paths, both of which end in
// AppRelauncher::requestRelaunch().
//
// The failure this locks down: the wipe unlinks the database file and erases
// its key, and the session carries on through a connection whose file has no
// name. The user -- shown the pairing screen because the wipe cleared the
// pairing -- re-pairs and syncs, and that sync either fails at the first
// write or lands in an inode no later launch can open, while cursors.ini
// (erased by the same wipe, then repopulated by that sync) tells the next
// launch it is already up to date. The mail is then permanently absent
// locally with nothing left to force a resync.
//
// A SELECT through the old connection does NOT catch this -- it succeeds
// precisely because the unlinked inode is still alive. Writing and then
// reading back by NAME does.
void TrackedWipeTest::aFinishedRecoveryLeavesASessionWhoseWritesSurvive()
{
    WipeFixture f;
    QTemporaryDir profileDir;
    QVERIFY(profileDir.isValid());
    const QString dbPath = profileDir.filePath(QStringLiteral("kypost.db"));
    QVERIFY(f.build(/*secureStoreRefusesRemovals=*/false, dbPath));
    QVERIFY(!f.databaseKeyStore->create().isEmpty());

    WipeTripwire tripwire(f.dataDir.filePath(QStringLiteral("wipe-pending")));
    QVERIFY(tripwire.arm());

    TrackedWipe tracked(tripwire, *f.wipe);
    const TrackedWipe::RecoveryOutcome recovery = tracked.recoverIfInterrupted();

    QVERIFY(recovery.wasInterrupted);
    QVERIFY(recovery.nowErased);
    QVERIFY(!tripwire.isArmed());
    QVERIFY(!f.emailsRemain());

    // A session continues from here, so it needs a database it can actually
    // keep something in. Checked by writing and reading back rather than by
    // asking the wipe what it did -- the report is asserted afterwards.
    QSqlQuery insert(f.database.handle());
    QVERIFY(insert.exec(QStringLiteral("INSERT INTO emails (message_id, folder, at_utc) "
                                        "VALUES ('post-recovery', 'INBOX', '2026-01-01')")));

    const DatabaseKeyStore::Result key = f.databaseKeyStore->existing();
    Database nextLaunch;
    QVERIFY(nextLaunch.open(dbPath,
                             key.status == DatabaseKeyStore::Status::Found ? key.key : QByteArray()));
    QSqlQuery readBack(nextLaunch.handle());
    QVERIFY(readBack.exec(QStringLiteral("SELECT COUNT(*) FROM emails WHERE message_id = 'post-recovery'")));
    QVERIFY(readBack.next());
    QVERIFY2(readBack.value(0).toInt() == 1,
              "the session that finished an interrupted wipe was left writing to an unlinked file");

    QVERIFY(recovery.result.databaseReopenedAs.has_value());
    QVERIFY(*recovery.result.databaseReopenedAs != ProfileDatabaseMode::FailedToOpen);
}

QTEST_GUILESS_MAIN(TrackedWipeTest)
#include "TrackedWipeTest.moc"
