#pragma once

#include "../core/debuglogger.h"
#include "../core/networkaccess.h"

#include <QObject>
#include <QPointer>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QHash>
#include <QList>
#include <QMap>
#include <QTcpServer>
#include <QTimer>
#include <QUrlQuery>
#include <QVariantMap>

class QQuickItem;
class QQuickWindow;
class QTcpSocket;

namespace OKILTV::App {

class AppController;
class ChannelListModel;
class GuideStateModel;
class NowNextModel;
class PlayerController;
class SettingsController;
class ShellController;
class TimeshiftController;
class UiTestCaptureController;

class UiTestBridge final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled CONSTANT)
    Q_PROPERTY(quint16 port READ port CONSTANT)
    Q_PROPERTY(QString runDirectory READ runDirectory CONSTANT)

public:
    struct RegionRecord
    {
        QString name;
        bool visible { false };
        qreal x { 0.0 };
        qreal y { 0.0 };
        qreal width { 0.0 };
        qreal height { 0.0 };
        qreal cx { 0.0 };
        qreal cy { 0.0 };
        qreal nx { 0.0 };
        qreal ny { 0.0 };
        qreal nwidth { 0.0 };
        qreal nheight { 0.0 };
    };

    explicit UiTestBridge(QObject *parent = nullptr);
    ~UiTestBridge() override;

    bool enabled() const;
    quint16 port() const;
    QString runDirectory() const;

    void setWindow(QObject *windowObject);
    void attachControllers(
        AppController *appController,
        ShellController *shellController,
        ChannelListModel *channelListModel,
        GuideStateModel *guideStateModel,
        NowNextModel *playbackNowNextModel,
        PlayerController *playerController,
        TimeshiftController *timeshiftController,
        SettingsController *settingsController,
        UiTestCaptureController *captureController);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct EventRecord
    {
        quint64 id { 0 };
        QString type;
        QJsonObject payload;
        QString timestampUtc;
    };

    void startServerIfEnabled();
    void ensureRunDirectoryLayout();
    void installLogSubscription();
    void installNetworkSubscription();
    void stopSubscriptions();
    void setupStatePolling();
    void appendEvent(const QString &type, const QJsonObject &payload);
    void appendEventThreadSafe(const QString &type, const QJsonObject &payload);
    void appendNetworkEvent(const QString &type, const QJsonObject &payload);
    void writeEventToArtifacts(const EventRecord &record);
    void writeStateSnapshotArtifact(const QJsonObject &snapshot, quint64 eventId) const;
    void broadcastSseEvent(const EventRecord &record);

    void handleServerConnection();
    void handleSocketReadyRead(QTcpSocket *socket);
    void handleEventsStream(QTcpSocket *socket, quint64 cursor);
    bool isAuthorized(const QMap<QByteArray, QByteArray> &headers, const QUrlQuery &query) const;
    void writeJsonResponse(QTcpSocket *socket, int statusCode, const QJsonObject &payload);
    void writeErrorResponse(QTcpSocket *socket, int statusCode, const QString &code, const QString &message);
    void writeSseHeaders(QTcpSocket *socket);
    static QString timestampUtc();
    static QString sanitizePathSegment(const QString &value, const QString &fallback);

    QJsonObject buildHealthPayload() const;
    QJsonObject buildStateSnapshot() const;
    QJsonArray buildRegionsArray() const;
    QJsonObject buildElementsPayload() const;
    QJsonArray buildSemanticElements(const QHash<QString, RegionRecord> &regionsByName) const;
    QJsonArray buildVisibleTextInventory() const;
    QJsonArray buildLogsPayload(quint64 cursor) const;
    QJsonObject regionToJson(const RegionRecord &region) const;
    RegionRecord buildRegion(const QString &name, QQuickItem *item, qreal windowWidth, qreal windowHeight) const; // NOLINT(bugprone-easily-swappable-parameters)
    RegionRecord buildWindowRegion(const QString &name, qreal windowWidth, qreal windowHeight) const; // NOLINT(bugprone-easily-swappable-parameters)
    QQuickItem *findItemByObjectName(const QString &objectName) const;
    QHash<QString, RegionRecord> buildRegionMap() const;
    QJsonObject currentWindowState() const;
    QJsonObject currentPlaybackState() const;
    QJsonArray currentNetworkMap() const;
    QJsonValue redactedJsonValue(const QJsonValue &value) const;
    QVariantMap redactedVariantMap(const QVariantMap &value) const;
    QString captureOutputPathForRequest(const QJsonObject &requestBody, QString *labelOut) const;
    void handleCaptureCompletion(const QString &outputPath, bool ok);
    void observePlaybackUrl();
    void observeTimeshiftPlaybackUrl(const QString &layer, const QString &url);
    void observeTimeshiftLocalRequest(
        const QString &method,
        const QString &target,
        int statusCode,
        qint64 payloadBytes);
    void handleStatePoll();

    bool m_enabled { false };
    QString m_token;
    quint16 m_requestedPort { 0 };
    quint16 m_boundPort { 0 };
    QString m_runDirectory;
    QString m_eventsFilePath;
    QString m_networkFilePath;
    QString m_stateSnapshotDirectory;
    QString m_screenshotDirectory;

    QTcpServer m_server;
    QPointer<QQuickWindow> m_window;
    QPointer<QObject> m_rootObject;
    QPointer<AppController> m_appController;
    QPointer<ShellController> m_shellController;
    QPointer<ChannelListModel> m_channelListModel;
    QPointer<GuideStateModel> m_guideStateModel;
    QPointer<NowNextModel> m_playbackNowNextModel;
    QPointer<PlayerController> m_playerController;
    QPointer<TimeshiftController> m_timeshiftController;
    QPointer<SettingsController> m_settingsController;
    QPointer<UiTestCaptureController> m_captureController;

    QList<EventRecord> m_events;
    QHash<QString, quint64> m_captureRequestIdByPath;
    QJsonArray m_networkMap;
    QList<QPointer<QTcpSocket>> m_sseClients;
    quint64 m_nextEventId { 1 };
    quint64 m_nextCaptureRequestId { 1 };
    QByteArray m_lastStateDigest;
    QTimer m_statePollTimer;
    quint64 m_logSubscriptionId { 0 };
    quint64 m_networkObserverId { 0 };
};

} // namespace OKILTV::App
