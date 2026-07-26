#pragma once

#include <optional>

// Shared failure model for every Relay HTTP call (Task 14-18 clients build
// on this). Mirrors kypost-for-Mac's NetworkError (verified reference:
// Data/Networking/HTTPClient.swift), read for structure only. `Server` does
// not carry the status code the way the Swift `.server(statusCode:)` case
// does — HttpResult::statusCode already exposes it to the caller, so no
// payload is duplicated here.
enum class NetworkError
{
    InvalidUrl,
    Unauthorized,       // 401/403 — pairing credentials rejected
    Conflict,           // 409 — backend rejected the request state
    RateLimited,        // 429 — too many requests, retry later
    ServiceUnavailable, // 503 — backend config issue, do not retry
    Server,             // any other non-2xx status
    Transport,          // network-level failure (e.g. connection refused/timeout)
    // The server's TLS public key does not match the one captured when this
    // device paired. Kept distinct from Transport because the two mean very
    // different things to a user: "the network is flaky, retry" versus
    // "something is impersonating your mail server, do not retry".
    CertificateMismatch,
    Decoding,           // JSON parse failure; produced by each Task 14-18
                        // client's own decode step, never by HttpClient
};

// Maps a non-2xx HTTP status code to its NetworkError. Returns std::nullopt
// for 2xx (success). Never returns Transport or Decoding — those arise
// outside the HTTP status code itself (see NetworkError above).
std::optional<NetworkError> networkErrorFromStatusCode(int statusCode);
