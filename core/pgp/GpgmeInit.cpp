#include "pgp/GpgmeInit.h"

#include <gpgme.h>

void ensureGpgmeInitialised()
{
    // Function-local static: the C++ runtime serialises the initialiser, so
    // gpgme_check_version runs exactly once process-wide however many threads
    // arrive here together.
    static const bool initialised = []() {
        gpgme_check_version(nullptr);
        return true;
    }();
    (void)initialised;
}
