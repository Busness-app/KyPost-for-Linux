#include "tray/TrayController.h"

#include "general/GeneralController.h"

#include <KStatusNotifierItem>
#include <QCoreApplication>
#include <QWindow>

TrayController::TrayController(QWindow* window, GeneralController& general, QObject* parent)
    : QObject(parent)
    , m_window(window)
    , m_general(general)
{
    connect(&general, &GeneralController::trayIconEnabledChanged, this, &TrayController::setEnabled);
    setEnabled(general.trayIconEnabled());
}

TrayController::~TrayController() = default;

void TrayController::setEnabled(bool enabled)
{
    if (enabled == static_cast<bool>(m_item))
        return;

    if (!enabled) {
        m_item.reset();
        return;
    }

    m_item = std::make_unique<KStatusNotifierItem>(QStringLiteral("com.kysecurity.mail"), this);
    m_item->setIconByName(QStringLiteral("com.kysecurity.mail"));
    m_item->setTitle(QStringLiteral("KyPost"));
    m_item->setCategory(KStatusNotifierItem::Communications);
    m_item->setStatus(KStatusNotifierItem::Active);
    m_item->setAssociatedWindow(m_window);
    m_item->setStandardActionsEnabled(true);

    connect(m_item.get(), &KStatusNotifierItem::quitRequested, qApp, &QCoreApplication::quit);
    connect(m_item.get(), &KStatusNotifierItem::activateRequested, this, [this](bool active, const QPoint&) {
        if (active) {
            m_window->show();
            m_window->raise();
            m_window->requestActivate();
        } else {
            m_window->hide();
        }
    });
}
