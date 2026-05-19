#pragma once

#include <QQuickFramebufferObject>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <qqmlintegration.h>

namespace OKILTV::App {
class PlayerController;
}

class QQuickWindow;

namespace OKILTV::Player {

class MpvPlayer;

class MpvVideoItem : public QQuickFramebufferObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(MpvVideoItem)
    QML_ADDED_IN_VERSION(1, 0)
    Q_PROPERTY(QObject *playerController READ playerController WRITE setPlayerController NOTIFY playerControllerChanged)
    Q_PROPERTY(QObject *playerObject READ playerObject WRITE setPlayerObject NOTIFY playerObjectChanged)

public:
    explicit MpvVideoItem(QQuickItem *parent = nullptr);

    Renderer *createRenderer() const override;

    QObject *playerController() const;
    void setPlayerController(QObject *controllerObject);
    QObject *playerObject() const;
    void setPlayerObject(QObject *playerObject);

signals:
    void playerControllerChanged();
    void playerObjectChanged();

private:
    Q_SLOT void requestUpdate();
    Q_SLOT void requestUpdateFromHeartbeat();
    Q_SLOT void requestUpdateFromMpv();
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void refreshWindowedRenderHeartbeat();
    void bindWindowSignals(QQuickWindow *window);
    void requestUpdateImpl(const char *source, bool forceWindowUpdate = false);
    void logWindowsRenderStats();
    bool heartbeatNeedsRecovery() const;
    MpvPlayer *resolvedPlayer() const;

    App::PlayerController *m_playerController = nullptr;
    QPointer<MpvPlayer> m_playerObject;
    QTimer m_windowedRenderHeartbeatTimer;
    QTimer m_windowsRenderStatsTimer;
    QPointer<QQuickWindow> m_boundWindow;
    QMetaObject::Connection m_isPlayingChangedConnection;
    QMetaObject::Connection m_windowVisibilityChangedConnection;
    bool m_windowedRenderHeartbeatEnabled { false };
    bool m_windowsPacingDiagEnabled { false };
    quint64 m_totalRequestUpdateCount { 0 };
    quint64 m_heartbeatRequestUpdateCount { 0 };
    quint64 m_mpvRequestUpdateCount { 0 };
    quint64 m_otherRequestUpdateCount { 0 };
    quint64 m_windowUpdateCount { 0 };
    quint64 m_heartbeatTickCount { 0 };
    bool m_windowedRenderHeartbeatRecoveryMode { true };
    QElapsedTimer m_windowsRenderStatsElapsed;
};

} // namespace OKILTV::Player
