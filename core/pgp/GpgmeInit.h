#pragma once

// The single gpgme initialisation point for this library.
//
// gpgme_check_version(nullptr) must run before any other gpgme call, and
// GPGME's own documentation says that FIRST call is not thread-safe. A
// function-local static gives the once-only guarantee -- but only per static,
// and this used to be three of them, one per translation unit
// (OpenPgpDecryptor, OpenPgpEncryptor, OpenPgpKeyImporter). Three separate
// "once" flags are not once: a decryptor on one executor and an importer on
// another can each be running their own first call at the same moment,
// concurrently, through an API that says not to. One static in one place is
// what actually makes the guarantee true.
void ensureGpgmeInitialised();
