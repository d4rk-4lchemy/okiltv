#pragma once

#include <QObject>

#include <memory>

class QSystemTrayIcon;
class QMenu;
class QAction;

namespace OKILTV::App {

class TrayController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)

public:
    explicit TrayController(QObject *parent = nullptr);
    ~TrayController() override;

    bool available() const;

    Q_INVOKABLE void showTrayIcon();
    Q_INVOKABLE void hideTrayIcon();

signals:
    void showRequested();
    void exitRequested();

private:
    std::unique_ptr<QSystemTrayIcon> m_trayIcon;
    std::unique_ptr<QMenu> m_menu;
    std::unique_ptr<QAction> m_showAction;
    std::unique_ptr<QAction> m_exitAction;
};

} // namespace OKILTV::App
