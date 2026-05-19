#pragma once

#include <QObject>
#include <QTimer>

namespace OKILTV::App {

class DisplaySleepBlocker final : public QObject
{
    Q_OBJECT

public:
    explicit DisplaySleepBlocker(QObject *parent = nullptr);
    ~DisplaySleepBlocker() override;

public slots:
    void setBlocked(bool blocked);

private:
    bool m_blocked { false };

#if defined(Q_OS_WIN)
    QTimer m_windowsRefreshTimer;
#endif

#if defined(OKILTV_HAS_DBUS_SLEEP_BLOCKER)
    uint m_inhibitCookie { 0 };
#endif
};

} // namespace OKILTV::App
