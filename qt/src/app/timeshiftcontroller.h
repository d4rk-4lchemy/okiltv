#pragma once

#include "../core/models.h"

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QVariantList>
#include <QTcpServer>
#include <QTimer>

#include <memory>
#include <optional>
#include <vector>

class QTcpSocket;
namespace OKILTV::Core {
class SettingsManager;
}

namespace OKILTV::App {

class DvrController;
class MultiViewController;
class PlayerController;

class TimeshiftController final : public QObject
{
    Q_OBJECT

public:
    explicit TimeshiftController(
        Core::SettingsManager *settings,
        PlayerController *playerController,
        DvrController *dvrController,
        MultiViewController *multiViewController,
        QObject *parent = nullptr);
    ~TimeshiftController() override;

    bool enabled() const;
    bool isActive() const;
    bool isPreparing() const;
    bool isAtLiveEdge() const;
    int configuredSegmentSeconds() const;
    double behindLiveSeconds() const;
    int windowSeconds() const;
    double availableDurationSeconds() const;
    double currentPositionSeconds() const;
    qint64 windowStartEpochMs() const;
    qint64 liveEdgeEpochMs() const;
    qint64 attachedWindowStartEpochMs() const;
    qint64 attachedWindowEndEpochMs() const;
    qint64 currentPlaybackEpochMs() const;
    int audioTrackCount() const;
    int subtitleTrackCount() const;
    QString droppedSubtitleSummary() const;
    QString noticeText() const;
    QString lastSeekModeText() const;

    bool handlePauseRequest();
    void handleUserStopRequest();
    void handleUserChannelSwitchRequest(const Core::Channel &nextChannel);
    bool handlePlaybackFailure(const QString &reason);
    bool handlePlaybackStarvation(const QString &reason);
    bool seekRelative(double seconds);
    bool seekToFraction(double fraction);
    bool jumpToLiveEdge();
    bool handlePrimaryPlaybackActivation();

public slots:
    void applySettings();
    void shutdownForApplicationExit();

signals:
    void stateChanged();
    void statusMessageRequested(const QString &message);
    void uiTestPlaybackUrlObserved(const QString &layer, const QString &url);
    void uiTestLocalRequestObserved(
        const QString &method,
        const QString &target,
        int statusCode,
        qint64 payloadBytes);

private:
    enum class SessionState
    {
        Idle,
        Starting,
        Running,
        Failed,
        Stopping
    };

    struct PlaylistInfo
    {
        QDateTime windowStartUtc;
        QDateTime liveEdgeUtc;
        double availableSeconds { 0.0 };
        bool valid { false };
    };

    struct SubtitleRendition
    {
        QString playlistFileName;
        QString playlistPath;
        QString name;
        QString language;
        QString codecName;
        bool isDefault { false };
    };

    struct TrackPreference
    {
        bool valid { false };
        bool noneSelected { false };
        QString title;
        QString language;
    };

    struct Session
    {
        QString id;
        SessionState state { SessionState::Idle };
        QString startReason;
        Core::Channel channel;
        QString inputUrl;
        QString fallbackPlaybackUrl;
        QString playbackUrl;
        QString sessionDirectory;
        QString playlistPath;
        QString avMasterPlaylistPath;
        std::unique_ptr<QProcess> probeProcess;
        std::unique_ptr<QProcess> ingestProcess;
        PlaylistInfo playlistInfo;
        QList<SubtitleRendition> subtitleRenditions;
        int audioTrackCount { 0 };
        int subtitleTrackCount { 0 };
        QStringList droppedSubtitleCodecs;
        QDateTime lastPlaylistAdvanceUtc;
        qint64 reconnectReadyOldestEpochMs { 0 };
        qint64 attachedWindowStartEpochMs { 0 };
        qint64 attachedWindowEndEpochMs { 0 };
        qint64 delayedStartEpochMs { 0 };
        qint64 pendingPlaybackAnchorEpochMs { 0 };
        qint64 pendingPlaybackTargetEpochMs { 0 };
        QDateTime pendingPlaybackRequestedUtc;
        QString pendingPlaybackUrl;
        double pendingPostLoadSeekSeconds { -1.0 };
        double pendingPostLoadSeekToleranceSeconds { -1.0 };
        bool pendingPostLoadSeekVerified { false };
        bool playbackLoadPending { false };
        bool resumeAfterLoad { false };
        bool pendingTrackRestore { false };
        TrackPreference preferredAudioTrack;
        TrackPreference preferredSubtitleTrack;
        QString lastSeekModeText { QStringLiteral("None") };
        QString noticeText;
        bool stopRequested { false };
        bool pauseWhenReady { false };
        bool playbackAttached { false };
        bool probeTimedOut { false };
        bool probeCompletionHandled { false };
        int runtimeRestartCount { 0 };
        QDateTime lastNearLiveEofRecoveryUtc;
        QDateTime lastLocalRecoveryUtc;
        int localRecoveryCount { 0 };
    };

    static QString findFfmpegBinary();
    static QString findFfprobeBinary();
    static QString sessionFolderName(const Core::Channel &channel);
    static PlaylistInfo parsePlaylistFile(const QString &playlistPath);
    static QString stateName(SessionState state);
    static int reconnectBackoffMs(int attempt);

