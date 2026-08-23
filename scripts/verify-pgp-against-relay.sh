#!/usr/bin/env bash
#
# Checks that what this client would put on the wire is what the relay will
# actually accept -- by running the relay's OWN decoder and validators over
# it, not a second opinion written in this repo.
#
# Three things, in increasing order of how much they cover:
#   * one PGP/MIME delivery, against validatePGPMimeDeliveryShape and
#     validatePGPMimeDelivery and the SMTP normaliser;
#   * one protected-headers content part, against ExtractProtectedSubject,
#     so a non-ASCII subject is shown to round-trip;
#   * the WHOLE send request, decoded into this server's own
#     clientEncryptedSendRequest and run through its per-delivery validation
#     and its Sent-copy logic.
#
# The third is the closest thing to sending for real that does not need a live
# relay. It is not a substitute for one: nothing here proves SMTP delivery,
# IMAP APPEND, or that the deployed server is this version.
#
# POST /api/mail/send-pgp relays a delivery's bytes verbatim and refuses
# anything that is not a complete RFC 5322 message. Re-deriving those rules
# here would give exactly the second, weaker copy that divergence is made of,
# so this drives the originals.
#
# Proven to discriminate rather than nod, by tampering: putting the real
# subject on the envelope, clearing sentCopyEncrypted, and using a From the
# caller may not send as are each rejected, in the server's own words.
#
# NOT part of ctest: it needs a checkout of the server repo and a Go
# toolchain, neither of which CI has. Run it by hand when the writer changes.
#
# It writes ONE temporary _test.go into that checkout and removes it again on
# every exit path, and refuses to start if the checkout has uncommitted work,
# so an interrupted run cannot be mistaken for the user's own edits.
set -euo pipefail

SERVER_REPO="${1:-$(cd "$(dirname "$0")/../.." && pwd)/kypost-server}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
PROBE=""

cleanup() {
    if [ -n "$PROBE" ] && [ -f "$PROBE" ]; then
        rm -f "$PROBE"
        echo "removed the temporary probe from the server checkout"
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ ! -d "$SERVER_REPO/backend/internal/api" ]; then
    echo "no server checkout at $SERVER_REPO -- pass its path as the first argument" >&2
    exit 1
fi

# Refuse to touch a checkout with work in it. This script adds and deletes a
# file there; doing that alongside someone's uncommitted changes is how a
# cleanup step deletes the wrong thing.
if [ -n "$(git -C "$SERVER_REPO" status --porcelain)" ]; then
    echo "$SERVER_REPO has uncommitted changes; refusing to write a probe into it" >&2
    exit 1
fi

echo "building the fixture emitter"
g++ -std=c++20 -fPIC -O1 -w \
    $(pkg-config --cflags Qt6Core) \
    -I "$REPO_ROOT/core" \
    "$REPO_ROOT/scripts/pgp-delivery-fixture.cpp" \
    "$REPO_ROOT/core/pgp/PgpMimeWriter.cpp" \
    "$REPO_ROOT/core/net/PgpSendRequest.cpp" \
    -o "$WORK/emit" \
    $(pkg-config --libs Qt6Core Qt6Network)

"$WORK/emit" "$WORK/delivery.eml" "$WORK/protected.mime" "$WORK/request.json"
echo "emitted a delivery, a protected content part and a whole send request"

PROBE="$SERVER_REPO/backend/internal/api/zz_kypost_linux_shape_probe_test.go"
cat > "$PROBE" <<GOEOF
package api

// TEMPORARY probe written by kypost-Linux/scripts/verify-pgp-delivery-shape.sh.
// If this file is still here, that script did not finish; delete it.

import (
	"encoding/json"
	"os"
	"strings"
	"testing"

	"kypost-server/backend/internal/mailmsg"
	"kypost-server/backend/internal/pgpmail"
)

func TestKypostLinuxDeliveryShape(t *testing.T) {
	raw, err := os.ReadFile("$WORK/delivery.eml")
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if err := validatePGPMimeDeliveryShape(string(raw)); err != nil {
		t.Fatalf("shape rejected: %v", err)
	}
	if err := validatePGPMimeDelivery(string(raw), "me@example.com"); err != nil {
		t.Fatalf("authorized From rejected: %v", err)
	}
	if _, err := mailmsg.PrepareSMTPMessage(raw); err != nil {
		t.Fatalf("SMTP preparation rejected: %v", err)
	}
	// The check discriminates: a From this caller may not use is still refused.
	if err := validatePGPMimeDelivery(string(raw), "someone-else@example.com"); err == nil {
		t.Fatal("an unauthorized From was accepted")
	}
}

// The WHOLE request, through this server's own decoder and its own Sent-copy
// logic -- not just one delivery's MIME. This is the closest thing to sending
// for real that does not need a live relay.
func TestKypostLinuxSendRequest(t *testing.T) {
	raw, err := os.ReadFile("$WORK/request.json")
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	var req clientEncryptedSendRequest
	if err := json.Unmarshal(raw, &req); err != nil {
		t.Fatalf("this server cannot decode the request: %v", err)
	}
	if len(req.Deliveries) == 0 {
		t.Fatal("no deliveries survived decoding")
	}
	for i, d := range req.Deliveries {
		if len(d.Recipients) == 0 {
			t.Fatalf("delivery %d has no recipients", i)
		}
		if _, rerr := parseDeliveryRecipients(d.Recipients); rerr != nil {
			t.Fatalf("delivery %d recipients rejected: %v", i, rerr)
		}
		if verr := validatePGPMimeDelivery(strings.TrimSpace(d.Ciphertext), req.From); verr != nil {
			t.Fatalf("delivery %d rejected: %v", i, verr)
		}
	}
	// The blind recipient gets their own delivery, which is why the wire
	// format is a list at all.
	if len(req.Deliveries) < 2 {
		t.Fatalf("expected a separate delivery per blind recipient, got %d", len(req.Deliveries))
	}
	// The real subject never travels outside the ciphertext.
	if req.Subject != pgpmail.OuterPlaceholderSubject {
		t.Fatalf("outer subject is not the placeholder: %q", req.Subject)
	}
	// And the Sent copy is stored, which this server does only when the
	// client asserts it is ciphertext.
	if _, ok := sentCopyDraft(req); !ok {
		t.Fatal("this server would not store the Sent copy this client sent")
	}
}

func TestKypostLinuxProtectedSubject(t *testing.T) {
	inner, err := os.ReadFile("$WORK/protected.mime")
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	subject, ok := pgpmail.ExtractProtectedSubject(inner)
	if !ok {
		t.Fatal("no protected subject found")
	}
	if subject != "Café — Redundancies confirmed" {
		t.Fatalf("protected subject round-tripped wrong: %q", subject)
	}
}
GOEOF

echo "running the relay's own validators"
( cd "$SERVER_REPO/backend" && go test ./internal/api/ -run TestKypostLinux -count=1 -v ) \
    | grep -E "^(=== RUN|--- (PASS|FAIL)|ok|FAIL)" || true

( cd "$SERVER_REPO/backend" && go test ./internal/api/ -run TestKypostLinux -count=1 > /dev/null )
echo "the relay decodes this client's send request, accepts every delivery, and would store the Sent copy"
