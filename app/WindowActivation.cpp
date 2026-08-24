#include "WindowActivation.h"

#include <QQmlApplicationEngine>
#include <QQuickWindow>

void activateMainWindow(QQmlApplicationEngine* engine)
{
    if (engine == nullptr)
        return;

    for (QObject* root : engine->rootObjects()) {
        auto* window = qobject_cast<QQuickWindow*>(root);
        if (window == nullptr)
            continue;

        // All three, in this order, because they fix different states and
        // none of them subsumes the others:
        //
        //   un-minimise  -- show() on a minimised window leaves it minimised,
        //                   so the click would still look like nothing.
        //   show()       -- the window may have been closed outright; the
        //                   process survives that, so this is the only thing
        //                   that brings it back at all.
        //   raise()      -- it may be visible but buried behind other windows.
        //
        // requestActivate() asks for focus on top. A Wayland compositor may
        // decline it without an xdg-activation token, which is why it is last
        // and why the other three do not depend on it: worst case the window
        // is on screen and unfocused, rather than not on screen.
        window->setWindowStates(window->windowStates() & ~Qt::WindowMinimized);
        window->show();
        window->raise();
        window->requestActivate();
        return;
    }
}
