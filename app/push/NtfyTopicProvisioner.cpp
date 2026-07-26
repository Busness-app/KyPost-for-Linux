#include "push/NtfyTopicProvisioner.h"

#include "stores/SecureStore.h"

#include <QByteArray>
#include <QLoggingCategory>
#include <QRandomGenerator>

namespace NtfyTopicProvisioner {

namespace {

constexpr auto kNtfyTopicKey = "ntfy-topic";

// 16 random bytes (128 bits) hex-encoded to a 32-character topic string --
// ntfy topics are plain URL-path segments (docs.ntfy.sh/publish/#topics), so
// hex avoids any need for URL-escaping, unlike e.g. QUuid's dashed/braced
// format. QRandomGenerator::system() draws from the OS CSPRNG
// (getrandom(2) on Linux) -- the only cryptographically-appropriate random
// source already available via Qt6::Core. This codebase has no existing
// random-secret generator to reuse instead: QUuid::createUuid() (used
// elsewhere, e.g. ContactSyncRepository's temp-uid generation) is not
// documented as cryptographically secure and is used there only for a
// non-secret, dedup-only identifier, not a bearer credential.
QString generateTopic()
{
    quint32 buffer[4]; // 4 * 32 bits = 128 bits
    QRandomGenerator::system()->fillRange(buffer);

    const QByteArray bytes(reinterpret_cast<const char*>(buffer), sizeof(buffer));
    return QString::fromLatin1(bytes.toHex());
}

} // namespace

QString getOrCreateTopic(SecureStore& secureStore)
{
    const std::optional<QString> existing = secureStore.get(QLatin1String(kNtfyTopicKey));
    if (existing.has_value() && !existing->isEmpty())
        return *existing;

    return rotateTopic(secureStore);
}

QString rotateTopic(SecureStore& secureStore)
{
    const QString topic = generateTopic();
    // Checked, not fire-and-forget. Returning an unpersisted topic meant
    // every launch generated a fresh one, the backend kept the address from
    // whichever launch last managed to register, and push simply never
    // arrived -- with no error anywhere, because the caller had a
    // plausible-looking string in hand. An empty return tells the caller the
    // EmbeddedSubscriber tier has no usable address.
    if (!secureStore.set(QLatin1String(kNtfyTopicKey), topic)) {
        qWarning("NtfyTopicProvisioner: could not persist the ntfy topic -- the embedded push "
                 "subscriber will be unavailable this session");
        return QString();
    }
    return topic;
}

} // namespace NtfyTopicProvisioner
