#include "security/AppRelauncher.h"

#include <QCoreApplication>
#include <QProcess>

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

void performPendingRelaunch()
{
    if (!s_relaunchPending)
        return;
    s_relaunchPending = false;

    // arguments().mid(1) drops argv[0]; applicationFilePath() is the real
    // binary even when argv[0] was something else. Any kypost:// URL that
    // launched this instance is deliberately carried over too -- if the user
    // toggled a setting from a deep-link-launched window, dropping it would
    // silently discard whatever they were doing.
    QProcess::startDetached(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1));
}

} // namespace AppRelauncher
