#!/usr/bin/env bash
#
# Checks that what core/pgp/PgpMimeWriter.cpp produces is what the relay will
# actually accept -- by running the relay's OWN validators over it, not a
# second opinion written in this repo.
#
# POST /api/mail/send-pgp relays a delivery's bytes verbatim and refuses
# anything that is not a complete RFC 5322 message. Those rules live in the
# server's validatePGPMimeDeliveryShape / validatePGPMimeDelivery and its
# ExtractProtectedSubject. Re-deriving them here would give exactly the second,
# weaker copy that the divergence is made of, so this drives the originals.
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
    -o "$WORK/emit" \
    $(pkg-config --libs Qt6Core)

"$WORK/emit" "$WORK/delivery.eml" "$WORK/protected.mime"
echo "emitted a delivery and a protected content part"

PROBE="$SERVER_REPO/backend/internal/api/zz_kypost_linux_shape_probe_test.go"
cat > "$PROBE" <<GOEOF
package api

// TEMPORARY probe written by kypost-Linux/scripts/verify-pgp-delivery-shape.sh.
// If this file is still here, that script did not finish; delete it.

import (
	"os"
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
echo "the relay accepts this client's PGP/MIME delivery and reads back its protected subject"
