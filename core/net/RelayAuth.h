#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>

// Per-device auth credentials (deviceId + deviceSecret), sent as
// X-Kypost-Device-Id/X-Kypost-Device-Secret headers on every authenticated
// Relay request. deviceSecret is minted server-side once per successful
// registration and returned only in that response -- see
// DeviceRegistrationService::pair(). Plain value type with no store
// dependency -- callers pull deviceId/deviceSecret out of whatever
// pairing/session store owns them and hand this to HttpClient::get/post.
struct RelayAuth
{
    QString deviceId;
    QString deviceSecret;

    QList<QPair<QString, QString>> headerItems() const
    {
        return { { QStringLiteral("X-Kypost-Device-Id"), deviceId },
                 { QStringLiteral("X-Kypost-Device-Secret"), deviceSecret } };
    }

    bool operator==(const RelayAuth&) const = default;
};

// Where to send an authenticated Relay request, and as whom. Everything a
// repository reads out of PairingStore before it can make a call.
//
// Exists as a type because the threading migration made that read a distinct
// PHASE: PairingStore caches and is mutated by the credential gate, so it may
// only be touched on the owning thread, and what crosses to the executor
// thread has to be this plain pair of values. See docs/THREADING.md.
struct RelayEndpoint
{
    QUrl serverBaseUrl;
    RelayAuth auth;

    bool operator==(const RelayEndpoint&) const = default;
};
