#include "security/DatabaseKeyStore.h"

#include "stores/SecureStore.h"

#include <QRandomGenerator>

namespace {

// Hex rather than base64: SecureStore holds QStrings, and hex has no
// characters a backend might quote, pad or normalise. 32 bytes -> 64 chars.
const QString kDatabaseKeyKey = QStringLiteral("db.encryptionKey");

} // namespace

DatabaseKeyStore::DatabaseKeyStore(SecureStore& secureStore) : m_secureStore(secureStore)
{
}

DatabaseKeyStore::Result DatabaseKeyStore::existing() const
{
    const SecureStore::ReadResult stored = m_secureStore.read(kDatabaseKeyKey);
    if (stored.failed())
        return { Status::Unreadable, {} };
    if (stored.status != SecureStore::ReadStatus::Found)
        return { Status::Absent, {} };

    // Corrupt is its own answer, never folded into Absent. Treating a
    // damaged key as "no key" would send the caller down the create() path
    // and overwrite the only copy of the real one.
    const QByteArray decoded = QByteArray::fromHex(stored.value.toLatin1());
    if (decoded.size() != kKeyBytes)
        return { Status::Corrupt, {} };

    return { Status::Found, decoded };
}

QByteArray DatabaseKeyStore::create()
{
    QByteArray key(kKeyBytes, Qt::Uninitialized);
    static_assert(kKeyBytes % 4 == 0, "generate() fills whole 32-bit words");
    // QRandomGenerator::system() is the CSPRNG (getrandom/urandom); the
    // default global generator is not, and this is a key.
    QRandomGenerator::system()->generate(reinterpret_cast<quint32*>(key.data()),
                                          reinterpret_cast<quint32*>(key.data() + key.size()));

    // Checked. A SecureStore write fails on any machine with no reachable
    // Secret Service, and returning the key anyway would have the caller
    // encrypt a database with a key that exists only in this process --
    // readable until the app exits, and then never again.
    if (!m_secureStore.set(kDatabaseKeyKey, QString::fromLatin1(key.toHex())))
        return {};

    return key;
}

bool DatabaseKeyStore::clear()
{
    return m_secureStore.remove(kDatabaseKeyKey);
}
