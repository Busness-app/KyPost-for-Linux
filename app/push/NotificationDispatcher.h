#pragma once

#include <QObject>
#include <QString>

struct PushNotification;

// Wraps KNotification (KNotifications/KNotification, KF6::Notifications) to
// surface a parsed PushNotification as a desktop notification. Lives in
// app/push/, alongside UnifiedPushConnector, per the existing core/ boundary
// rule -- KNotifications is a KF6 system library, never linked into
// kypostcore.
//
// Deliberately ignorant of QML/MailController/window focus: Task 42 wires
// openRequested() to real navigation. This class only builds and sends the
// KNotification and forwards its default-action activation as a Qt signal.
class NotificationDispatcher : public QObject
{
    Q_OBJECT
public:
    explicit NotificationDispatcher(QObject* parent = nullptr);

    // Builds and sends a KNotification for payload, with one default
    // ("View") action. Title/text field choice: payload.senderName
    // (falling back to payload.sender, then to payload.title, when empty)
    // as the title, payload.emailSubject (falling back to payload.subject,
    // then to payload.body, when empty) as the text -- matches
    // buildNativePushData's own field-naming intent that senderName/
    // emailSubject are the friendlier display copies, while sender/subject
    // are the raw fallbacks for when those are absent. The final title/body
    // tier exists for the backend's sparser /api/notifications/test envelope,
    // which carries only a flat title/body and none of the mail-shaped
    // fields (see PushPayloadParser::parse, which accepts it deliberately).
    // Do not delete it as dead code: it was originally added for the
    // embedded ntfy subscriber tier, and that tier's removal on 2026-07-26
    // (see core/domain/TransportStateMachine.h) is NOT what this branch is
    // for any more. Confirmed safe for the Distributor tier:
    // backend/internal/processor/poller.go's
    // buildNativePushData duplicates the same title/body values into
    // data.title/data.body that it derives senderName/emailSubject from
    // (buildNativeNotificationText: title="New Email" / body="You have a new
    // email." whenever sender/subject are themselves empty), so this tier's
    // data.title/data.body are never empty when senderName/sender (or
    // emailSubject/subject) are -- this fallback never overrides an
    // already-populated field, it only fires in the same both-empty case
    // that previously rendered blank. Built via pickTitle()/pickText() below.
    void notify(const PushNotification& payload);

    // While true, notify() suppresses the sender and subject and sends a
    // fixed "new mail, unlock to read" popup instead. Driven from
    // AppLockManager::locked in main.cpp -- see notify()'s own comment for
    // why a locked app must not render mail content into a notification.
    bool contentHidden() const;
    void setContentHidden(bool hidden);

    // Pure, deterministic fallback-selection logic backing notify()'s
    // title/text: senderName/emailSubject first, then sender/subject, then
    // title/body when both prior fields are empty. Public and static --
    // rather than anonymous-namespace
    // file-scope helpers, the way PushPayloadParser.cpp's splitKeywords is
    // done -- specifically so NotificationDispatcherTest can call them
    // directly without touching KNotification/D-Bus. notify() itself has no
    // fake/injectable seam to test through (see tests/CMakeLists.txt's
    // Task-40 comment on why NotificationDispatcher has no test exercising
    // notify() end-to-end), so these two functions are this class's only
    // unit-testable surface.
    static QString pickTitle(const PushNotification& payload);
    static QString pickText(const PushNotification& payload);

    // VibeSec fix: KNotification::setText()'s own doc comment states Plasma
    // renders the body as Text.StyledText -- supporting clickable <a href>
    // links and remotely-fetched <img src> images -- whenever the local
    // notification server advertises the "body-markup" capability (which a
    // stock Plasma session does). pickTitle()/pickText() above return raw,
    // attacker-influenceable push content (sender/subject/body); notify()
    // runs both through this before calling setTitle()/setText() so a
    // malicious or compromised relay can't plant a phishing link or
    // tracking-pixel image in the desktop notification. Public and
    // static for the same testability reason as pickTitle/pickText above.
    //
    // Applies to the BODY only -- see sanitizeTitleForNotification below for
    // why the summary is a different question.
    static QString sanitizeForNotification(const QString& text);

    // The summary (setTitle) half, and deliberately NOT HTML escaping.
    //
    // "body-markup" is exactly what it says: the freedesktop Desktop
    // Notifications spec defines the summary as a single line of plain text
    // and the capability governs the body only. Running the title through
    // toHtmlEscaped() therefore escaped nothing dangerous and instead
    // corrupted ordinary content -- a message from "Smith & Jones" was
    // displayed as "Smith &amp; Jones", every time.
    //
    // What the summary DOES need is control-character stripping: it is
    // specified as one line, and a sender name carrying a newline (or a
    // terminal escape, on a notification server that passes them through)
    // can push text into the region the user reads as the body.
    static QString sanitizeTitleForNotification(const QString& text);

    // What notify() actually passes to setTitle()/setText(): the sanitized
    // pickTitle()/pickText() result, or fixed non-revealing copy when
    // contentHidden. Pure and static so the redaction rule is testable
    // without a notification server -- see the .cpp for why it matters.
    static QString displayTitle(const PushNotification& payload, bool contentHidden);
    static QString displayText(const PushNotification& payload, bool contentHidden);

signals:
    // Emitted when the user activates the notification's default ("View")
    // action.
    void openRequested(const QString& messageId);

private:
    bool m_contentHidden = false;
};
