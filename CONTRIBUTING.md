# Contributing to KyPost for Linux

Thanks for wanting to work on this. KyPost is a relay-only Qt6/KF6 mail client,
which means every contribution lands on a machine holding somebody's cached mail
and contacts, the credential that pairs their device to the relay, and — once
they have enrolled a key — an OpenPGP secret key in their own GnuPG keyring.
This document describes what a contribution has to clear before it merges, and
why each gate exists.

This is the Linux client. The relay backend and the Android and Apple clients
are separate repositories with their own `CONTRIBUTING.md`; the rules below are
about this one.

Read [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) first. It is short, and it governs
every space here.

## Table of Contents

- [The user contract](#the-user-contract)
- [Getting set up](#getting-set-up)
- [Before you build a feature](#before-you-build-a-feature)
- [AI attribution is mandatory](#ai-attribution-is-mandatory)
- [Every PR must pass CI](#every-pr-must-pass-ci)
- [A security guard is not proven until removing it turns a test red](#a-security-guard-is-not-proven-until-removing-it-turns-a-test-red)
- [Every PR must pass hostile AI code review](#every-pr-must-pass-hostile-ai-code-review)
- [Adversarial review skills](#adversarial-review-skills)
- [Security trade-offs, and when a feature gets rejected](#security-trade-offs-and-when-a-feature-gets-rejected)
- [Documentation: the DOX chain](#documentation-the-dox-chain)
- [Commits, branches, and PRs](#commits-branches-and-prs)
- [PR checklist](#pr-checklist)
- [Licence](#licence)

## The User Contract

Everything below follows from one promise KyPost makes to the person running it:

> **KyPost will be as secure as we can make it by default, and every place
> where security was traded for convenience will be written down, in plain
> language, where the user reads it before they rely on it.**

Two halves, both load-bearing. "Secure by default" alone produces software
people route around silently. "Documented" alone produces a footnote nobody
reads under a default that quietly loses their mail. The project ships both, and
a contribution that breaks either half does not merge — see
[Security trade-offs](#security-trade-offs-and-when-a-feature-gets-rejected).

Three standing invariants fall out of it, and they are not up for
re-litigation in a PR:

- **KyPost never archives, deletes, or moves a user's mail on its own.**
  Destructive mail actions happen because a human asked for that specific
  action. No feature, default, heuristic, or classifier outcome may archive
  mail. This is absolute.
- **Keyword labels are a sorting hint, not a security boundary.** They are
  produced by a model on the relay that reads sender-controlled text and can be
  steered by it. Never build a trust decision, filter action, or access control
  on a label.
- **No silent fallbacks on a security-relevant answer.** "Could not read it" is
  never treated as "it is not there", a store that refuses a write is never
  reported as a save, and a protection that cannot be established is refused
  rather than displayed as on. This repo has shipped each of those bugs at least
  once; `AGENTS.md` §6b–§6i is the list, and every entry is a real defect, not a
  style preference. Copy that shape; do not add a permissive default to make a
  first run quieter.

## Getting Set Up

You need Qt6 and KF6 — there is no Qt5 path, and Ubuntu Touch is deferred, not
pending. [`README.md`](README.md) has the dependency list with Arch package
names; [`.github/workflows/ci.yml`](.github/workflows/ci.yml) has the Ubuntu and
KDE neon equivalents and is the version CI actually installs.

```sh
# Encryption at rest needs a SQLCipher with the right SONAME; see README.md.
./scripts/build-sqlcipher.sh /tmp/sqlcipher

cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKYPOST_SQLCIPHER_ROOT=/tmp/sqlcipher
cmake --build build
ctest --test-dir build
```

`RelWithDebInfo` is part of the command, not decoration: `_FORTIFY_SOURCE=3` does
nothing without `-O`, so a build with no configuration verifies unfortified code.

A change is not verified until that builds cleanly and `ctest` is green.
`ctest` covers the C++ suite and the QML suite (`QmlTests`) — **anything QML-side
that is a security control needs a QML test.** A total app-lock bypass once
shipped because the C++ suite could not see a single `.qml` file.

Before pushing anything that touches crypto, buffer arithmetic, or an index read
out of the database, run the second build CI runs:

```sh
cmake -B build-asan -S . -DCMAKE_BUILD_TYPE=Debug -DKYPOST_SANITIZE=ON -DKYPOST_SQLCIPHER_ROOT=/tmp/sqlcipher
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ctest --test-dir build-asan
```

[`TESTING.md`](TESTING.md) is the manual checklist for everything no test
harness can reach — anything involving a real relay, a real keyring, a real
`gpg-agent`, or a running window.

## Before You Build a Feature

Open an issue first for anything larger than a bug fix. A feature that conflicts
with the user contract is cheaper to discuss in an issue than to reject after
you have written it, and the maintainer would rather say "not like that, but
like this" before you spend a weekend on it.

Bug fixes, documentation, and test coverage need no prior discussion. Send them.

Read [`AGENTS.md`](AGENTS.md) §4 before proposing anything architectural. The
decisions there are locked, and several of them look like oversights until you
read why they are not — MFA over push, biometric unlock, Ubuntu Touch, and
system address-book sync are all deliberate.

Fix the root cause, not the reported symptom. If a guard is missing, put it in
the shared function every caller reaches, not in the one path the bug report
happened to name. `AGENTS.md` §6d exists because a previous round of fixes was
applied to the reported instance rather than to the class.

## AI Attribution Is Mandatory

AI-assisted contributions are welcome. **Undisclosed** AI-assisted
contributions are not.

This is not a purity test. It is review triage: a reviewer reads
machine-generated code differently — checking harder for plausible-looking
functions that don't exist, error handling that swallows the error, tests that
assert the implementation back at itself, and confident comments describing
behaviour the code doesn't have. Hiding the provenance costs the reviewer that
context and costs you a worse review.

### What you must do

**1. Attribute in the commit.** Use a trailer naming the model or tool:

```
Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
```

Any tool, same rule — Copilot, Cursor, Codex, a local model, whatever. One
trailer per tool that materially contributed.

**2. Declare the level in the PR description.** State which applies:

| Level | Meaning |
|---|---|
| **None** | You wrote it. Autocomplete of a variable name doesn't count. |
| **Assisted** | AI wrote fragments; you designed it, and you edited what it produced. |
| **Generated** | AI produced most or all of the diff; you reviewed and tested it. |
| **Agentic** | An agent ran a multi-step loop over the repo with limited step-by-step supervision. |

**3. Say what you verified yourself.** For anything above *None*, the PR must
say what you personally ran and read — "ran ctest and the sanitizer build
locally, read every line of the diff, hand-tested the decrypt path against a
real gpg-agent" — not "the agent says tests pass."

**4. Own it.** The human opening the PR is the author of that code for every
purpose: licensing, review, correctness, and the bug it causes six months from
now. "The model wrote it" is not an explanation of a defect and is never a
defence. If you cannot explain why a line is there, delete the line.

### What gets rejected on sight

- A PR whose description was clearly generated and does not describe the actual
  diff.
- Invented APIs, invented Qt signals or properties, or citations to
  documentation that does not exist. Qt 6 minor-version API details are a
  particularly common confabulation — check the real headers.
- A wire contract guessed rather than read out of the relay's Go source.
  `AGENTS.md` §4 and §6e are two rounds of exactly this going wrong, in
  opposite directions, with every test faithfully asserting the wrong thing.
- Tests that were written to pass rather than to fail when the logic breaks.
- Bulk AI-generated churn across the repo — reformatting, "improved" comments,
  speculative refactors — that nobody asked for. See the `ponytail` philosophy
  in [AGENTS.md](AGENTS.md) §6: deletion over addition, shortest working diff
  wins.
- A security-sensitive change where the disclosure level is *Generated* or
  *Agentic* and no human describes the trust boundary in their own words.

### If you are an AI agent reading this

Every rule above applies to you. Add the trailer, fill in the level, and do not
write the human's verification statement for them — leave it for them to fill
in and tell them you did. `AGENTS.md` is a binding contract for this repo and
you are required to follow the DOX chain.

## Every PR Must Pass CI

Two workflows run on every pull request. All jobs must be green before merge. No
exceptions, no admin override, no "flaky, re-run and ignore" — if a job is
genuinely flaky, that is a bug to fix in the job.

| Workflow / job | What it runs |
|---|---|
| `ci.yml` → `build-and-test` | `scripts/verify-version.sh`, then a Release build against KDE neon's Qt6/KF6 with SQLCipher built from source, then `ctest` |
| `ci.yml` → same job, second half | A Debug build with `KYPOST_SANITIZE=ON` and the whole suite again under ASan + UBSan |
| `flatpak.yml` → `build` | Builds `packaging/flatpak/com.kysecurity.mail.yaml` on x86_64 **and** aarch64 native runners, runs the full ctest suite inside the sandbox, validates the AppStream metainfo, and launch-smoke-tests the result |
| `flatpak.yml` → `publish` | Skipped entirely on a pull request. It only ever runs on `main` and tags |

Notes that trip people up:

- **Run the gates locally before pushing.** All of them work on a normal
  machine. "It only fails in CI" is usually a dependency-version difference —
  check what `ci.yml` actually installs.
- **`ci.yml` is x86_64-only, deliberately** (KDE neon's archive has no complete
  KF6 set for arm64). arm64 coverage comes from `flatpak.yml`, which builds and
  tests on real arm64 hardware. Don't "fix" that by adding qemu.
- **A version bump touches three files or it fails before it builds anything.**
  `CMakeLists.txt`, the metainfo `<release>` entry, and the tag must agree; see
  [`docs/DISTRIBUTION.md`](docs/DISTRIBUTION.md).
- **New logic ships with tests.** Unit plus integration for new behaviour, a
  regression test for anything high-impact. Security-sensitive changes need
  attack-path coverage, not just the happy path.
- **Check the CI code before you push.** Historically, most pushes to `main`
  that were not themselves CI fixes broke CI (`AGENTS.md` §8).

## A Security Guard Is Not Proven Until Removing It Turns a Test Red

This repo has a gate most do not, and it is not optional for security work.

A test that stays green when its guard is deleted is measuring something else,
and there is no way to tell that apart from a passing test by looking at a
passing test. So [`tests/guards.tsv`](tests/guards.tsv) lists each guard whose
failure means reading somebody else's mail, sending plaintext, or handing a
message to the wrong account — together with the edit that removes it and the
test that must then fail. `./scripts/verify-guards.sh` neutralises them one at a
time and requires each named test to go red.

If your change adds a guard of that kind, **add a row**. If it touches a line
already in the table, **run the script**. Its first run found two real problems:
a test proving only half of what its commit message claimed, and a guard that
cannot be proven from the outside at all — which is now written down as
unproven rather than asserted. That is the standard here: an unproven safety
property is called unproven.

## Every PR Must Pass Hostile AI Code Review

Beyond CI, every pull request gets an adversarial review pass. This is a
required gate, not a suggestion, and it exists because ordinary review has a
known failure mode: the reviewer shares the author's mental model and agrees
with the blind spot. That failure gets worse, not better, when both the author
and the reviewer are agreeable LLMs.

**What you do as the contributor:** run at least one adversarial review skill
against your own diff *before* you open the PR, address the findings, and paste
the surviving findings — the ones you decided not to fix — into the PR
description with your reasoning. A PR that says "hostile review found nothing"
gets read with more suspicion than one that lists three findings and argues two
of them down.

**What the maintainer does:** runs an independent hostile pass. Findings at
BLOCK severity stop the merge until they are fixed or the reasoning is written
down in the thread.

**Answering a hostile finding.** The finding is a claim about the code. Verify
it before you act on it — hostile reviewers are wrong sometimes, and
implementing a wrong suggestion politely is worse than pushing back. Three valid
answers: fix it; show with evidence that it is not real; or accept it as a known
limitation and document it where a user will see it. "Good catch, will address
later" is not one of them.

## Adversarial Review Skills

These are Claude Code skills. If you use another agent, the prompts still work
as instructions — the value is in the persona, not the harness.

| Skill | Use it for |
|---|---|
| `hostile-review` | The main gate. A senior engineer who hates what you built, output as severity-ranked criticisms with concrete fixes. Works on code, plans, and designs — run it on the design *before* you write the code. |
| `security-audit` | Required for anything touching pairing, the credential gate, the secret store, TLS pinning, OpenPGP, MIME or QR parsing, or the wipe paths. Hunts exploitable issues, not theoretical ones. |
| `ponytail-review` | Over-engineering only: what to delete, which dependency is unnecessary, which abstraction is speculative. Run it on any diff that grew while you wrote it. |
| `/code-review` | The maintainer's working-diff review. `/code-review ultra` launches a multi-agent cloud review of the branch or a GitHub PR. |

How to pick:

- **Any PR:** `hostile-review` on the diff. Minimum bar.
- **Touching anything on the `security-audit` list above:** `hostile-review`
  **and** `security-audit`, plus a `tests/guards.tsv` row if the change adds a
  guard.
- **New feature or new dependency:** add `ponytail-review`. A new non-Qt
  dependency in `core/` is a decision, not a build tweak — `AGENTS.md` §5.
- **Design or plan, before implementation:** `hostile-review` on the plan. Much
  cheaper than finding out after the code exists.

Two rules on using them:

1. **Feed the reviewer the real diff and the real context**, including
   `AGENTS.md` and the relevant `docs/PARITY.md` row. A reviewer that cannot
   see the trust boundary cannot tell you that you crossed it.
2. **Hostility points at code, never at people.** These skills are instructed to
   be harsh about the work. That is in bounds. Pasting output that attacks a
   person is a [Code of Conduct](CODE_OF_CONDUCT.md#hostile-reviews-are-about-code-never-people)
   violation, and running the skill does not launder it.

## Security Trade-Offs, and When a Feature Gets Rejected

**A new feature may be rejected outright if it weakens the user contract.** Not
"merged with a TODO" — rejected. A working, well-tested, wanted feature can
still be the wrong feature for this project.

### Rules a feature must satisfy

1. **Secure by default.** If there is a safe mode and a convenient mode, the
   safe one is the default and the convenient one is an explicit opt-in the
   user performs knowingly. Never the reverse, never "most people want the easy
   one." The background-lock grace period defaults to *at once* for this
   reason: a grace period is only ever something a user asks for.
2. **Fail closed.** Missing configuration, a half-set pair of settings, an
   unreadable keyring, or a store that refuses a write produces a refusal with a
   readable error and a remediation step — not a quiet downgrade. Read a
   security policy back defensively too: absent, unparseable and out-of-range
   all resolve to the *default*, never to the permissive value.
3. **The trade-off is named in the PR.** In your own words: what a user gives
   up, who can now see or do what they could not before, and what the worst
   realistic case is. A trade-off you cannot state plainly is one you have not
   finished understanding.
4. **The trade-off is signposted where the user reads it**, in three places as
   applicable:
   - **README.md** — one clear sentence on the feature bullet, so it is visible
     while deciding to use the feature.
   - **AGENTS.md / docs/PARITY.md** — the full explanation, naming the cost
     explicitly, in the section that owns the behaviour.
   - **The UI** — at the point of choice, if the user selects it at runtime.

   The model for this is the PGP key-custody wording, and the two accepted costs
   of delegating custody to `gpg-agent` written out flatly in `AGENTS.md` §4a.
   Aim for that. Write what the user loses, not a reassurance.
5. **New attack surface is justified.** A new dependency, a new outbound network
   call, a new stored secret, or a new file this app writes each need a
   paragraph on why it is necessary and what it can reach. Dependencies that
   parse hostile input (MIME, vCard, OpenPGP, HTML, QR) get the highest
   scrutiny — prefer Qt, then an already-present dependency, then nothing.
6. **The blast radius is bounded and stated.** If it goes wrong, say what is
   reachable: one message, one account, or the device.

### Rejected on principle

- Anything that archives, deletes, or moves mail without an explicit human
  action for that message.
- Any trust decision derived from a keyword label.
- IMAP or SMTP client code. This is a relay-only client; `mail.urlxl.com` is the
  sole transport (`AGENTS.md` §4).
- Convenience defaults that silently disable a protection — treating an
  unreadable keyring as an empty one, downgrading an encrypted send to
  plaintext, following a cross-host redirect with the device secret attached,
  or rendering wire text as `Text.AutoText`.
- Telemetry, analytics, crash reporting, or any phone-home. The device talks to
  the relay the user paired with and the push distributor they chose. Nothing
  else.
- Sending mail content, subjects, or credentials to a third-party service the
  user did not explicitly configure — including AI services, and including a
  public push broker.
- Weakening a gate to make CI or a review pass, or deleting a `tests/guards.tsv`
  row to make `verify-guards.sh` quiet.
- A convenience feature whose security cost cannot be explained to a
  non-expert user in two sentences. If it cannot be explained, it cannot be
  consented to.

### If your trade-off is legitimate

Plenty are. Importing recipient keys into the user's own GnuPG keyring modifies
state this app does not own, and it ships, because gpg owning the record is
better than a second and weaker copy of key-change rules — with the consequences
written down where the next person will find them. That is the standard: not "no
trade-offs," but **no unmarked trade-offs, and never as the default.**

## Documentation: The DOX Chain

`AGENTS.md` files are binding contracts for their subtrees. Before editing,
walk from the repository root to each file you intend to touch and read every
`AGENTS.md` on the way. After editing, update the closest owning `AGENTS.md` if
your change affected purpose, scope, contracts, workflows, inputs/outputs,
constraints, or side effects — and refresh any affected Child DOX Index.

Read the DOX section of the root [AGENTS.md](AGENTS.md) in the session you are
working in. Do not work from memory of it.

Two documents are the record of what exists *today*: `AGENTS.md` and
[`docs/PARITY.md`](docs/PARITY.md). `Linux_QT_Client_Plan.md` is the original
design record and is stale wherever it disagrees with them. If your change makes
a document wrong, fixing it is part of the change, not a follow-up.

## Commits, Branches, and PRs

- Branch off `main`. Name it for the work: `fix/draft-expiry-bounds`,
  `feat/carddav-groups`.
- Conventional Commits for the subject: `fix:`, `feat:`, `docs:`, `refactor:`,
  `test:`, `chore:`. Subject ≤ 50 characters, imperative mood.
- The body explains **why**, not what — the diff already says what. Skip the
  body when the why is obvious from the subject.
- Include the AI attribution trailer where it applies.
- One logical change per PR. A security fix and a refactor in one diff is two
  PRs, because the refactor will be reviewed less carefully than it should be.
- Link the issue the PR closes.

## PR Checklist

Paste this into your PR description and fill it in.

```markdown
### What and why


### AI involvement
- Level: None / Assisted / Generated / Agentic
- Tools:
- What I verified myself:

### Gates
- [ ] All CI jobs green (ci.yml and flatpak.yml)
- [ ] Ran ctest locally, and the sanitizer build if this touches crypto,
      buffer arithmetic, or a value read out of the database
- [ ] New logic has tests; security-sensitive changes have attack-path tests
- [ ] QML-side security controls have a QML test
- [ ] tests/guards.tsv updated, and ./scripts/verify-guards.sh run, if this
      adds or touches a guard
- [ ] Ran adversarial review; findings addressed or argued below
- [ ] DOX pass done — AGENTS.md and any affected doc updated

### Adversarial review findings
<!-- Skills run, and every finding not fixed, with your reasoning. -->

### Security
- [ ] No change to defaults that weakens a protection
- [ ] Fails closed on a missing, unreadable, or partial answer
- Trade-off introduced (if any), in one sentence:
- Documented in: README.md / AGENTS.md / docs/PARITY.md / UI / n-a
- [ ] Does not archive, delete, or move mail without an explicit user action
- [ ] No trust decision derived from a keyword label
- New dependencies / network calls / stored secrets, and why:
```

## Licence

KyPost is licensed under the MIT License. By
contributing, you agree that your contribution is licensed under the same
terms, and that you have the right to submit it — including the right to submit
anything an AI tool produced on your behalf.
