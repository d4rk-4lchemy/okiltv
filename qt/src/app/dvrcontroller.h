#pragma once

#include "../core/models.h"
#include "../core/settingsmanager.h"

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>

#include <memory>
#include <map>
#include <optional>
#include <utility>

namespace OKILTV::App {

class PlayerController;

class DvrController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int scheduledCount READ scheduledCount NOTIFY stateChanged)
    Q_PROPERTY(int activeRecordingCount READ activeRecordingCount NOTIFY stateChanged)
    Q_PROPERTY(bool exitConfirmationRequired READ exitConfirmationRequired NOTIFY stateChanged)

public:
    explicit DvrController(Core::SettingsManager *settings, PlayerController *playerController, QObject *parent = nullptr);
    ~DvrController() override;

    int scheduledCount() const;
    int activeRecordingCount() const;
    bool exitConfirmationRequired() const;

    Q_INVOKABLE bool toggleProgramSchedule(const QVariantMap &channel, const QVariantMap &program);
    Q_INVOKABLE bool isProgramScheduled(const QVariantMap &channel, const QVariantMap &program) const;
    Q_INVOKABLE bool isProgramScheduledByIdentity(
        const QString &profileId,
        int channelId,
        const QString &startIso,
        const QString &stopIso, // NOLINT(bugprone-easily-swappable-parameters)
        const QString &title) const;
    QString activeTapPlaybackUrlForChannel(const Core::Channel &channel) const;

    bool attachPlaybackForChannel(const Core::Channel &channel);
    QList<int> recordingChannelIdsForProfile(const QString &profileId) const;
    void shutdownForApplicationExit();

signals:
    void stateChanged();
    void recordingChannelsChanged(const QString &profileId, const QList<int> &channelIds);

private:
    enum class SessionState
    {
        Starting,
        Running,
        Stopping,
        Failed
    };

    struct MergedWindow
    {
        QString id;
        QString channelKey;
        QString profileId;
        int channelId { -1 };
        QString channelName;
        QString streamUrl;
        QString tvgId;
        QString displayTitle;
        QDateTime startAt;
        QDateTime stopAt;
    };

    struct Session
    {
        MergedWindow window;
        std::unique_ptr<QProcess> ingestProcess;
        SessionState state { SessionState::Starting };
        QString tapUrl;
        quint16 tapPort { 0 };
        QString recordTempPath;
        QString recordFinalPath;
        bool remuxToMkv { true };
        bool stopRequested { false };
        bool failedToStart { false };
        bool recordingStarted { false };
        qint64 processId { 0 };
        bool finishedSignaled { false };
        int lastExitCode { 0 };
        QProcess::ExitStatus lastExitStatus { QProcess::NormalExit };
        int orphanSweepRetriesRemaining { 0 };
        QDateTime startRequestedAt;
        QString stopReason;
    };

    static QDateTime parseIsoUtc(const QString &value);
    static QString toIsoUtc(const QDateTime &value);
    static QString sanitizeForFilename(const QString &value);
    static QString makeScheduleId(
        const QString &profileId,
        int channelId,
        const QDateTime &start,
        const QDateTime &stop,
        const QString &title);
    static QString makeChannelKey(const QString &profileId, int channelId);
    static QString makeMergedWindowId(const QString &profileId, int channelId, const QDateTime &startAt, const QDateTime &stopAt);
    static QString findFfmpegBinary();
    static std::optional<quint16> reserveUdpPort();

    std::pair<QDateTime, QDateTime> effectiveWindow(const Core::DvrScheduleEntry &entry) const;
    QList<MergedWindow> mergedWindows() const;
    Core::Channel channelFromWindow(const MergedWindow &window, const QString &overrideUrl = QString {}) const;
    void loadSchedulesFromSettings();
    void persistSchedules();
    void emitRecordingChannelsChanged();
    void tick();
    bool startSession(const MergedWindow &window);
    void maybeAutoHandoffToTap(const Session &session);
    void requestStopSession(const QString &sessionId, const QString &reason = QString {});
    void finalizeStopSession(const QString &sessionId, int exitCode, QProcess::ExitStatus status);
    void scheduleRestartForWindow(const QString &windowId, const QString &reason);
    void forceStopSessionProcess(const QString &sessionId, int retriesLeft);
    void reconcileStoppingSession(const QString &sessionId, const QString &reason);
    void sweepWindowsOrphanFfmpeg(const Session &session) const;
    Session *sessionByChannel(const QString &profileId, int channelId) const;
    void maybeStartRemux(const Session &session);
    void scheduleDeleteTempRecording(const QString &path, int retriesLeft) const;
    void scheduleDeleteInvalidRemuxOutput(const QString &path, int retriesLeft) const;
    QString recordingOutputDirectory() const;

    Core::SettingsManager *m_settings;
    PlayerController *m_playerController;
    QList<Core::DvrScheduleEntry> m_schedules;
    std::map<QString, std::unique_ptr<Session>> m_sessions;
    QMap<QString, QDateTime> m_restartNotBeforeByWindowId;
    mutable QMap<QString, QList<int>> m_lastRecordingChannelsByProfile;
    QTimer m_tickTimer;
    bool m_exitShutdownStarted { false };
};

} // namespace OKILTV::App
