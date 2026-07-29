#include "domain/PairingStoreCredentialSealer.h"

#include "domain/PairingStore.h"

PairingStoreCredentialSealer::PairingStoreCredentialSealer(PairingStore& pairingStore)
    : m_pairingStore(pairingStore)
{
}

bool PairingStoreCredentialSealer::seal(const QString& pin)
{
    return m_pairingStore.sealDeviceSecret(pin);
}

bool PairingStoreCredentialSealer::unsealPermanently(const QString& pin)
{
    return m_pairingStore.unsealDeviceSecretPermanently(pin);
}

bool PairingStoreCredentialSealer::reseal(const QString& oldPin, const QString& newPin)
{
    return m_pairingStore.resealDeviceSecret(oldPin, newPin);
}

bool PairingStoreCredentialSealer::unsealForSession(const QString& pin)
{
    return m_pairingStore.unsealDeviceSecret(pin);
}

void PairingStoreCredentialSealer::relock()
{
    m_pairingStore.lockDeviceSecret();
}

bool PairingStoreCredentialSealer::isSealed() const
{
    return m_pairingStore.deviceSecretSealed();
}
