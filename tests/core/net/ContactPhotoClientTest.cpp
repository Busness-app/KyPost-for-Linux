#include "net/ContactPhotoClient.h"

#include "net/HttpClient.h"
#include "net/RelayAuth.h"

#include "FakeRelayServer.h"

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTest>

class ContactPhotoClientTest : public QObject
{
    Q_OBJECT

private slots:
    void fetchReturnsRawBytesOnSuccess();
    void fetchSendsAuthAsHeadersAndBuildsPathWithContactUid();
    void fetchUnauthorizedFrom401DegradesGracefullyToEmptyResult();
    void fetchOnTransportFailureDegradesGracefullyToEmptyResult();
    void aPhotoLargerThanAnAvatarIsRefusedWellBeforeTheGenericCeiling();
    void aUidCarryingPathSyntaxCannotReachAnotherEndpoint();
    void aUidCarryingPathSyntaxCannotReachAnotherEndpoint_data();
    void aUidThatIsNothingButADotSegmentSendsNoRequestAtAll();
    void aUidThatIsNothingButADotSegmentSendsNoRequestAtAll_data();
};

void ContactPhotoClientTest::fetchReturnsRawBytesOnSuccess()
{
    const QByteArray photoBytes = QByteArrayLiteral("\xFF\xD8\xFF\xE0not-really-a-jpeg-but-bytes-are-bytes");
    FakeRelayServer fake(httpResponse(200, "OK", photoBytes, "image/jpeg"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactPhotoClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const ContactPhotoFetchResult result = client.fetch(serverBaseUrl, QStringLiteral("contact-1"), auth);

    QVERIFY(!result.error.has_value());
    QCOMPARE(result.photoBytes, photoBytes);
}

void ContactPhotoClientTest::fetchSendsAuthAsHeadersAndBuildsPathWithContactUid()
{
    FakeRelayServer fake(httpResponse(200, "OK", ""));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactPhotoClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-9"), QStringLiteral("secret-9") };
    client.fetch(serverBaseUrl, QStringLiteral("contact-42"), auth);

    const QByteArray request = fake.receivedRequest();
    QVERIFY(request.contains("GET /api/contacts/contact-42/photo HTTP/1.1"));
    QVERIFY(request.contains("X-Kypost-Device-Id: device-9"));
    QVERIFY(request.contains("X-Kypost-Device-Secret: secret-9"));
    QVERIFY(!request.contains("device=device-9"));
    QVERIFY(!request.contains("secret=secret-9"));
}

void ContactPhotoClientTest::fetchUnauthorizedFrom401DegradesGracefullyToEmptyResult()
{
    // Global Constraint (task-3-brief.md, same wording as task-2-brief.md's
    // rule for GroupsClient): ContactPhotoClient must degrade gracefully on
    // 401/error -- empty photoBytes, error set, never a crash -- since the
    // backend endpoint this depends on is an unverified external dependency
    // that may not be deployed.
    FakeRelayServer fake(httpResponse(401, "Unauthorized", "Unauthorized\n"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactPhotoClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const ContactPhotoFetchResult result = client.fetch(serverBaseUrl, QStringLiteral("contact-1"), auth);

    QVERIFY(result.error.has_value());
    QCOMPARE(*result.error, NetworkError::Unauthorized);
    QVERIFY(result.photoBytes.isEmpty());
}

void ContactPhotoClientTest::fetchOnTransportFailureDegradesGracefullyToEmptyResult()
{
    // Grab an ephemeral port, then close the listener immediately so
    // nothing is listening on it when the client connects -- same approach
    // as HttpClientTest::transportFailureWhenNothingListens(). Real
    // connection-refused transport failure, same "never crash"
    // degrade-gracefully requirement as the 401 case above.
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost));
    const quint16 freePort = probe.serverPort();
    probe.close();

    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactPhotoClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(freePort));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const ContactPhotoFetchResult result = client.fetch(serverBaseUrl, QStringLiteral("contact-1"), auth);

    QVERIFY(result.error.has_value());
    QCOMPARE(*result.error, NetworkError::Transport);
    QVERIFY(result.photoBytes.isEmpty());
}

// An avatar is not eight megabytes. On HttpClient's generic default ceiling,
// every contact viewed could allocate 8 MiB and -- via ContactPhotoCache --
// write that much to the user's disk, so this route names its own much lower
// bound at the call site.
void ContactPhotoClientTest::aPhotoLargerThanAnAvatarIsRefusedWellBeforeTheGenericCeiling()
{
    const QByteArray oversized(ContactPhotoClient::kMaxPhotoBytes + 1, 'x');
    QVERIFY2(oversized.size() < HttpClient::kMaxResponseBytes,
             "the payload has to be one the generic ceiling would have allowed");

    FakeRelayServer fake(httpResponse(200, "OK", oversized, "image/jpeg"));
    QNetworkAccessManager manager;
    HttpClient http(manager); // the DEFAULT ceiling, which this body fits under
    ContactPhotoClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-1"), QStringLiteral("secret-1") };
    const ContactPhotoFetchResult result = client.fetch(serverBaseUrl, QStringLiteral("contact-1"), auth);

    QVERIFY(result.error.has_value());
    QCOMPARE(*result.error, NetworkError::ResponseTooLarge);
    QVERIFY(result.photoBytes.isEmpty());
}

// The device secret rides on this request as a header. The uid is the only
// runtime value this repo puts in a URL path, and it arrives from contact
// sync or a vCard import -- neither of which this client controls. Raw
// concatenation let a uid of "../../api/notifications/native/pull" resolve to
// a different authenticated endpoint on the same origin, so the interesting
// assertion is not "the request failed", it is "the request line still names
// /api/contacts/<something>/photo and nothing else".
void ContactPhotoClientTest::aUidCarryingPathSyntaxCannotReachAnotherEndpoint_data()
{
    QTest::addColumn<QString>("uid");
    QTest::addColumn<QByteArray>("expectedTarget");

    QTest::newRow("traversal")
        << QStringLiteral("../../api/notifications/native/pull")
        << QByteArray("/api/contacts/..%2F..%2Fapi%2Fnotifications%2Fnative%2Fpull/photo");
    QTest::newRow("extra segment") << QStringLiteral("a/b") << QByteArray("/api/contacts/a%2Fb/photo");
    QTest::newRow("leading slash") << QStringLiteral("/etc/passwd")
                                   << QByteArray("/api/contacts/%2Fetc%2Fpasswd/photo");
    // Already-encoded input must be encoded AGAIN, not passed through -- a
    // server that decodes once would otherwise see a bare slash.
    QTest::newRow("pre-encoded slash") << QStringLiteral("a%2fb")
                                        << QByteArray("/api/contacts/a%252fb/photo");
    QTest::newRow("query") << QStringLiteral("a?x=1") << QByteArray("/api/contacts/a%3Fx%3D1/photo");
    QTest::newRow("fragment") << QStringLiteral("a#f") << QByteArray("/api/contacts/a%23f/photo");
    QTest::newRow("space") << QStringLiteral("a b") << QByteArray("/api/contacts/a%20b/photo");
    QTest::newRow("crlf injection") << QStringLiteral("a\r\nX-Evil: 1")
                                     << QByteArray("/api/contacts/a%0D%0AX-Evil%3A%201/photo");
    QTest::newRow("dot inside a longer uid")
        << QStringLiteral("..contact") << QByteArray("/api/contacts/..contact/photo");
}

void ContactPhotoClientTest::aUidCarryingPathSyntaxCannotReachAnotherEndpoint()
{
    QFETCH(QString, uid);
    QFETCH(QByteArray, expectedTarget);

    FakeRelayServer fake(httpResponse(200, "OK", ""));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactPhotoClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-9"), QStringLiteral("secret-9") };
    client.fetch(serverBaseUrl, uid, auth);

    const QByteArray request = fake.receivedRequest();
    QVERIFY2(request.contains("GET " + expectedTarget + " HTTP/1.1"),
             ("request line was: " + request.left(request.indexOf('\r'))).constData());
    // The uid is confined to one segment: nothing it contained added a
    // segment of its own, so the path is exactly three deep.
    QVERIFY(!request.contains("GET /api/notifications"));
    QVERIFY(request.contains("X-Kypost-Device-Secret: secret-9"));
    // ...and it did not smuggle a header of its own past the request line.
    // The substring "X-Evil" IS in the request -- percent-encoded, inside the
    // path -- so the assertion has to be about a header LINE, not a substring.
    QVERIFY(!request.contains("\r\nX-Evil:"));
}

// "." and ".." survive percent-encoding untouched -- they contain no
// character an encoder acts on -- so encoding alone cannot stop them
// resolving away from /api/contacts. They are refused before the socket is
// opened, which is also the only outcome that keeps the device secret at
// home. The empty uid is here for the same reason: it produces "//" and a
// path one segment short of the endpoint.
void ContactPhotoClientTest::aUidThatIsNothingButADotSegmentSendsNoRequestAtAll_data()
{
    QTest::addColumn<QString>("uid");
    QTest::newRow("empty") << QString();
    QTest::newRow("dot") << QStringLiteral(".");
    QTest::newRow("dot dot") << QStringLiteral("..");
}

void ContactPhotoClientTest::aUidThatIsNothingButADotSegmentSendsNoRequestAtAll()
{
    QFETCH(QString, uid);

    FakeRelayServer fake(httpResponse(200, "OK", "photo-bytes"));
    QNetworkAccessManager manager;
    HttpClient http(manager);
    ContactPhotoClient client(http);

    const QUrl serverBaseUrl(QStringLiteral("http://127.0.0.1:%1").arg(fake.port()));
    const RelayAuth auth{ QStringLiteral("device-9"), QStringLiteral("secret-9") };
    const ContactPhotoFetchResult result = client.fetch(serverBaseUrl, uid, auth);

    QVERIFY(result.error.has_value());
    QCOMPARE(*result.error, NetworkError::InvalidUrl);
    QVERIFY(result.photoBytes.isEmpty());
    // Nothing was sent, so nothing was received -- the credentials never left.
    QVERIFY(fake.receivedRequest().isEmpty());
}

QTEST_GUILESS_MAIN(ContactPhotoClientTest)
#include "ContactPhotoClientTest.moc"