    QString rootDirectory() const;
    QString configuredStorageDirectory() const;
    qint64 preferredLiveEdgeAnchorEpochMs() const;
    qint64 clampedPlaybackAnchorEpochMs(qint64 targetEpochMs) const;
    qint64 targetEpochMsFromFraction(double fraction) const;
    qint64 targetEpochMsFromCurrentOffset(double seconds) const;
    bool seekToEpochMs(qint64 targetEpochMs, const QString &logContext, bool preferDirect);
    bool tryRecoverNearLivePlaybackEnded();
    bool canUseDirectSeek(qint64 targetEpochMs) const;
    bool directSeekToEpochMs(qint64 targetEpochMs);
    void setNoticeText(const QString &text, int autoClearMs = 0);
    void captureTrackPreferences();
    void restoreTrackPreferences();
    TrackPreference currentTrackPreference(const QString &type) const;
    int findTrackIdForPreference(const QVariantList &tracks, const QString &type, const TrackPreference &preference) const;
    void playSessionFromAnchor(qint64 targetEpochMs, bool pauseWhenReady);
    void resumePlaybackAfterTransportAction();
    bool ensurePlaybackServer();
    QString localPlaybackUrl(const Session &session) const;
    QString delayedPlaybackUrl(const Session &session, qint64 anchorEpochMs, qint64 targetEpochMs) const;
    qint64 sessionWindowStartEpochMs(const Session &session) const;
    qint64 sessionLiveEdgeEpochMs(const Session &session) const;
    qint64 sessionOldestSegmentEpochMs(const Session &session) const;
    qint64 sessionReliableStartEpochMs(const Session &session) const;
    qint64 sessionCurrentPlaybackEpochMs(const Session &session) const;
    qint64 globalWindowStartEpochMs() const;
    qint64 globalLiveEdgeEpochMs() const;
    Session *findRetainedSessionById(const QString &sessionId);
    const Session *findRetainedSessionById(const QString &sessionId) const;
    Session *findAnySessionById(const QString &sessionId);
    const Session *findAnySessionById(const QString &sessionId) const;
    std::vector<const Session *> allSessionsSortedByStart() const;
    void refreshSessionPlaylistInfo(Session &session);
    void refreshRetainedSessionPlaylists();
    bool hasRunningIngestSession() const;
    bool activateRetainedSession(const QString &sessionId);
    bool switchToNextGenerationPlayback(const QString &reason, bool emitNotice = false);
    void scheduleReconnectGeneration(const QString &reason);
    void attemptReconnectGeneration();
    bool startDetachedGeneration(const QString &reason);
    void handleRuntimeIngestFailure(
        Session &session,
        const QString &reason,
        bool fromStartup,
        const QString &fallbackStatusMessage = {});
    void cleanupSessionFiles(Session &session, bool forceImmediateProcessKill = false);
    void cleanupUnreachableRetainedSessions();
    bool resolveSeekTarget(
        qint64 requestedEpochMs,
        QString *sessionId,
        qint64 *resolvedEpochMs,
        bool *snappedGap) const;
    qint64 anchoredPlaybackStartEpochMs(const Session &session, qint64 targetEpochMs, const QString &playlistPath) const;
    QByteArray buildPlaybackMasterPlaylist(const Session &session, qint64 anchorEpochMs = 0, qint64 targetEpochMs = 0) const;
    QByteArray buildDelayedMediaPlaylist(
        const Session &session,
        const QString &playlistPath,
        qint64 anchorEpochMs,
        qint64 targetEpochMs) const;
    void handlePlaybackServerConnection();
    void handlePlaybackServerRequest(QTcpSocket *socket);
    void handlePlaybackFileLoaded();
    void finalizePendingPlaybackLoadIfReady(const QString &source);
    void applyPendingPostLoadSeekIfReady(const QString &source);
    bool channelEligibleForTimeshift(const std::optional<Core::Channel> &channel) const;
    bool startSessionForCurrentChannel(bool pauseWhenReady, const QString &reason);
    void attachSessionPlaybackIfReady();
    void stopSession(
        bool restoreLivePlayback,
        const QString &reason,
        bool markForSinglePlaybackRestart = false,
        bool forceImmediateProcessKill = false);
    bool restoreLivePlaybackPath();
    void handleCurrentChannelChanged();
    void handleMultiviewLayoutChanged();
    void handlePlaylistPoll();
    void cleanupStaleSessions() const;
    bool ensureDiskAdmission(const QString &inputUrl, QString *failureMessage) const;
    QString selectInputUrlForChannel(const Core::Channel &channel) const;
    void updateDerivedState();

    Core::SettingsManager *m_settings;
    PlayerController *m_playerController;
    DvrController *m_dvrController;
    MultiViewController *m_multiViewController;
    std::optional<Session> m_session;
    QTcpServer m_playbackServer;
    QTimer m_readyPollTimer;
    QTimer m_playlistPollTimer;
    QTimer m_noticeClearTimer;
    QTimer m_reconnectGenerationTimer;
    QString m_noticeAutoClearSessionId;
    QString m_noticeAutoClearText;
    std::vector<Session> m_retainedSessions;
    int m_reconnectGenerationAttempt { 0 };
    bool m_restartWhenSinglePlaybackReturns { false };
    bool m_lastKnownMultiviewActive { false };
};

} // namespace OKILTV::App
