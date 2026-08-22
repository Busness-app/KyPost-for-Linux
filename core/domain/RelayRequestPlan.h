#pragma once

#include "domain/DevicePairing.h"
#include "net/RelayAuth.h"

// Phase 1's output for the repositories whose request needs nothing but an
// endpoint: where to send it, as whom, and which pairing authorised it.
//
// RelayEndpoint alone was not enough. It carries deviceId (inside RelayAuth)
// but not subscriberId, and the identity a reply is judged against needs
// both -- see PairingIdentity in DevicePairing.h for why, and for what
// happens to a reply that arrives after the device has been re-paired.
//
// Deliberately NOT solved by adding subscriberId to RelayEndpoint: that is a
// net/ type describing what goes on the wire, and the subscriber id is never
// sent. It is a domain fact about who asked.
struct RelayRequestPlan
{
    RelayEndpoint endpoint;
    PairingIdentity identity;
};
