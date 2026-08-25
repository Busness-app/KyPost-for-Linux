#include "version/VersionCompare.h"

#include <QTest>

class VersionCompareTest : public QObject
{
    Q_OBJECT

private slots:
    void comparesDottedNumericVersions_data();
    void comparesDottedNumericVersions();
    void refusesAnythingThatIsNotThreeNumbers_data();
    void refusesAnythingThatIsNotThreeNumbers();
};

void VersionCompareTest::comparesDottedNumericVersions_data()
{
    QTest::addColumn<QString>("latest");
    QTest::addColumn<QString>("installed");
    QTest::addColumn<bool>("expected");

    QTest::newRow("newer patch") << "0.2.1" << "0.2.0" << true;
    QTest::newRow("newer minor") << "0.3.0" << "0.2.9" << true;
    QTest::newRow("newer major") << "1.0.0" << "0.99.99" << true;
    QTest::newRow("equal") << "0.2.0" << "0.2.0" << false;
    QTest::newRow("older") << "0.1.0" << "0.2.0" << false;
    // Numeric, not lexicographic: "10" sorts before "9" as text.
    QTest::newRow("ten beats nine numerically") << "0.10.0" << "0.9.0" << true;
    // The tags carry a leading v and KYPOST_VERSION does not. Both sides have
    // to reach the same form or every comparison is against a malformed
    // string.
    QTest::newRow("leading v on latest") << "v0.3.0" << "0.2.0" << true;
    QTest::newRow("leading v on both") << "v0.3.0" << "v0.2.0" << true;
}

void VersionCompareTest::comparesDottedNumericVersions()
{
    QFETCH(QString, latest);
    QFETCH(QString, installed);
    QFETCH(bool, expected);
    QCOMPARE(VersionCompare::isNewer(latest, installed), expected);
}

void VersionCompareTest::refusesAnythingThatIsNotThreeNumbers_data()
{
    QTest::addColumn<QString>("latest");
    QTest::addColumn<QString>("installed");

    // v0.1-alpha is in this repository's tag list today. Parsing it
    // best-effort as 0.1.0 would be a silent wrong answer; refusing it is a
    // visible "no information".
    QTest::newRow("alpha tag") << "v0.1-alpha" << "0.2.0";
    QTest::newRow("two components") << "0.3" << "0.2.0";
    QTest::newRow("four components") << "0.3.0.1" << "0.2.0";
    QTest::newRow("non-numeric component") << "0.x.0" << "0.2.0";
    QTest::newRow("empty latest") << "" << "0.2.0";
    QTest::newRow("empty installed") << "0.3.0" << "";
    QTest::newRow("malformed installed") << "0.3.0" << "not-a-version";
    QTest::newRow("negative") << "-1.0.0" << "0.2.0";
    QTest::newRow("trailing suffix") << "0.3.0-rc1" << "0.2.0";
}

void VersionCompareTest::refusesAnythingThatIsNotThreeNumbers()
{
    QFETCH(QString, latest);
    QFETCH(QString, installed);
    // Refusing means "no update", never a crash and never a true.
    QCOMPARE(VersionCompare::isNewer(latest, installed), false);
}

QTEST_MAIN(VersionCompareTest)
#include "VersionCompareTest.moc"
