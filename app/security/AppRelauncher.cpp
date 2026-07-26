#include "security/AppRelauncher.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QProcess>
#include <QStringList>
#include <QUrl>

namespace {
bool s_relaunchPending = false;
}

namespace AppRelauncher {

void requestRelaunch()
{
    s_relaunchPending = true;
    if (QCoreApplication::instance())
        QCoreApplication::quit();
}

bool relaunchPending()
{
    return s_relaunchPending;
}

bool performPendingRelaunch()
{
    if (!s_relaunchPending)
        return true;
    s_relaunchPending = false;

    // arguments().mid(1) drops argv[0]; applicationFilePath() is the real
    // binary even when argv[0] was something else.
    //
    // A kypost://native-pair URL is NOT carried over, unlike every other
    // argument. It carries a live pairing token, and the relaunch paths are
    // the security-sensitive ones -- Hostile Location Protection and the
    // wipe-after-10-failures reset -- so re-running the pairing flow
    // automatically, unattended, in the fresh process is the last thing
    // either of them should do. Losing a deep link the user had already
    // acted on costs one click; replaying it does not.
    QStringList arguments;
    for (const QString& argument : QCoreApplication::arguments().mid(1)) {
        if (QUrl(argument).scheme() == QStringLiteral("kypost"))
            continue;
        arguments.append(argument);
    }

    // Checked, not fire-and-forget: a failed spawn here means the app simply
    // vanishes -- right after wiping the user's local data, in the case the
    // wipe path triggers -- with nothing on screen to explain it.
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments)) {
        qCritical("AppRelauncher: failed to relaunch %s -- start KyPost again manually",
                  qPrintable(QCoreApplication::applicationFilePath()));
        return false;
    }
    return true;
}

} // namespace AppRelauncher
