#pragma once

#include "net/NetworkExecutor.h"

// Calls NetworkExecutor::shutdown() while the receiver is still alive.
//
// DECLARE IT AFTER THE CONTROLLER. The executor has to be constructed before
// the controller so it can be handed to it, which means it is destroyed
// AFTER -- so by the time ~NetworkExecutor runs shutdown() the receiver is
// already gone, and any callback still queued is delivered into freed memory.
// This guard, declared after the controller, destructs before it and closes
// that window. See NetworkExecutor::run()'s stated precondition.
//
// A plain `executor.shutdown()` at the end of the test body is not enough:
// QVERIFY/QCOMPARE return from the function on failure, so a FAILING test
// skips it -- which is how this first showed up, as a SIGSEGV inside
// QCoreApplicationPrivate::sendPostedEvents after four unrelated assertion
// failures. A test suite whose failure mode is "crash the runner" hides
// whatever else was about to be reported.
struct ExecutorShutdownGuard
{
    NetworkExecutor& executor;
    ~ExecutorShutdownGuard() { executor.shutdown(); }
};
