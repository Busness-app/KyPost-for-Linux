#include "net/FolderClient.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include "FakeRelayServer.h"

#include <QNetworkAccessManager>
#include <QTest>

class FolderClientTest : public QObject
{
    Q_OBJECT

private slots:
    void listParsesFoldersAndStampsParent();
    void listSendsParentQueryEvenWhenEmpty();
    void listReportsHttpErrors();
    void createPostsParentAndName();
    void renameNormalisesRenamedIntoFolder();
    void removeSendsFolderAsQueryParam();
    void mutationSurfacesPlainTextErrorBody();

private:
    static QUrl baseUrl(quint16 port) { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port)); }
    static RelayAuth auth() { return RelayAuth{ QStringLiteral("dev-1"), QStringLiteral("secret-1") }; }
};

void FolderClientTest::listParsesFoldersAndStampsParent()
{
    const QByteArray body = R"({
      "parent": "Archive",
      "folders": [
        {"path": "Archive/2026", "deletable": true},
        {"path": "Archive/2025", "deletable": true},
        {"path": "", "deletable": true}
      ]
    })";
    FakeRelayServer fake(httpResponse(200, "OK", body));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    FolderClient client(http);

    const FolderListResult result = client.list(baseUrl(fake.port()), auth(), QStringLiteral("Archive"));

    QVERIFY(!result.error.has_value());
    QCOMPARE(result.parent, QStringLiteral("Archive"));
    // The empty-path entry is dropped rather than cached as a blank row.
    QCOMPARE(result.folders.size(), 2);
    QCOMPARE(result.folders.at(0).path, QStringLiteral("Archive/2026"));
    QVERIFY(result.folders.at(0).deletable);
    // parent is not per-folder on the wire; it must be stamped from the
    // response's echoed parent or FolderDao rows aren't addressable.
    QCOMPARE(result.folders.at(0).parent, QStringLiteral("Archive"));
    QCOMPARE(result.folders.at(1).parent, QStringLiteral("Archive"));

    QVERIFY(fake.receivedRequest().contains("GET /api/inbox/folders?parent=Archive"));
    QVERIFY(fake.receivedRequest().contains("X-Kypost-Device-Id: dev-1"));
}

void FolderClientTest::listSendsParentQueryEvenWhenEmpty()
{
    FakeRelayServer fake(httpResponse(200, "OK", R"({"parent":"","folders":[]})"));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    FolderClient client(http);

    const FolderListResult result = client.list(baseUrl(fake.port()), auth(), QString());
    QVERIFY(!result.error.has_value());
    QVERIFY(result.folders.isEmpty());
    // Qt normalises an empty query value to a bare `?parent`, dropping the
    // `=`. Go's url.ParseQuery reads that as parent="" -- the same thing
    // handleInboxFolders' strings.TrimSpace(Get("parent")) sees for the top
    // level -- so this is correct, just not the spelling you'd predict.
    QVERIFY(fake.receivedRequest().startsWith("GET /api/inbox/folders?parent"));
}

void FolderClientTest::listReportsHttpErrors()
{
    FakeRelayServer fake(httpResponse(401, "Unauthorized", "unauthorized", "text/plain"));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    FolderClient client(http);

    const FolderListResult result = client.list(baseUrl(fake.port()), auth(), QStringLiteral("Archive"));
    QCOMPARE(result.error, std::optional<NetworkError>(NetworkError::Unauthorized));
    QVERIFY(result.folders.isEmpty());
}

void FolderClientTest::createPostsParentAndName()
{
    FakeRelayServer fake(httpResponse(
        200, "OK", R"({"ok":true,"parent":"Archive","name":"2026","folder":"Archive/2026"})"));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    FolderClient client(http);

    const FolderMutationResult result =
        client.create(baseUrl(fake.port()), auth(), QStringLiteral("Archive"), QStringLiteral("2026"));

    QVERIFY(!result.error.has_value());
    QVERIFY(result.ok);
    QCOMPARE(result.folder, QStringLiteral("Archive/2026"));
    QCOMPARE(result.parent, QStringLiteral("Archive"));

    QVERIFY(fake.receivedRequest().startsWith("POST /api/inbox/folders"));
    QCOMPARE(fake.receivedJsonBody().value(QStringLiteral("parent")).toString(), QStringLiteral("Archive"));
    QCOMPARE(fake.receivedJsonBody().value(QStringLiteral("name")).toString(), QStringLiteral("2026"));
}

void FolderClientTest::renameNormalisesRenamedIntoFolder()
{
    // Rename answers with `renamed`, create/delete with `folder`. The client
    // normalises both onto one field so callers don't branch on the verb.
    FakeRelayServer fake(httpResponse(
        200, "OK",
        R"({"ok":true,"folder":"Archive/old","renamed":"Archive/new","parent":"Archive"})"));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    FolderClient client(http);

    const FolderMutationResult result =
        client.rename(baseUrl(fake.port()), auth(), QStringLiteral("Archive/old"), QStringLiteral("new"));

    QVERIFY(!result.error.has_value());
    QCOMPARE(result.folder, QStringLiteral("Archive/new"));
    QCOMPARE(result.parent, QStringLiteral("Archive"));
    QVERIFY(fake.receivedRequest().startsWith("PUT /api/inbox/folders"));
    QCOMPARE(fake.receivedJsonBody().value(QStringLiteral("folder")).toString(), QStringLiteral("Archive/old"));
}

void FolderClientTest::removeSendsFolderAsQueryParam()
{
    FakeRelayServer fake(
        httpResponse(200, "OK", R"({"ok":true,"folder":"Archive/2026","parent":"Archive"})"));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    FolderClient client(http);

    const FolderMutationResult result =
        client.remove(baseUrl(fake.port()), auth(), QStringLiteral("Archive/2026"));

    QVERIFY(!result.error.has_value());
    QCOMPARE(result.folder, QStringLiteral("Archive/2026"));
    // DELETE takes ?folder=, not a body -- the backend reads the query only.
    QVERIFY(fake.receivedRequest().startsWith("DELETE /api/inbox/folders?folder=Archive"));
}

void FolderClientTest::mutationSurfacesPlainTextErrorBody()
{
    // These refusals arrive via http.Error as plain text, not JSON, and the
    // body is already the message worth showing the user.
    FakeRelayServer fake(
        httpResponse(400, "Bad Request", "built-in folders cannot be renamed", "text/plain"));

    QNetworkAccessManager manager;
    HttpClient http(manager);
    FolderClient client(http);

    const FolderMutationResult result =
        client.rename(baseUrl(fake.port()), auth(), QStringLiteral("INBOX"), QStringLiteral("nope"));

    QVERIFY(result.error.has_value());
    QVERIFY(!result.ok);
    QCOMPARE(result.detail, QStringLiteral("built-in folders cannot be renamed"));
}

QTEST_GUILESS_MAIN(FolderClientTest)
#include "FolderClientTest.moc"
