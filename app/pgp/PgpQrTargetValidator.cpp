#include "pgp/PgpQrTargetValidator.h"

#include <QDeadlineTimer>
#include <QEventLoop>
#include <QHostInfo>
#include <QTimer>

namespace {

// How long a QR target's DNS lookup may take before it is treated as
// unresolvable. QHostInfo::fromName() is unbounded -- with an unreachable
// resolver it blocks for however long libc's resolver decides to retry,
// which was tens of seconds of frozen UI on a path reached straight from a
// camera frame. lookupHost() + a timer is the same nested-event-loop shape
// HttpClient::waitForReply already uses for exactly this reason.
constexpr int kResolveTimeoutMs = 5000;

QList<QHostAddress> defaultResolveHost(const QString& host)
{
    QHostAddress literal;
    if (literal.setAddress(host))
        return { literal };

    QList<QHostAddress> addresses;
    QEventLoop loop;
    bool finished = false;

    const int lookupId = QHostInfo::lookupHost(host, &loop, [&](const QHostInfo& info) {
        finished = true;
        if (info.error() == QHostInfo::NoError)
            addresses = info.addresses();
        loop.quit();
    });

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(kResolveTimeoutMs);

    loop.exec();

    if (!finished) {
        // Timed out. Abort the in-flight lookup so its callback cannot fire
        // against the now-dead loop, and report "unresolvable", which
        // isSafeQrTarget below treats as unsafe.
        QHostInfo::abortHostLookup(lookupId);
        return {};
    }
    return addresses;
}

// Everything that is not a routable public address. isLinkLocal() alone
// (169.254/16, fe80::/10) covered the cloud-metadata IP and nothing else, so
// a scanned QR could still aim the fetch at a router admin page, a printer,
// or any other service on the user's own LAN and have the response rendered
// back as a contact's PGP key.
//
// Loopback stays allowed, deliberately and test-locked: a self-hosted relay
// on localhost is a supported setup, and a literal loopback address cannot
// be re-pointed by DNS the way a hostname can.
bool isNonPublicAddress(const QHostAddress& addr)
{
    return addr.isLinkLocal() || addr.isUniqueLocalUnicast() || addr.isMulticast()
        || addr.isBroadcast() || addr.isPrivateUse();
}

} // namespace

bool isSafeQrTarget(const QUrl& url, const HostResolver& resolveHost)
{
    const bool isHttps = url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0;
    const bool isHttp = url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0;
    if (!isHttps && !isHttp)
        return false;

    const QString host = url.host();
    if (host.isEmpty())
        return false;
    if (host.compare(QStringLiteral("metadata.google.internal"), Qt::CaseInsensitive) == 0)
        return false;

    const HostResolver resolver = resolveHost ? resolveHost : HostResolver(defaultResolveHost);
    const QList<QHostAddress> addresses = resolver(host);
    if (addresses.isEmpty())
        return false; // unresolvable -- can't verify safety, so don't proceed

    bool allLoopback = true;
    for (const QHostAddress& addr : addresses) {
        if (addr.isLoopback())
            continue; // allowed, deliberately -- see isNonPublicAddress()
        allLoopback = false;
        if (isNonPublicAddress(addr))
            return false;
    }

    // https required for anything that is not purely loopback.
    //
    // Two separate problems, one rule -- and it has to be applied AFTER
    // resolution, because it is the resolved addresses that decide whether
    // cleartext is defensible:
    //
    //  - Cleartext: the fetched public key and fingerprint are what the user
    //    is about to trust for encrypting mail to this person. Over http,
    //    any on-path attacker substitutes their own key. On loopback there
    //    is no path to be on.
    //  - DNS rebinding: this function resolves the host, but Qt resolves it
    //    AGAIN when connecting, and an attacker-controlled nameserver can
    //    answer differently the second time -- passing the address checks
    //    above with a public address, then connecting to a link-local one.
    //    Requiring TLS closes that, because the rebound address must present
    //    a certificate valid for the QR's hostname, which a metadata service
    //    or LAN device cannot. Loopback is exempt because rebinding a name
    //    that already resolves to 127.0.0.1 buys an attacker nothing: they
    //    could have written the loopback literal into the QR directly.
    if (isHttp && !allLoopback)
        return false;

    return true;
}
