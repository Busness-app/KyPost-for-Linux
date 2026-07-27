#include "pgp/PgpQrTargetValidator.h"

#include <QTest>

// Covers isSafeQrTarget() in isolation, with an injected HostResolver so the
// DNS-based link-local bypass fix (VibeSec finding) is testable without
// depending on real DNS resolution.
class PgpQrTargetValidatorTest : public QObject
{
    Q_OBJECT

private slots:
    void rejectsNonHttpScheme();
    void rejectsEmptyHost();
    void rejectsMetadataGoogleInternalHostnameWithoutResolving();
    void rejectsLiteralLinkLocalIpViaDefaultResolver();
    void rejectsHostnameThatResolvesToLinkLocalAddress();
    void allowsHostnameThatResolvesToPublicAddress();
    void allowsHostnameThatResolvesToLoopbackAddress();
    void rejectsUnresolvableHostname();

    // Review-finding regressions.
    void rejectsPrivateAndUniqueLocalAddresses();
    void rejectsCleartextForAnythingButLoopback();
    void rejectsHostWithAMixOfPublicAndPrivateAddresses();
};

void PgpQrTargetValidatorTest::rejectsNonHttpScheme()
{
    // file:// must never reach a resolver at all -- reading local files
    // back as if they were key material is the risk this blocks.
    bool resolverCalled = false;
    const HostResolver resolver = [&resolverCalled](const QString&) {
        resolverCalled = true;
        return QList<QHostAddress>{ QHostAddress(QStringLiteral("203.0.113.10")) };
    };

    QVERIFY(!isSafeQrTarget(QUrl(QStringLiteral("file:///etc/passwd")), resolver));
    QVERIFY(!resolverCalled);
}

void PgpQrTargetValidatorTest::rejectsEmptyHost()
{
    QVERIFY(!isSafeQrTarget(QUrl(QStringLiteral("http:///api/pgp/qr/key"))));
}

void PgpQrTargetValidatorTest::rejectsMetadataGoogleInternalHostnameWithoutResolving()
{
    bool resolverCalled = false;
    const HostResolver resolver = [&resolverCalled](const QString&) {
        resolverCalled = true;
        return QList<QHostAddress>{ QHostAddress(QStringLiteral("203.0.113.10")) };
    };

    QVERIFY(!isSafeQrTarget(QUrl(QStringLiteral("http://metadata.google.internal/api/pgp/qr/key")), resolver));
    QVERIFY(!resolverCalled);
}

void PgpQrTargetValidatorTest::rejectsLiteralLinkLocalIpViaDefaultResolver()
{
    // No injected resolver -- exercises the real default resolver's literal
    // IP fast path (no DNS query needed).
    QVERIFY(!isSafeQrTarget(QUrl(QStringLiteral("http://169.254.169.254/api/pgp/qr/key"))));
}

void PgpQrTargetValidatorTest::rejectsHostnameThatResolvesToLinkLocalAddress()
{
    // VibeSec regression guard: this is the actual bypass -- an ordinary
    // hostname (not a literal IP) whose DNS record points at a link-local/
    // cloud-metadata address used to sail straight through, since the old
    // check only ran QHostAddress::setAddress() on the literal QR text.
    const HostResolver resolver = [](const QString& host) -> QList<QHostAddress> {
        if (host == QStringLiteral("attacker-domain.example"))
            return { QHostAddress(QStringLiteral("169.254.169.254")) };
        return {};
    };

    QVERIFY(!isSafeQrTarget(QUrl(QStringLiteral("http://attacker-domain.example/api/pgp/qr/key")), resolver));
}

void PgpQrTargetValidatorTest::allowsHostnameThatResolvesToPublicAddress()
{
    const HostResolver resolver = [](const QString& host) -> QList<QHostAddress> {
        if (host == QStringLiteral("relay.example"))
            return { QHostAddress(QStringLiteral("203.0.113.10")) };
        return {};
    };

    QVERIFY(isSafeQrTarget(QUrl(QStringLiteral("https://relay.example/api/pgp/qr/key")), resolver));
}

