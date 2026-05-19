#include "mpvvideoitem.h"

#include "../app/playercontroller.h"
#include "../core/debuglogger.h"

#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QOpenGLFunctions>
#include <QMetaObject>
#include <QPointer>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>
#include <QSize>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <chrono>

namespace OKILTV::Player {

namespace {

constexpr int kWindowedRenderWatchdogIntervalMs = 250;
constexpr int kWindowedRenderRecoveryIntervalMs = 16;
constexpr qint64 kWindowedRenderRecoveryThresholdMs = 200;

bool envFlagEnabled(const char *name)
{
    const auto value = qEnvironmentVariable(name).trimmed().toLower();
    return value == QStringLiteral("1")
        || value == QStringLiteral("true")
        || value == QStringLiteral("yes")
        || value == QStringLiteral("on");
}

QSize itemRenderSize(const MpvVideoItem *item)
{
    if (item == nullptr) {
        return {};
    }

    const auto logicalWidth = std::max(1, static_cast<int>(std::round(item->width())));
    const auto logicalHeight = std::max(1, static_cast<int>(std::round(item->height())));
    const auto *quickWindow = item->window();
    const auto dpr = quickWindow != nullptr ? quickWindow->effectiveDevicePixelRatio() : 1.0;

    return {
        std::max(1, static_cast<int>(std::round(static_cast<double>(logicalWidth) * dpr))),
        std::max(1, static_cast<int>(std::round(static_cast<double>(logicalHeight) * dpr)))
    };
}

qint64 monotonicNowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

QString visibilityLabel(const QWindow::Visibility visibility)
{
    switch (visibility) {
    case QWindow::Hidden:
        return QStringLiteral("Hidden");
    case QWindow::AutomaticVisibility:
        return QStringLiteral("Automatic");
    case QWindow::Windowed:
        return QStringLiteral("Windowed");
    case QWindow::Minimized:
        return QStringLiteral("Minimized");
    case QWindow::Maximized:
        return QStringLiteral("Maximized");
    case QWindow::FullScreen:
        return QStringLiteral("FullScreen");
    }

    return QStringLiteral("Unknown");
}

} // namespace

class MpvVideoRenderer final : public QQuickFramebufferObject::Renderer, protected QOpenGLFunctions
{
public:
    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        return new QOpenGLFramebufferObject(size, format);
    }

    void synchronize(QQuickFramebufferObject *item) override
    {
        m_item = static_cast<MpvVideoItem *>(item);
        const auto nextRenderSize = itemRenderSize(m_item);
        if (m_lastRenderSize != nextRenderSize) {
            if (m_lastRenderSize.isValid()) {
                Core::DebugLogger::instance().log(
                    QStringLiteral("video"),
                    QStringLiteral("FBO resize detected for %1: %2x%3 -> %4x%5, invalidating framebuffer.")
                        .arg(m_item != nullptr ? m_item->objectName() : QStringLiteral("<unknown>"))
                        .arg(m_lastRenderSize.width())
                        .arg(m_lastRenderSize.height())
                        .arg(nextRenderSize.width())
                        .arg(nextRenderSize.height()));
                invalidateFramebufferObject();
                if (m_item != nullptr) {
                    QMetaObject::invokeMethod(m_item, "requestUpdate", Qt::QueuedConnection);
                }
            }
            m_lastRenderSize = nextRenderSize;
        }

        m_playerController = qobject_cast<App::PlayerController *>(m_item->playerController());
        m_player = m_playerController != nullptr
            ? m_playerController->player()
            : qobject_cast<MpvPlayer *>(m_item->playerObject());
        if (m_player != nullptr) {
            m_player->setRenderUpdateTarget(m_item);
        }
    }

    void render() override
    {
        if (!m_initialized) {
            initializeOpenGLFunctions();
            m_initialized = true;
        }

        if (m_player == nullptr || framebufferObject() == nullptr) {
            return;
        }

        QQuickOpenGLUtils::resetOpenGLState();

        m_player->setRenderUpdateTarget(m_item);
        m_player->renderToFbo(
            static_cast<int>(framebufferObject()->handle()),
            static_cast<int>(framebufferObject()->width()),
            static_cast<int>(framebufferObject()->height()));
        m_player->reportSwap();

        QQuickOpenGLUtils::resetOpenGLState();
    }

private:
    bool m_initialized { false };
    QPointer<MpvVideoItem> m_item;
    App::PlayerController *m_playerController = nullptr;
    MpvPlayer *m_player = nullptr;
    QSize m_lastRenderSize;
};

