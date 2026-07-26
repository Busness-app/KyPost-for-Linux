#pragma once

// Stops a Q_INVOKABLE that blocks on HttpClient from being entered a second
// time while the first call is still inside its nested QEventLoop.
//
// Why this is needed at all: HttpClient::waitForReply runs a local
// QEventLoop (see core/net/HttpClient.h). A nested event loop is worse than
// blocking -- the UI keeps delivering input, timers keep firing, and
// bindings keep re-evaluating -- so any controller method that makes a
// network call can be re-entered from QML while it is suspended mid-call.
// Every such method mutates controller state, so an interleaved second call
// can leave the first one reading state that belongs to the second. That is
// not hypothetical: MailController::sendMail was patched with a per-send
// token after exactly this produced a message delivered against the wrong
// composition's recipient list.
//
// The real fix is to move HttpClient off the GUI thread; this is the guard
// that makes the current design safe until that happens, and it stays useful
// afterwards for anything that must not overlap with itself.
//
// Usage -- first statement of the invokable, before any state is touched:
//
//     ReentrancyGuard guard(m_inNetworkCall);
//     if (!guard.entered())
//         return false;
//
// Scope-based, so the flag is cleared on every exit path including an
// exception unwinding through the nested loop.
class ReentrancyGuard
{
public:
    explicit ReentrancyGuard(bool& flag)
        : m_flag(flag)
        , m_entered(!flag)
    {
        if (m_entered)
            m_flag = true;
    }

    ~ReentrancyGuard()
    {
        if (m_entered)
            m_flag = false;
    }

    ReentrancyGuard(const ReentrancyGuard&) = delete;
    ReentrancyGuard& operator=(const ReentrancyGuard&) = delete;

    // False when the guarded section was already active -- the caller must
    // return without doing anything.
    bool entered() const { return m_entered; }

private:
    bool& m_flag;
    bool m_entered;
};
