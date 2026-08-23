#!/usr/bin/env bash
#
# Builds SQLCipher into a prefix, with the exact flags this app needs.
#
# WHY WE BUILD IT RATHER THAN INSTALL A PACKAGE
#
# Debian/Ubuntu ship SQLCipher renamed: the library is libsqlcipher.so.0, not
# libsqlite3.so.0. That rename is the whole problem. Qt's stock SQLite driver
# plugin (libqsqlite.so) has a DT_NEEDED on libsqlite3.so.0, so a differently
# named SQLCipher does not satisfy it -- the loader pulls in the SYSTEM sqlite
# as well, and two sqlite implementations end up mapped in one process.
# Measured, not assumed: with a renamed/soname-less build, `LD_DEBUG=libs`
# shows both libraries initialising.
#
# Built here with -Wl,-soname,libsqlite3.so.0, the plugin's dependency is
# already satisfied by the time it loads and the system sqlite is never
# mapped at all -- verified the same way, one `calling init` line.
#
# WHY EACH FLAG IS HERE
#
#   SQLITE_HAS_CODEC / EXTRA_INIT / EXTRA_SHUTDOWN
#       Required by SQLCipher itself. Omit them and the build stops with
#       "#error SQLCipher must be compiled with ...".
#
#   SQLITE_ENABLE_COLUMN_METADATA
#       Qt's driver imports sqlite3_column_table_name16, which only exists
#       with this defined. Without it the plugin fails to load at all and
#       QSqlDatabase reports the unhelpful "Driver not loaded".
#
#   SQLITE_TEMP_STORE=2 / --with-tempstore=yes
#       Temporary tables and sort spill files go to RAM, not to an unencrypted
#       file next to the database. An encrypted database whose temp spill is
#       plaintext is not an encrypted database.
#
set -euo pipefail

VERSION="${SQLCIPHER_VERSION:-4.14.0}"
SHA256="67fb27e967a4a6968c0905691c89c908e7250dddc581b887c19ef981c737e473"
PREFIX="${1:?usage: build-sqlcipher.sh <install-prefix>}"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "${WORKDIR}"' EXIT

TARBALL="${WORKDIR}/sqlcipher-${VERSION}.tar.gz"
curl -fsSL -o "${TARBALL}" \
  "https://github.com/sqlcipher/sqlcipher/archive/refs/tags/v${VERSION}.tar.gz"

# Checked, not trusted. This is the crypto layer for every cached message on
# the device; a tarball fetched over TLS from a host we do not control is not
# on its own a reason to compile it.
echo "${SHA256}  ${TARBALL}" | sha256sum -c -

tar xzf "${TARBALL}" -C "${WORKDIR}"
cd "${WORKDIR}/sqlcipher-${VERSION}"

# One line, deliberately. Wrapping this with backslash-continuations inside
# the quotes embeds the newlines in the value, configure takes only part of
# it, and the build silently comes out without column metadata -- which the
# check at the bottom then catches. It caught it once already.
./configure \
  --prefix="${PREFIX}" \
  --with-tempstore=yes \
  --disable-tcl \
  CFLAGS="-DSQLITE_HAS_CODEC -DSQLITE_TEMP_STORE=2 -DSQLITE_EXTRA_INIT=sqlcipher_extra_init -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown -DSQLITE_ENABLE_COLUMN_METADATA" \
  LDFLAGS="-lcrypto -Wl,-soname,libsqlite3.so.0"

make -j"$(nproc)"
make install

# Fail here rather than at runtime. Both of these have already been shipped
# wrong once in this spike: a build with no soname loads a second sqlite
# alongside ours, and a build without column metadata makes Qt refuse to load
# its own driver.
SO="$(ls "${PREFIX}"/lib/libsqlite3.so.*.* | head -1)"

# `grep`, not `grep -q`, and that is not a style choice. Under `set -o
# pipefail`, grep -q exits the moment it matches, the producer on the left
# gets SIGPIPE, and the PIPELINE reports failure -- so a check that found
# exactly what it was looking for fails. This script reported "built without
# SQLITE_ENABLE_COLUMN_METADATA" against a library that had the symbol.
# Plain grep reads its input to the end and has nothing to trip over.
objdump -p "${SO}" | grep "SONAME *libsqlite3.so.0" > /dev/null \
  || { echo "FATAL: built library has no libsqlite3.so.0 SONAME" >&2; exit 1; }
nm -D --defined-only "${SO}" | grep "sqlite3_column_table_name16" > /dev/null \
  || { echo "FATAL: built without SQLITE_ENABLE_COLUMN_METADATA" >&2; exit 1; }

echo "SQLCipher ${VERSION} installed to ${PREFIX}"
