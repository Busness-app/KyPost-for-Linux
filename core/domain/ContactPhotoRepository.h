#pragma once

#include "domain/RelayRequestPlan.h"
#include "net/ContactPhotoClient.h" // ContactPhotoFetchResult -- crosses the thread hop by value

#include <QString>
#include <optional>

class ContactPhotoCache;
class HttpClient;
class PairingStore;

// Lazy per-contact photo fetch+cache, mirroring GroupsRepository's shape
// (see its doc comment for the "why a separate class" reasoning) but scoped
// to one contact at a time rather than a full-replace list. Reached from
// ContactsController::photoPathFor() on contact-detail open / list-row
// become-visible (task-3-brief.md: "fetch lazily... not as part of the bulk
// sync payload") -- never from ContactSyncRepository::sync() itself.
//
// CACHE READ AND NETWORK FETCH ARE SEPARATE CALLS, and that separation is the
// whole point of this interface rather than a tidying.
//
// photoPathFor() below used to be the only entry point, and it did both: a
// cache miss went straight to the relay on the calling thread. Avatar.qml
// binds `photoSource` to it, so that put a blocking HTTP request behind a QML
// property binding. Every visible delegate with an uncached photo entered
// HttpClient's nested event loop in turn -- which keeps dispatching QML input
// while it is suspended -- and a failed fetch retried on every re-evaluation
// of the binding, forever, because nothing recorded that it had been tried.
// A slow-but-reachable relay was enough to freeze the contact list solid.
//
// So callers now read the cache (never blocks, never touches the network) and
// dispatch the miss themselves.
class ContactPhotoRepository
{
public:
    ContactPhotoRepository(ContactPhotoClient& client, ContactPhotoCache& cache, PairingStore& pairingStore);

    // Cache only. Empty means "not on disk", which is NOT the same as "no
    // photo" -- it is the caller's cue to dispatch planFetch() below. Never
    // blocks and never reaches the network, so it is safe from a binding.
    QString cachedPathFor(const QString& photoRef) const;

    // Three-phase form, same shape as every other repository here. Returns
    // std::nullopt when this device is not paired, which is an ordinary state
    // and not a failure -- a contact list still renders, on initials.
    std::optional<RelayRequestPlan> planFetch() const;
    static ContactPhotoFetchResult fetchWith(HttpClient& httpClient, const RelayEndpoint& endpoint,
                                              const QString& contactUid);

    // Writes nothing when the device has been re-paired since the request went
    // out. A face is about as identifying as cached data gets, and the photo
    // cache directory is one of the things pairing a replacement account
    // erases -- writing this now would put it back, keyed by a photoRef the
    // new account's contacts can resolve.
    //
    // Returns the cached absolute path, or an empty string on any failure
    // (transport, 401, empty body, re-pair mid-flight, or a cache write that
    // did not land). Degrades gracefully, never crashes, matching this
    // feature's ContactPhotoClient Global Constraint.
    QString applyFetch(const RelayRequestPlan& plan, const QString& photoRef,
                        const ContactPhotoFetchResult& result) const;

    // The synchronous composition of the three above, kept for the same
    // reason every other repository here keeps one: it is what the tests
    // drive, and it is what stops the async phases drifting from the
    // behaviour those tests pin. BLOCKS ON THE NETWORK -- do not call it from
    // the GUI thread.
    QString photoPathFor(const QString& contactUid, const QString& photoRef) const;

private:
    ContactPhotoClient& m_client;
    ContactPhotoCache& m_cache;
    PairingStore& m_pairingStore;
};
