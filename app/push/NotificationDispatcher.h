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
    // tier (Task 43 review-finding fix) was added for the embedded ntfy
    // subscriber tier, whose arrivals only ever carried payload.title/
    // payload.body; that tier was removed on 2026-07-26 (see
    // core/domain/TransportStateMachine.h) but the fallback is kept as
    // defence in depth for any payload that reaches here with only those two
    // fields set. Confirmed safe for the Distributor tier:
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

    // Pure, deterministic fallback-selection logic backing notify()'s
    // title/text: senderName/emailSubject first, then sender/subject, then
    // (Task 43 review-finding fix) title/body when both prior fields are
    // empty. Public and static -- rather than anonymous-namespace
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
    // malicious/compromised relay or ntfy publisher can't plant a phishing
    // link or tracking-pixel image in the desktop notification. Public and
    // static for the same testability reason as pickTitle/pickText above.
    static QString sanitizeForNotification(const QString& text);

signals:
    // Emitted when the user activates the notification's default ("View")
    // action.
    void openRequested(const QString& messageId);
};