void PgpQrTargetValidatorTest::allowsHostnameThatResolvesToLoopbackAddress()
{
    // Self-hosted relays commonly live on localhost -- must stay allowed.
    const HostResolver resolver = [](const QString& host) -> QList<QHostAddress> {
        if (host == QStringLiteral("my-relay.localdomain"))
            return { QHostAddress(QStringLiteral("127.0.0.1")) };
        return {};
    };

    QVERIFY(isSafeQrTarget(QUrl(QStringLiteral("http://my-relay.localdomain/api/pgp/qr/key")), resolver));
}

void PgpQrTargetValidatorTest::rejectsUnresolvableHostname()
{
    // Fail closed: if the safety of a host can't be verified, don't
    // proceed.
    const HostResolver resolver = [](const QString&) { return QList<QHostAddress>{}; };

    QVERIFY(!isSafeQrTarget(QUrl(QStringLiteral("http://does-not-resolve.example/api/pgp/qr/key")), resolver));
}

void PgpQrTargetValidatorTest::rejectsPrivateAndUniqueLocalAddresses()
{
    // A QR code is a picture an attacker can print on a poster or paste into
    // a message. Blocking only link-local left the whole of the user's own
    // LAN reachable -- a router admin page, a printer, an internal service --
    // with whatever answered rendered back as a contact's PGP key.
    const auto resolvesTo = [](const QString& literal) {
        return HostResolver([literal](const QString&) {
            return QList<QHostAddress>{ QHostAddress(literal) };
        });
    };

    QVERIFY(!isSafeQrTarget(QUrl(QStringLiteral("https://scan.example/api/pgp/qr/key")),
                             resolvesTo(QStringLiteral("192.168.1.1"))));
    QVERIFY(!isSafeQrTarget(QUrl(QStringLiteral("https://scan.example/api/pgp/qr/key")),
                             resolvesTo(QStringLiteral("10.0.0.5"))));
    QVERIFY(!isSafeQrTarget(QUrl(QStringLiteral("https://scan.example/api/pgp/qr/key")),
                             resolvesTo(QStringLiteral("172.16.4.9"))));
    QVERIFY(!isSafeQrTarget(QUrl(QStringLiteral("https://scan.example/api/pgp/qr/key")),
                             resolvesTo(QStringLiteral("fd00::1"))));
}

void PgpQrTargetValidatorTest::rejectsCleartextForAnythingButLoopback()
{
    const HostResolver public_ = [](const QString&) {
        return QList<QHostAddress>{ QHostAddress(QStringLiteral("203.0.113.10")) };
    };

    // Cleartext lets an on-path attacker swap the public key the user is
    // about to trust. It also leaves DNS rebinding open: this validator
    // resolves the host, Qt resolves it again when connecting, and an
    // attacker-controlled nameserver can answer differently the second time.
    // Requiring TLS means a rebound address must present a certificate valid
    // for the QR's hostname, which a metadata service or LAN device cannot.
    QVERIFY(!isSafeQrTarget(QUrl(QStringLiteral("http://scan.example/api/pgp/qr/key")), public_));
    QVERIFY(isSafeQrTarget(QUrl(QStringLiteral("https://scan.example/api/pgp/qr/key")), public_));

    // Loopback keeps its cleartext exemption: self-hosted relays on
    // localhost are supported, and rebinding a name that already resolves to
    // 127.0.0.1 buys an attacker nothing they could not do by writing the
    // literal into the QR.
    const HostResolver loopback = [](const QString&) {
        return QList<QHostAddress>{ QHostAddress(QStringLiteral("127.0.0.1")) };
    };
    QVERIFY(isSafeQrTarget(QUrl(QStringLiteral("http://my-relay.localdomain/api/pgp/qr/key")), loopback));
}

void PgpQrTargetValidatorTest::rejectsHostWithAMixOfPublicAndPrivateAddresses()
{
    // Every resolved address has to pass, not just the first one -- a
    // multi-A-record host is otherwise a trivial way to smuggle one.
    const HostResolver mixed = [](const QString&) {
        return QList<QHostAddress>{ QHostAddress(QStringLiteral("203.0.113.10")),
                                     QHostAddress(QStringLiteral("192.168.0.7")) };
    };
    QVERIFY(!isSafeQrTarget(QUrl(QStringLiteral("https://scan.example/api/pgp/qr/key")), mixed));
}

QTEST_GUILESS_MAIN(PgpQrTargetValidatorTest)
#include "PgpQrTargetValidatorTest.moc"
