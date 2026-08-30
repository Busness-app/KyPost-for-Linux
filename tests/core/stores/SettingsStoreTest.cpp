#include "stores/SettingsStore.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

class SettingsStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAreUnset();
    void themeIdRoundTrips();
    void preferredModeRoundTrips();
    void pushDeliveryFieldsRoundTrip();
    void keywordVisibleDefaultsTrueUntilExplicitlyToggled();
    void keywordOrderRoundTrips();
    void aHostileLocationFlagThatCannotReachTheDiskIsReportedAsFailed();

private:
    QString tempFilePath(QTemporaryDir& dir, const QString& name) const;
};

QString SettingsStoreTest::tempFilePath(QTemporaryDir& dir, const QString& name) const
{
    return dir.filePath(name);
}

void SettingsStoreTest::defaultsAreUnset()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SettingsStore store(tempFilePath(dir, QStringLiteral("settings.ini")));

    QCOMPARE(store.themeId(), QStringLiteral("Patina Ky"));
}

void SettingsStoreTest::themeIdRoundTrips()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SettingsStore store(tempFilePath(dir, QStringLiteral("settings.ini")));

    store.setThemeId(QStringLiteral("Solar Flare"));
    QCOMPARE(store.themeId(), QStringLiteral("Solar Flare"));
}

void SettingsStoreTest::preferredModeRoundTrips()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SettingsStore store(tempFilePath(dir, QStringLiteral("settings.ini")));

    QVERIFY(store.setPreferredMode(QStringLiteral("mobile")));
    QCOMPARE(store.preferredMode(), QStringLiteral("mobile"));
}

void SettingsStoreTest::pushDeliveryFieldsRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SettingsStore store(tempFilePath(dir, QStringLiteral("settings.ini")));

    QVERIFY(store.deliveryMode().isEmpty());
    QVERIFY(store.pullEndpoint().isEmpty());
    QVERIFY(store.transport().isEmpty());

    store.setDeliveryMode(QStringLiteral("pull"));
    store.setPullEndpoint(QStringLiteral("http://relay.example/api/notifications/native/pull"));
    store.setTransport(QStringLiteral("unifiedpush"));

    QCOMPARE(store.deliveryMode(), QStringLiteral("pull"));
    QCOMPARE(store.pullEndpoint(), QStringLiteral("http://relay.example/api/notifications/native/pull"));
    QCOMPARE(store.transport(), QStringLiteral("unifiedpush"));
}

void SettingsStoreTest::keywordVisibleDefaultsTrueUntilExplicitlyToggled()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SettingsStore store(tempFilePath(dir, QStringLiteral("settings.ini")));

    QCOMPARE(store.keywordVisible(QStringLiteral("Work")), true);

    store.setKeywordVisible(QStringLiteral("Work"), false);
    QCOMPARE(store.keywordVisible(QStringLiteral("Work")), false);
    // An unrelated keyword that was never toggled stays visible.
    QCOMPARE(store.keywordVisible(QStringLiteral("Personal")), true);

    store.setKeywordVisible(QStringLiteral("Work"), true);
    QCOMPARE(store.keywordVisible(QStringLiteral("Work")), true);
}

void SettingsStoreTest::keywordOrderRoundTrips()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = tempFilePath(dir, QStringLiteral("settings.ini"));

    SettingsStore store(path);
    QVERIFY(store.keywordOrder().isEmpty());
    store.setKeywordOrder({ QStringLiteral("Urgent"), QStringLiteral("Work") });

    SettingsStore reloaded(path);
    QCOMPARE(reloaded.keywordOrder(), QStringList({ QStringLiteral("Urgent"), QStringLiteral("Work") }));
}

// The flag decides, on the NEXT launch, whether the database opens ":memory:"
// or the real file on disk -- and AppLockManager erases the profile and
// relaunches on the strength of it. A write that never reached the disk has to
// say so, or the replacement process comes up unprotected over mail the erase
// already destroyed. Same rule, same mechanism as CursorStore::flush().
void SettingsStoreTest::aHostileLocationFlagThatCannotReachTheDiskIsReportedAsFailed()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // A directory where the .ini is meant to be: QSettings can neither read
    // nor write it, and reports AccessError from sync().
    const QString blocked = tempFilePath(dir, QStringLiteral("settings.ini"));
    QVERIFY(QDir().mkpath(blocked));

    SettingsStore store(blocked);
    QVERIFY(!store.setHostileLocationProtectionEnabled(true));

    // And nothing reached the disk, which is what the false was about.
    // Checked against the filesystem rather than a second SettingsStore over
    // the same path: QSettings shares one in-memory QConfFile per file name
    // within a process, so a second store would answer from the same unwritten
    // copy and prove nothing about the next launch.
    QVERIFY2(QDir(blocked).isEmpty(), "something was written where the settings file could not go");
}

QTEST_GUILESS_MAIN(SettingsStoreTest)
#include "SettingsStoreTest.moc"
