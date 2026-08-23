#pragma once

#include <QByteArray>
#include <QString>

class SecureStore;

// Custody of the 32-byte key the local mail/contacts database is encrypted
// with.
//
// WHY THE READ IS THREE-STATE AND THE CREATE IS SEPARATE
//
// The obvious shape for this class is a single loadOrCreate(). It is also
// the one shape that can destroy a user's mail.
//
// SecureStore::get() collapses "there is no key" and "the store could not be
// consulted" into the same nullopt (see SecureStore.h, and PairingStore's
// loadChecked() for the same problem solved the same way). A loadOrCreate()
// built on that would, on a keyring that is merely locked or not yet
// running, decide there was no key, generate a FRESH one, and write it over
// the old. The database it was meant to open is then unopenable by anyone,
// including its owner, permanently -- the key that decrypts it no longer
// exists anywhere.
//
// So reading and creating are different questions with different answers,
// and a caller has to ask them separately. Unreadable is never a reason to
// mint a key.
class DatabaseKeyStore
{
public:
    explicit DatabaseKeyStore(SecureStore& secureStore);

    enum class Status
    {
        Found,      // `key` holds this profile's database key
        Absent,     // the store answered, and this profile has no key
        Unreadable, // the store could not be consulted; nothing may be concluded
        Corrupt,    // a key is stored, but it is not one this build can use
    };

    struct Result
    {
        Status status = Status::Unreadable;
        QByteArray key; // set only when Found; exactly kKeyBytes long
    };

    // Never generates anything.
    Result existing() const;

    // Generates a fresh key from the system CSPRNG and persists it.
    //
    // Returns the key on success and an empty QByteArray if the store
    // refused the write -- in which case NOTHING has been created as far as
    // any later read is concerned, and the caller must not go on to encrypt
    // a database with a key that was never saved.
    //
    // Deliberately not "create if missing": the caller has to have looked
    // first, and to have decided that Absent (not Unreadable) is what it
    // saw.
    QByteArray create();

    // Removes the key. Used by the wipe paths -- a device whose mail has
    // been erased must not keep the key that would have decrypted it.
    // Reports whether the removal actually landed.
    bool clear();

    static constexpr int kKeyBytes = 32;

private:
    SecureStore& m_secureStore;
};
