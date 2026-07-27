#pragma once

#include <QHostAddress>
#include <QList>
#include <QString>
#include <QUrl>

#include <functional>

// Resolves a hostname to its IP addresses -- injectable so tests can
// exercise the link-local check against a hostname without depending on
// real DNS. Defaults to a real (blocking) lookup in production; see
// PgpQrTargetValidator.cpp's defaultResolveHost(). Literal IP strings are
// handled by the default resolver without any actual DNS query.
using HostResolver = std::function<QList<QHostAddress>(const QString& host)>;

// Decides whether a scanned QR code's URL may be fetched at all. The input
// is entirely attacker-chosen -- a QR code is just a picture, and it can be
// printed on a poster, embedded in a message, or shown on a screen -- so
// every rule here exists to stop a scan from doing something worse than
// "fetch a key from wherever it points":
//
//  - Scheme: http(s) only (file:// would read local files back as if they
//    were key material), and https specifically unless the host is
//    loopback. See the .cpp for why TLS is what actually closes the DNS
//    rebinding hole, and why loopback is the one safe exemption.
//  - Address: no link-local (169.254.0.0/16 -- AWS/Azure/DigitalOcean's
//    metadata IP), no GCP metadata.google.internal, and no RFC1918/
//    unique-local/multicast/broadcast address. Loopback remains allowed:
//    a self-hosted relay on localhost is a supported setup and is
//    test-locked as such.
//  - Resolution: every host is resolved (via resolveHost, real DNS by
//    default) and every returned address is checked. An unresolvable host
//    is rejected -- fail closed rather than treated as safe.
//
// The address check used to fire only when the QR text's host was ALREADY a
// literal IP string, because QHostAddress::setAddress() returns false for an
// ordinary hostname; any attacker-registered domain whose A record pointed
// at a metadata address sailed straight through. That is what the
// resolve-everything rule above fixed.
bool isSafeQrTarget(const QUrl& url, const HostResolver& resolveHost = {});
