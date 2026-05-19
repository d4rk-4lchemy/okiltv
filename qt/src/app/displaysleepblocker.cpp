#include "displaysleepblocker.h"

#include "../core/debuglogger.h"

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

#if defined(OKILTV_HAS_DBUS_SLEEP_BLOCKER)
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#endif

namespace OKILTV::App {

#if defined(Q_OS_WIN)
namespace {

bool applyWindowsExecutionState(const bool blocked)
{
    const auto state = blocked
        ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)
        : ES_CONTINUOUS;
    return SetThreadExecutionState(state) != 0;
}

} // namespace
#endif

DisplaySleepBlocker::DisplaySleepBlocker(QObject *parent)
    : QObject(parent)
{
#if defined(Q_OS_WIN)
    m_windowsRefreshTimer.setInterval(30000);
    m_windowsRefreshTimer.setSingleShot(false);
    connect(&m_windowsRefreshTimer, &QTimer::timeout, this, [this]() {
        if (!m_blocked) {
            return;
        }

        if (!applyWindowsExecutionState(true)) {
            Core::DebugLogger::instance().log(
                QStringLiteral("power"),
                QStringLiteral("SetThreadExecutionState refresh failed while display sleep blocking is active."));
        }
    });
#endif
}

DisplaySleepBlocker::~DisplaySleepBlocker()
{
    setBlocked(false);
}

void DisplaySleepBlocker::setBlocked(const bool blocked)
{
#if defined(Q_OS_WIN)
    if (m_blocked == blocked && (!blocked || m_windowsRefreshTimer.isActive())) {
        return;
    }

    if (!applyWindowsExecutionState(blocked)) {
        Core::DebugLogger::instance().log(
            QStringLiteral("power"),
            QStringLiteral("SetThreadExecutionState failed while %1 display sleep blocking.")
                .arg(blocked ? QStringLiteral("enabling") : QStringLiteral("clearing")));
    }
    if (blocked) {
        if (!m_windowsRefreshTimer.isActive()) {
            m_windowsRefreshTimer.start();
        }
    } else {
        m_windowsRefreshTimer.stop();
    }
#elif defined(OKILTV_HAS_DBUS_SLEEP_BLOCKER)
    if (m_blocked == blocked) {
        return;
    }

    QDBusInterface interface(
        QStringLiteral("org.freedesktop.ScreenSaver"),
        QStringLiteral("/ScreenSaver"),
        QStringLiteral("org.freedesktop.ScreenSaver"),
        QDBusConnection::sessionBus());

    if (!interface.isValid()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("power"),
            QStringLiteral("org.freedesktop.ScreenSaver is unavailable; display sleep blocking skipped."));
    } else if (blocked) {
        QDBusReply<uint> reply = interface.call(
            QStringLiteral("Inhibit"),
            QStringLiteral("OKILTV"),
            QStringLiteral("Stream playback in progress"));
        if (reply.isValid()) {
            m_inhibitCookie = reply.value();
        } else {
            Core::DebugLogger::instance().log(
                QStringLiteral("power"),
                QStringLiteral("ScreenSaver Inhibit failed: %1").arg(reply.error().message()));
        }
    } else if (m_inhibitCookie != 0) {
        QDBusReply<void> reply = interface.call(QStringLiteral("UnInhibit"), m_inhibitCookie);
        if (!reply.isValid()) {
            Core::DebugLogger::instance().log(
                QStringLiteral("power"),
                QStringLiteral("ScreenSaver UnInhibit failed: %1").arg(reply.error().message()));
        }
        m_inhibitCookie = 0;
    }
#else
    if (m_blocked == blocked) {
        return;
    }
#endif

    m_blocked = blocked;
}

} // namespace OKILTV::App
