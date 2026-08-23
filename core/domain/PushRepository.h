#pragma once

#include "db/PushDao.h"
#include "models/PushNotification.h"

#include <QString>
#include <QVector>
#include <optional>

class CursorStore;
class PairingStore;
class PushNotificationClient;
class SettingsStore;

// Sits between PushNotificationClient and PushDao, matching the
// Domain/Repositories layer in kypost-for-Mac (PushRepository.swift) --
// port of PushRepositoryTests from PushTests.swift, with one deliberate
// deviation documented on recordPushArrival() below.
class PushRepository
{
public:
    PushRepository(PushDao& pushDao, CursorStore& cursorStore, PushNotificationClient& client,
                    PairingStore& pairingStore, SettingsStore& settingsStore);

    // Most-recent-first, up to `limit`.
    QVector<PushRecord> history(int limit = 50) const;

    // pushDao.markConsumed. False means the row is still unread on disk --
    // the UI may have already greyed it out, so a caller that cares about
    // the two agreeing after a restart has to look.
    [[nodiscard]] bool markRead(const QString& messageId);

    // Records a push-mode arrival (no server seq on this path -- one is
    // synthesized from arrival time). Returns the record actually
    // persisted (its .seq may differ from the naive receivedAtEpochMs on
    // a collision).
    //
    // Deviation from PushTests.swift's pushArrivalsGetUniqueSynthesizedSeqs,
    // deliberate, not an oversight: that test asserts the *same* messageId
    // arriving twice at the same instant produces two separate history
    // rows with different seqs. Our push_notifications table is keyed by
    // message_id (a Phase 2 decision, already shipped) -- insertOrReplace
    // on a repeat messageId overwrites the existing row rather than adding
    // a second one, which is the intentionally correct behavior for our
    // schema (two notifications about the *same* message shouldn't produce
    // two history entries). See PushRepositoryTest for the two assertions
    // that replace the Swift test's single assertion.
    //
    // nullopt when the row did not land. Deliberately NOT a reason to drop
    // the notification: this is the distributor path, the message has already
    // been handed to us by the OS and cannot be re-requested from the relay,
    // so showing it without a history row beats not showing it at all. The
    // caller logs.
    std::optional<PushRecord> recordPushArrival(const PushNotification& payload, qint64 receivedAtEpochMs);

    // One poll of the pull endpoint. Returns newly-delivered notifications,
    // which are persisted AND covered by a cursor that is on the disk before
    // any of them is returned -- an empty result can therefore mean "nothing
    // new" or "could not store what arrived", and both are handled the same
    // way: the cursor did not move, so the next poll asks again.
    //
    // NotPaired-equivalent: returns an empty vector when there is no stored
    // pairing -- this method has no error-outcome
    // return type (unlike the repositories above) because polling is
    // expected to run silently and retry on its own schedule (Task 25
    // owns the retry/backoff policy); a caller that needs to distinguish
    // "no pairing" from "polled, nothing new" should check
    // PairingStore::isPaired() itself first.
    QVector<PushNotification> pullOnce();

private:
    QString resolvePullEndpoint(const QString& serverBaseUrl) const;

    PushDao& m_pushDao;
    CursorStore& m_cursorStore;
    PushNotificationClient& m_client;
    PairingStore& m_pairingStore;
    SettingsStore& m_settingsStore;
};
