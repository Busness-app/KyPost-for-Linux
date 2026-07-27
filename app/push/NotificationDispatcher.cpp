#include "push/NotificationDispatcher.h"

#include "models/PushNotification.h"

#include <KLocalizedString>
#include <KNotification>

#include <QDebug>

NotificationDispatcher::NotificationDispatcher(QObject* parent)
    : QObject(parent)
{
}

QString NotificationDispatcher::pickTitle(const PushNotification& payload)
{
    if (!payload.senderName.isEmpty())
        return payload.senderName;
    if (!payload.sender.isEmpty())
        return payload.sender;
    // Task 43 review-finding fix: last-resort fallback for tiers (currently
    // only EmbeddedSubscriber, via NtfySubscriber's flat {title,message}
    // envelope) that never populate senderName/sender at all. See the
    // header comment for why this never changes the Distributor tier's
    // already-correct rendering.
    return payload.title;
}

QString NotificationDispatcher::pickText(const PushNotification& payload)
{
    if (!payload.emailSubject.isEmpty())
        return payload.emailSubject;
    if (!payload.subject.isEmpty())
        return payload.subject;
    return payload.body;
}

QString NotificationDispatcher::sanitizeForNotification(const QString& text)
{
    return text.toHtmlEscaped();
}

// The app lock exists to stop someone at an unattended machine reading this
// user's mail. A notification popup carrying the sender and subject hands
// over exactly that, on the lock screen, without a PIN -- the same leak
// LockOverlay.qml closes for popped-out windows, through a surface the
// overlay cannot cover, because it is drawn by the notification server and
// not by this process. While hidden the popup keeps its arrival signal and
// loses its content; the "View" action still works and still lands behind
// the PIN prompt.
//
// Split out of notify() as pure statics for the same reason pickTitle/
// pickText are: notify() itself needs KNotification and a live D-Bus
// notification server, so this is the only way the redaction rule gets a
// runnable check.
QString NotificationDispatcher::displayTitle(const PushNotification& payload, bool contentHidden)
{
    if (contentHidden)
        return i18n("KyPost");
    return sanitizeForNotification(pickTitle(payload));
}

QString NotificationDispatcher::displayText(const PushNotification& payload, bool contentHidden)
{
    if (contentHidden)
        return i18n("New mail — unlock to read");
    return sanitizeForNotification(pickText(payload));
}

bool NotificationDispatcher::contentHidden() const
{
    return m_contentHidden;
}

void NotificationDispatcher::setContentHidden(bool hidden)
{
    m_contentHidden = hidden;
}

void NotificationDispatcher::notify(const PushNotification& payload)
{
    // Logging discipline (Phase 7 global constraint 6): never log
    // payload.body/subject/senderName/emailSubject content, only messageId
    // (an opaque identifier, not message content) plus a bare arrival
    // marker.
    qDebug() << "NotificationDispatcher: notifying for messageId" << payload.messageId;

    const QString title = displayTitle(payload, m_contentHidden);
    const QString text = displayText(payload, m_contentHidden);

    // No parent: matches KNotification's own documented lifecycle -- with
    // the default CloseOnTimeout flag it deletes itself once the
    // notification closes.
    auto* notification = new KNotification(QStringLiteral("newMail"));
    notification->setTitle(title);
    notification->setText(text);

    const QString messageId = payload.messageId;
    // "View" is this notification's own action-button chrome label, not
    // content derived from the mail message -- the one literal in this file
    // that the task-49 brief's "don't wrap NotificationDispatcher's dynamic
    // title/body" rule doesn't cover (that rule is about pickTitle()/
    // pickText() above, which stay untouched since they return mail
    // content).
    KNotificationAction* viewAction = notification->addDefaultAction(i18n("View"));
    connect(viewAction, &KNotificationAction::activated, this, [this, messageId]() {
        emit openRequested(messageId);
    });

    notification->sendEvent();
}
