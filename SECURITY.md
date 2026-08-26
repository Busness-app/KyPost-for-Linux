# Security Policy

KyPost for Linux is the desktop companion to a self-hosted KyPost server. It
holds a per-device pairing credential for someone's mail account, caches their
mail, and holds the PGP private key sealed to this device. This document covers
how to report a vulnerability and where the boundary with the server lies.

For **server-side** security — TLS termination, reverse proxies, key custody at
rest, deployment hardening — see
[SECURITY.md in the server repository](https://github.com/Busness-app/KyPost-Server/blob/main/SECURITY.md).
Report server vulnerabilities there, not here.

## Reporting a vulnerability

Report privately through GitHub Security Advisories rather than opening a public
issue.

1. Go to the [Security Advisories](https://github.com/Busness-app/KyPost-for-Linux/security/advisories/new) page
2. Click **Report a vulnerability**
3. Give a description, the affected versions, and reproduction steps if you have them
4. Please do not disclose publicly until a fix is available

Private vulnerability reporting is enabled on this repository, so that form works
for anyone — you do not need to be a maintainer.

If you are unsure which repository a finding belongs to, file it here and it will
be moved. A misfiled report is better than an unfiled one.

## What this client is responsible for

- **The device credential.** `X-Kypost-Device-Id` / `X-Kypost-Device-Secret` are
  minted once at pairing and authenticate every later request. They are this
  device's identity to the server.
- **The sealed PGP envelope.** Enrollment seals the account key to a key in this
  device's own store. The verification code shown during enrollment is the only
  thing standing between you and a server that substituted its own key — see
  [PLATFORM_BASELINE.md](https://github.com/Busness-app/KyPost-Server/blob/main/docs/PLATFORM_BASELINE.md)
  section 2, which is normative across every client.
- **Rendering hostile mail.** Message bodies are attacker-controlled input.

## Known limitation: pairing is trust-on-first-use

Server 0.3.0 and later publish a certificate pin in the pairing QR (`pin=`). This
client does not read it. It instead records the certificate that served
registration and pins later calls to that — trust-on-first-use, which is what it
did before the pin existed.

The gap is that the pairing request carries the pairing token and push
credentials together, and trust-on-first-use trusts the certificate only *after*
they have been disclosed. On a network with a locally trusted CA — enterprise
MDM, a user-installed root, a hostile captive portal — an interceptor can read
the token. This is tracked as an open issue; it is stated here rather than left
for someone to discover.

## Disclosure timeline

Matching the server repository, so a finding that spans both is not governed by
two different clocks.

- **Acknowledgement:** within 7 days
- **Assessment:** within 14 days
- **Fix or mitigation plan:** communicated with the assessment

## Contact

- **Vulnerability reports:** [GitHub Security Advisories](https://github.com/Busness-app/KyPost-for-Linux/security/advisories/new)
- **Maintainer:** [Yoshiofthewire](https://github.com/Yoshiofthewire)
