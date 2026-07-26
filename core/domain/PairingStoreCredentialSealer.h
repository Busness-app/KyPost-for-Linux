#pragma once

#include "security/CredentialSealer.h"

class PairingStore;

// The production CredentialSealer: forwards each operation to PairingStore's
// seal/unseal methods and, unlike the main.cpp lambda this replaces, returns
// what they actually reported.
class PairingStoreCredentialSealer : public CredentialSealer
{
public:
    explicit PairingStoreCredentialSealer(PairingStore& pairingStore);

    bool seal(const QString& pin) override;
    bool unsealPermanently(const QString& pin) override;
    bool unsealForSession(const QString& pin) override;
    void relock() override;
    bool isSealed() const override;

private:
    PairingStore& m_pairingStore;
};
