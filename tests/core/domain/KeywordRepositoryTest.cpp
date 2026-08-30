#include "domain/KeywordRepository.h"

#include "models/Email.h"
#include "stores/SettingsStore.h"

#include <QTemporaryDir>
#include <QTest>

class KeywordRepositoryTest : public QObject
{
    Q_OBJECT

private slots:
    void computeTabsOrdersAlphabeticallyCaseInsensitiveWithPerKeywordCounts();
    void visibleTabsExcludesExplicitlyHiddenKeywordButKeepsNeverToggledOnes();
    void allSettingsUsesSavedOrderAndAppendsNewKeywords();

private:
    static Email emailWithKeywords(const QString& messageId, const QStringList& keywords);
};

Email KeywordRepositoryTest::emailWithKeywords(const QString& messageId, const QStringList& keywords)
{
    Email email;
    email.messageId = messageId;
    email.keywords = keywords;
    return email;
}

void KeywordRepositoryTest::computeTabsOrdersAlphabeticallyCaseInsensitiveWithPerKeywordCounts()
{
    const QVector<Email> emails = {
        emailWithKeywords(QStringLiteral("m1"), { QStringLiteral("work"), QStringLiteral("Urgent") }),
        emailWithKeywords(QStringLiteral("m2"), { QStringLiteral("work") }),
        emailWithKeywords(QStringLiteral("m3"), { QStringLiteral("archive") }),
    };

    const QVector<KeywordTab> tabs = KeywordRepository::computeTabs(emails);

    QCOMPARE(tabs.size(), 3);
    // Alphabetical, case-insensitive: archive, Urgent, work.
    QCOMPARE(tabs.at(0).name, QStringLiteral("archive"));
    QCOMPARE(tabs.at(0).count, 1);
    QCOMPARE(tabs.at(1).name, QStringLiteral("Urgent"));
    QCOMPARE(tabs.at(1).count, 1);
    // "work" appears on two separate emails -- counted twice, not
    // deduplicated down to a single count across emails.
    QCOMPARE(tabs.at(2).name, QStringLiteral("work"));
    QCOMPARE(tabs.at(2).count, 2);
}

void KeywordRepositoryTest::visibleTabsExcludesExplicitlyHiddenKeywordButKeepsNeverToggledOnes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    KeywordRepository repository(settingsStore);

    const QVector<Email> emails = {
        emailWithKeywords(QStringLiteral("m1"), { QStringLiteral("Work"), QStringLiteral("Personal") }),
    };

    repository.setVisible(QStringLiteral("Work"), false);

    const QVector<KeywordTab> visible = repository.visibleTabs(emails);
    QCOMPARE(visible.size(), 1);
    QCOMPARE(visible.at(0).name, QStringLiteral("Personal"));

    const QVector<KeywordSettings> all = repository.allSettings(emails);
    QCOMPARE(all.size(), 2);
    for (const KeywordSettings& setting : all) {
        if (setting.keyword == QStringLiteral("Work"))
            QCOMPARE(setting.visible, false);
        else
            QCOMPARE(setting.visible, true);
    }
}

void KeywordRepositoryTest::allSettingsUsesSavedOrderAndAppendsNewKeywords()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SettingsStore settingsStore(dir.filePath(QStringLiteral("settings.ini")));
    KeywordRepository repository(settingsStore);
    repository.setOrder({ QStringLiteral("Work"), QStringLiteral("Missing"),
                          QStringLiteral("Work"), QStringLiteral("Personal") });

    const QVector<Email> emails = {
        emailWithKeywords(QStringLiteral("m1"), { QStringLiteral("Urgent"), QStringLiteral("Personal") }),
        emailWithKeywords(QStringLiteral("m2"), { QStringLiteral("Work") }),
    };
    const QVector<KeywordSettings> settings = repository.allSettings(emails);

    QCOMPARE(settings.size(), 3);
    QCOMPARE(settings.at(0).keyword, QStringLiteral("Work"));
    QCOMPARE(settings.at(1).keyword, QStringLiteral("Personal"));
    QCOMPARE(settings.at(2).keyword, QStringLiteral("Urgent"));

    const QVector<KeywordTab> visible = repository.visibleTabs(emails);
    QCOMPARE(visible.at(0).name, QStringLiteral("Work"));
    QCOMPARE(visible.at(1).name, QStringLiteral("Personal"));
    QCOMPARE(visible.at(2).name, QStringLiteral("Urgent"));
}

QTEST_GUILESS_MAIN(KeywordRepositoryTest)
#include "KeywordRepositoryTest.moc"
