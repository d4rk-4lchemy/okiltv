#include "traycontroller.h"

#include <QAction>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>

namespace OKILTV::App {

TrayController::TrayController(QObject *parent)
    : QObject(parent)
    , m_trayIcon(std::make_unique<QSystemTrayIcon>(QIcon(QStringLiteral(":/resources/icons/app.png")), this))
    , m_menu(std::make_unique<QMenu>())
    , m_showAction(std::make_unique<QAction>(QStringLiteral("Show"), this))
    , m_exitAction(std::make_unique<QAction>(QStringLiteral("Exit"), this))
{
    m_menu->addAction(m_showAction.get());
    m_menu->addSeparator();
    m_menu->addAction(m_exitAction.get());
    m_trayIcon->setContextMenu(m_menu.get());
    m_trayIcon->setToolTip(QStringLiteral("OKILTV"));

    connect(m_showAction.get(), &QAction::triggered, this, &TrayController::showRequested);
    connect(m_exitAction.get(), &QAction::triggered, this, &TrayController::exitRequested);
    connect(m_trayIcon.get(), &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            emit showRequested();
        }
    });
}

TrayController::~TrayController()
{
    hideTrayIcon();
}

bool TrayController::available() const
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

void TrayController::showTrayIcon()
{
    if (!available()) {
        return;
    }
    if (!m_trayIcon->isVisible()) {
        m_trayIcon->show();
    }
}

void TrayController::hideTrayIcon()
{
    if (m_trayIcon && m_trayIcon->isVisible()) {
        m_trayIcon->hide();
    }
}

} // namespace OKILTV::App
