#pragma once

#include <QByteArray>
#include <QString>

// Converts an existing plaintext profile database to an encrypted one.
//
// The only code in this repo that rewrites a user's existing mail store, so
// the ordering below is the design and not an implementation detail:
//
//   1. A marker goes down BEFORE anything changes. A conversion killed
//      part-way -- power loss, an OOM kill -- writes nothing on its own,
//      because the code that would write it never runs. Same reasoning as
//      core/security/WipeTripwire.h.
//   2. The encrypted copy is written to a SEPARATE file. The original is not
//      touched at all while it is being read.
//   3. That copy is opened with the key and its contents compared against
//      the original, table by table, before anything is moved.
//   4. Only then is the original renamed aside, the copy put in its place,
//      and the final file re-opened to prove it works.
//   5. The plaintext original is securely deleted LAST, once there is a
//      verified encrypted database sitting where it used to be.
//
// Any step failing leaves the plaintext database exactly where it was and
// still openable. Losing the user's mail is a worse outcome than leaving it
// unencrypted for another launch, and every branch here is written that way
// round.
//
// reconcile() is called on every launch, not only after a failure: leftovers
// are the normal evidence of an interrupted run, and cleaning them up is how
// the next attempt gets a clean field.
class DatabaseEncryptionMigration
{
public:
    // The three files this class puts beside the profile database. Public
    // because the wipe paths have to be able to name them, and the only way
    // to keep the two in step is to have one place that says what they are:
    // `.plaintext-old` is a COMPLETE unencrypted copy of the user's mail,
    // swapInEncrypted() deliberately survives failing to delete it, and a run
    // killed between the two renames leaves it for the next launch to find.
    static constexpr QLatin1StringView kMarkerSuffix{".encrypting"};
    static constexpr QLatin1StringView kWorkingCopySuffix{".encrypted-new"};
    static constexpr QLatin1StringView kSupersededSuffix{".plaintext-old"};

    // `databasePath` is the live profile database.
    explicit DatabaseEncryptionMigration(const QString& databasePath);

    enum class Status
    {
        NotNeeded,   // already encrypted, or there is nothing to convert
        Migrated,    // converted and verified this launch
        Failed,      // the plaintext database is untouched and still usable
        Stranded,    // NOTHING is openable at the profile's path: an
                     // interrupted swap left the only complete database under
                     // another name and it could not be moved back. The
                     // caller must not open or create anything here.
    };

    // Tidies up after an interrupted previous attempt, then converts if
    // there is a plaintext database to convert.
    Status run(const QByteArray& key);

    // True when a previous attempt armed the marker and never reported
    // finishing. Exposed for the host's journal, and for tests.
    bool interrupted() const;

private:
    bool arm();
    bool disarm();
    void disarmOrWarn();
    void discardWorkingCopy();
    // Puts the profile back into one of the two states a launch can start
    // from -- "plaintext original in place" or "encrypted database in place"
    // -- by undoing whatever half-finished rename an interrupted run left.
    //
    // Returns false only for the outcome that matters: a complete database
    // that could not be moved back to the profile's path. Reporting that as
    // success is how a failed rollback turns into a fresh empty profile
    // written over the top of somebody's mail.
    [[nodiscard]] bool reconcile();
    bool exportEncrypted(const QByteArray& key);
    bool verifyCopyMatchesOriginal(const QByteArray& key) const;
    bool swapInEncrypted(const QByteArray& key);

    QString m_databasePath;
    QString m_markerPath;
    QString m_workingCopyPath;
    QString m_supersededPath;
};