MpvVideoItem::MpvVideoItem(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
{
    setMirrorVertically(false);
    setTextureFollowsItemSize(true);
    m_windowedRenderHeartbeatEnabled = !envFlagEnabled("OKILTV_DISABLE_WINDOWED_RENDER_HEARTBEAT");
    m_windowedRenderHeartbeatTimer.setInterval(kWindowedRenderRecoveryIntervalMs);
    connect(&m_windowedRenderHeartbeatTimer, &QTimer::timeout, this, [this]() {
        ++m_heartbeatTickCount;
        if (heartbeatNeedsRecovery()) {
            // Only nudge full-window rendering when mpv callbacks/renders go stale.
            requestUpdateFromHeartbeat();
        } else {
            refreshWindowedRenderHeartbeat();
        }
    });
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *window) {
        bindWindowSignals(window);
    });
#if defined(Q_OS_WIN)
    m_windowsPacingDiagEnabled = envFlagEnabled("OKILTV_WINDOWS_PACING_DIAG");
    if (m_windowsPacingDiagEnabled) {
        m_windowsRenderStatsElapsed.start();
        m_windowsRenderStatsTimer.setInterval(1000);
        connect(&m_windowsRenderStatsTimer, &QTimer::timeout, this, &MpvVideoItem::logWindowsRenderStats);
        m_windowsRenderStatsTimer.start();
    }
#endif
    Core::DebugLogger::instance().log(QStringLiteral("video"), QStringLiteral("MpvVideoItem constructed."));
}

QQuickFramebufferObject::Renderer *MpvVideoItem::createRenderer() const
{
    if (window() != nullptr) {
        window()->setPersistentGraphics(true);
        window()->setPersistentSceneGraph(true);
        Core::DebugLogger::instance().log(
            QStringLiteral("video"),
            QStringLiteral("Creating renderer with window graphics API %1.")
                .arg(static_cast<int>(QQuickWindow::graphicsApi())));
    }

    return new MpvVideoRenderer();
}

QObject *MpvVideoItem::playerController() const
{
    return m_playerController;
}

void MpvVideoItem::setPlayerController(QObject *controllerObject)
{
    auto *controller = qobject_cast<App::PlayerController *>(controllerObject);
    if (m_playerController == controller) {
        return;
    }

    if (m_isPlayingChangedConnection) {
        disconnect(m_isPlayingChangedConnection);
        m_isPlayingChangedConnection = {};
    }

    m_playerController = controller;
#if defined(Q_OS_WIN)
    if (m_playerController != nullptr) {
        m_isPlayingChangedConnection =
            connect(m_playerController, &App::PlayerController::isPlayingChanged, this, [this]() {
                refreshWindowedRenderHeartbeat();
            });
    }
#endif
    emit playerControllerChanged();
    Core::DebugLogger::instance().log(QStringLiteral("video"), QStringLiteral("Player controller attached to video item."));
    refreshWindowedRenderHeartbeat();
    update();
}

QObject *MpvVideoItem::playerObject() const
{
    return m_playerObject.data();
}

void MpvVideoItem::setPlayerObject(QObject *playerObject)
{
    auto *resolvedPlayer = qobject_cast<MpvPlayer *>(playerObject);
    if (m_playerObject == resolvedPlayer) {
        return;
    }

    m_playerObject = resolvedPlayer;
    emit playerObjectChanged();
    Core::DebugLogger::instance().log(
        QStringLiteral("video"),
        QStringLiteral("Player object attached to video item: item=%1 player=%2.")
            .arg(reinterpret_cast<quintptr>(this), 0, 16)
            .arg(reinterpret_cast<quintptr>(resolvedPlayer), 0, 16));
    refreshWindowedRenderHeartbeat();
    requestUpdateImpl("player-object", true);
}

void MpvVideoItem::requestUpdate()
{
    requestUpdateImpl("generic");
}

void MpvVideoItem::requestUpdateFromHeartbeat()
{
    requestUpdateImpl("heartbeat", true);
}

void MpvVideoItem::requestUpdateFromMpv()
{
    requestUpdateImpl("mpv");
}

void MpvVideoItem::requestUpdateImpl(const char *source, const bool forceWindowUpdate)
{
    ++m_totalRequestUpdateCount;
    if (qstrcmp(source, "heartbeat") == 0) {
        ++m_heartbeatRequestUpdateCount;
    } else if (qstrcmp(source, "mpv") == 0) {
        ++m_mpvRequestUpdateCount;
    } else {
        ++m_otherRequestUpdateCount;
    }
    refreshWindowedRenderHeartbeat();
    update();
    if (forceWindowUpdate && window() != nullptr) {
        ++m_windowUpdateCount;
        window()->update();
    }
}

void MpvVideoItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickFramebufferObject::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() == oldGeometry.size()) {
        return;
    }

    refreshWindowedRenderHeartbeat();
    requestUpdateImpl("geometry", true);
}

void MpvVideoItem::refreshWindowedRenderHeartbeat()
{
    if (!m_windowedRenderHeartbeatEnabled) {
        if (m_windowedRenderHeartbeatTimer.isActive()) {
            m_windowedRenderHeartbeatTimer.stop();
        }
        return;
    }

    const auto *quickWindow = window();
    const auto hasVisibleWindow = quickWindow != nullptr && quickWindow->isVisible();
    const auto hasExposedWindow = quickWindow != nullptr && quickWindow->isExposed();
    const auto itemRenderable = isVisible() && width() > 0.0 && height() > 0.0;
    const auto shouldRun = ((m_playerController != nullptr && m_playerController->isPlaying())
                            || (m_playerController == nullptr && m_playerObject != nullptr))
        && hasVisibleWindow
        && hasExposedWindow
        && itemRenderable;
    const auto shouldRecover = shouldRun && heartbeatNeedsRecovery();
    const auto desiredInterval = shouldRecover ? kWindowedRenderRecoveryIntervalMs : kWindowedRenderWatchdogIntervalMs;

    if (shouldRun && !m_windowedRenderHeartbeatTimer.isActive()) {
        m_windowedRenderHeartbeatRecoveryMode = shouldRecover;
        m_windowedRenderHeartbeatTimer.setInterval(desiredInterval);
        m_windowedRenderHeartbeatTimer.start();
        if (m_windowsPacingDiagEnabled) {
            Core::DebugLogger::instance().log(
                QStringLiteral("video"),
                QStringLiteral("Windowed render heartbeat enabled (mode=%1 interval=%2ms visibility=%3:%4 visible=%5 exposed=%6 itemVisible=%7 size=%8x%9).")
                    .arg(shouldRecover ? QStringLiteral("recovery") : QStringLiteral("watchdog"))
                    .arg(desiredInterval)
                    .arg(static_cast<int>(quickWindow->visibility()))
                    .arg(visibilityLabel(quickWindow->visibility()))
                    .arg(hasVisibleWindow ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(hasExposedWindow ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(isVisible() ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(width(), 0, 'f', 1)
                    .arg(height(), 0, 'f', 1));
        }
    } else if (shouldRun && m_windowedRenderHeartbeatTimer.isActive()) {
        if (m_windowedRenderHeartbeatRecoveryMode != shouldRecover
            || m_windowedRenderHeartbeatTimer.interval() != desiredInterval) {
            m_windowedRenderHeartbeatRecoveryMode = shouldRecover;
            m_windowedRenderHeartbeatTimer.setInterval(desiredInterval);
            if (m_windowsPacingDiagEnabled) {
                Core::DebugLogger::instance().log(
                    QStringLiteral("video"),
                    QStringLiteral("Windowed render heartbeat switched to %1 mode (interval=%2ms).")
                        .arg(shouldRecover ? QStringLiteral("recovery") : QStringLiteral("watchdog"))
                        .arg(desiredInterval));
            }
        }
    } else if (!shouldRun && m_windowedRenderHeartbeatTimer.isActive()) {
        m_windowedRenderHeartbeatTimer.stop();
        if (m_windowsPacingDiagEnabled) {
            Core::DebugLogger::instance().log(
                QStringLiteral("video"),
                QStringLiteral("Windowed render heartbeat disabled (visible=%1 exposed=%2 visibility=%3:%4 fullscreen=%5 playing=%6 itemVisible=%7 size=%8x%9).")
                    .arg(hasVisibleWindow ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(hasExposedWindow ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(quickWindow != nullptr ? static_cast<int>(quickWindow->visibility()) : -1)
                    .arg(quickWindow != nullptr ? visibilityLabel(quickWindow->visibility()) : QStringLiteral("NoWindow"))
                    .arg((quickWindow != nullptr && quickWindow->visibility() == QWindow::FullScreen)
                             ? QStringLiteral("true")
                             : QStringLiteral("false"))
                    .arg((m_playerController != nullptr && m_playerController->isPlaying())
                             ? QStringLiteral("true")
                             : QStringLiteral("false"))
                    .arg(isVisible() ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(width(), 0, 'f', 1)
                    .arg(height(), 0, 'f', 1));
        }
    }
}

void MpvVideoItem::bindWindowSignals(QQuickWindow *window)
{
    if (m_windowVisibilityChangedConnection) {
        disconnect(m_windowVisibilityChangedConnection);
        m_windowVisibilityChangedConnection = {};
    }

    m_boundWindow = window;
    if (m_boundWindow != nullptr) {
        m_windowVisibilityChangedConnection =
            connect(m_boundWindow, &QWindow::visibilityChanged, this, [this](QWindow::Visibility visibility) {
                if (m_windowsPacingDiagEnabled) {
                    Core::DebugLogger::instance().log(
                        QStringLiteral("video"),
                        QStringLiteral("Window visibility changed to %1:%2 (isVisible=%3 exposed=%4 active=%5).")
                            .arg(static_cast<int>(visibility))
                            .arg(visibilityLabel(visibility))
                            .arg(m_boundWindow->isVisible() ? QStringLiteral("true") : QStringLiteral("false"))
                            .arg(m_boundWindow->isExposed() ? QStringLiteral("true") : QStringLiteral("false"))
                            .arg(m_boundWindow->isActive() ? QStringLiteral("true") : QStringLiteral("false")));
                }
                refreshWindowedRenderHeartbeat();
            });
    }
    refreshWindowedRenderHeartbeat();
}

void MpvVideoItem::logWindowsRenderStats()
{
    if (!m_windowsPacingDiagEnabled || !m_windowsRenderStatsElapsed.isValid()) {
        return;
    }

    const auto elapsedMs = m_windowsRenderStatsElapsed.restart();
    const auto *quickWindow = window();
    Core::DebugLogger::instance().log(
        QStringLiteral("video"),
        QStringLiteral(
            "Windows render stats over %1 ms: heartbeatMode=%2 interval=%3ms heartbeatTicks=%4 "
            "requestUpdate(total=%5 mpv=%6 heartbeat=%7 other=%8) windowUpdate=%9 "
            "windowVisible=%10 windowExposed=%11 visibility=%12:%13 itemVisible=%14 itemEnabled=%15 itemSize=%16x%17.")
            .arg(elapsedMs)
            .arg(m_windowedRenderHeartbeatRecoveryMode ? QStringLiteral("recovery") : QStringLiteral("watchdog"))
            .arg(m_windowedRenderHeartbeatTimer.isActive() ? m_windowedRenderHeartbeatTimer.interval() : 0)
            .arg(m_heartbeatTickCount)
            .arg(m_totalRequestUpdateCount)
            .arg(m_mpvRequestUpdateCount)
            .arg(m_heartbeatRequestUpdateCount)
            .arg(m_otherRequestUpdateCount)
            .arg(m_windowUpdateCount)
            .arg((quickWindow != nullptr && quickWindow->isVisible()) ? QStringLiteral("true") : QStringLiteral("false"))
            .arg((quickWindow != nullptr && quickWindow->isExposed()) ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(quickWindow != nullptr ? static_cast<int>(quickWindow->visibility()) : -1)
            .arg(quickWindow != nullptr ? visibilityLabel(quickWindow->visibility()) : QStringLiteral("NoWindow"))
            .arg(isVisible() ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(isEnabled() ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(width(), 0, 'f', 1)
            .arg(height(), 0, 'f', 1));
    m_totalRequestUpdateCount = 0;
    m_heartbeatRequestUpdateCount = 0;
    m_mpvRequestUpdateCount = 0;
    m_otherRequestUpdateCount = 0;
    m_windowUpdateCount = 0;
    m_heartbeatTickCount = 0;
}

bool MpvVideoItem::heartbeatNeedsRecovery() const
{
    const auto *quickWindow = window();
    if (quickWindow == nullptr || !quickWindow->isVisible() || !quickWindow->isExposed()) {
        return false;
    }

    if (!isVisible() || width() <= 0.0 || height() <= 0.0) {
        return false;
    }

    const auto *player = resolvedPlayer();
    if (player == nullptr) {
        return true;
    }

    const auto wallNowMs = monotonicNowMs();
    const auto lastRenderMs = player->lastRenderTimestampMs();
    const auto lastCallbackMs = player->lastRenderUpdateTimestampMs();
    const auto renderHealthy =
        lastRenderMs >= 0 && (wallNowMs - lastRenderMs) <= kWindowedRenderRecoveryThresholdMs;
    const auto callbackHealthy =
        lastCallbackMs >= 0 && (wallNowMs - lastCallbackMs) <= kWindowedRenderRecoveryThresholdMs;
    return !(renderHealthy && callbackHealthy);
}

MpvPlayer *MpvVideoItem::resolvedPlayer() const
{
    if (m_playerController != nullptr) {
        return m_playerController->player();
    }
    return m_playerObject.data();
}

} // namespace OKILTV::Player
