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
    // A redirect was offered and refused, because the target failed the
    // request's redirect validator (by default: it left the origin the
    // caller named).
    //
    // Distinct from Server for the same reason CertificateMismatch is
    // distinct from Transport. Aborting the reply mid-redirect leaves the
    // 3xx as the reply's status, which networkErrorFromStatusCode() maps to
    // Server -- so "the relay tried to send your device secret to another
    // host" and "the relay returned a 500" arrived at every caller, and
    // every log line, as the same value. The relay emits no redirects at
    // all, so this firing is a security event, not a hiccup.
    RedirectRefused,
    // The response exceeded the byte ceiling the caller allowed, either by
    // declaring too large a Content-Length or by simply sending more than it
    // declared. The reply is aborted; no partial body is returned.
    //
    // Its own value rather than Transport because it is the one failure here
    // that is a defence rather than a fault: nothing about the network went
    // wrong, this client refused to keep allocating. A caller may reasonably
    // retry a Transport error and must not retry this one -- the same
    // response will be just as large next time.
    ResponseTooLarge,
    Decoding,           // JSON parse failure; produced by each Task 14-18
                        // client's own decode step, never by HttpClient
};

// Maps a non-2xx HTTP status code to its NetworkError. Returns std::nullopt
// for 2xx (success). Never returns Transport, Decoding or RedirectRefused —
// those arise outside the HTTP status code itself (see NetworkError above).
std::optional<NetworkError> networkErrorFromStatusCode(int statusCode);
