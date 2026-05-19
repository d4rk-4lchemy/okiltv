#pragma once

#include "../core/models.h"
#include "../player/mpvplayer.h"

#include <QObject>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <deque>
#include <optional>
#include <utility>

namespace OKILTV::App {

class TimeshiftController;

class PlayerController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY isPlayingChanged)
    Q_PROPERTY(bool isLoading READ isLoading NOTIFY isLoadingChanged)
    Q_PROPERTY(bool isBuffering READ isBuffering NOTIFY isBufferingChanged)
    Q_PROPERTY(bool channelSwitchInProgress READ channelSwitchInProgress NOTIFY channelSwitchInProgressChanged)
    Q_PROPERTY(bool channelLoadFailed READ channelLoadFailed NOTIFY channelLoadFailedChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY positionTextChanged)
    Q_PROPERTY(QString nowPlayingName READ nowPlayingName NOTIFY nowPlayingNameChanged)
    Q_PROPERTY(QVariantMap currentChannel READ currentChannel NOTIFY currentChannelChanged)
    Q_PROPERTY(QString playbackMode READ playbackMode NOTIFY playbackModeChanged)
    Q_PROPERTY(QString catchupProgramLabel READ catchupProgramLabel NOTIFY catchupProgramLabelChanged)
    Q_PROPERTY(bool catchupTimelineActive READ catchupTimelineActive NOTIFY catchupTimelineChanged)
    Q_PROPERTY(qint64 catchupTimelineStartEpochMs READ catchupTimelineStartEpochMs NOTIFY catchupTimelineChanged)
    Q_PROPERTY(qint64 catchupTimelineAvailableEdgeEpochMs READ catchupTimelineAvailableEdgeEpochMs NOTIFY catchupTimelineChanged)
    Q_PROPERTY(double catchupTimelineAvailableSeconds READ catchupTimelineAvailableSeconds NOTIFY catchupTimelineChanged)
    Q_PROPERTY(double catchupTimelinePositionSeconds READ catchupTimelinePositionSeconds NOTIFY catchupTimelineChanged)
    Q_PROPERTY(bool catchupTimelineAtLiveEdge READ catchupTimelineAtLiveEdge NOTIFY catchupTimelineChanged)
    Q_PROPERTY(QString catchupTimelineNoticeText READ catchupTimelineNoticeText NOTIFY catchupTimelineChanged)
    Q_PROPERTY(bool timeshiftActive READ timeshiftActive NOTIFY timeshiftStateChanged)
    Q_PROPERTY(bool timeshiftPreparing READ timeshiftPreparing NOTIFY timeshiftStateChanged)
    Q_PROPERTY(bool timeshiftAtLiveEdge READ timeshiftAtLiveEdge NOTIFY timeshiftStateChanged)
    Q_PROPERTY(double timeshiftBehindLiveSeconds READ timeshiftBehindLiveSeconds NOTIFY timeshiftStateChanged)
    Q_PROPERTY(int timeshiftWindowSeconds READ timeshiftWindowSeconds NOTIFY timeshiftStateChanged)
    Q_PROPERTY(double timeshiftAvailableSeconds READ timeshiftAvailableSeconds NOTIFY timeshiftStateChanged)
    Q_PROPERTY(double timeshiftPositionSeconds READ timeshiftPositionSeconds NOTIFY timeshiftStateChanged)
    Q_PROPERTY(qint64 timeshiftWindowStartEpochMs READ timeshiftWindowStartEpochMs NOTIFY timeshiftStateChanged)
    Q_PROPERTY(qint64 timeshiftLiveEdgeEpochMs READ timeshiftLiveEdgeEpochMs NOTIFY timeshiftStateChanged)
    Q_PROPERTY(qint64 timeshiftAttachedWindowStartEpochMs READ timeshiftAttachedWindowStartEpochMs NOTIFY timeshiftStateChanged)
    Q_PROPERTY(qint64 timeshiftAttachedWindowEndEpochMs READ timeshiftAttachedWindowEndEpochMs NOTIFY timeshiftStateChanged)
    Q_PROPERTY(QString timeshiftNoticeText READ timeshiftNoticeText NOTIFY timeshiftStateChanged)
    Q_PROPERTY(bool isRecording READ isRecording NOTIFY isRecordingChanged)
    Q_PROPERTY(bool isRemuxing READ isRemuxing NOTIFY isRemuxingChanged)

public:
    explicit PlayerController(QObject *parent = nullptr);

    bool isPlaying() const;
    bool isLoading() const;
    bool isBuffering() const;
    bool channelSwitchInProgress() const;
    bool channelLoadFailed() const;
    double volume() const;
    bool muted() const;
    void setVolume(double value);
    QString positionText() const;
    QString nowPlayingName() const;
    QVariantMap currentChannel() const;
    QString playbackMode() const;
    QString catchupProgramLabel() const;
    bool catchupTimelineActive() const;
    qint64 catchupTimelineStartEpochMs() const;
    qint64 catchupTimelineAvailableEdgeEpochMs() const;
    double catchupTimelineAvailableSeconds() const;
    double catchupTimelinePositionSeconds() const;
    bool catchupTimelineAtLiveEdge() const;
    QString catchupTimelineNoticeText() const;
    bool timeshiftActive() const;
    bool timeshiftPreparing() const;
    bool timeshiftAtLiveEdge() const;
    double timeshiftBehindLiveSeconds() const;
    int timeshiftWindowSeconds() const;
    double timeshiftAvailableSeconds() const;
    double timeshiftPositionSeconds() const;
    qint64 timeshiftWindowStartEpochMs() const;
    qint64 timeshiftLiveEdgeEpochMs() const;
    qint64 timeshiftAttachedWindowStartEpochMs() const;
    qint64 timeshiftAttachedWindowEndEpochMs() const;
    QString timeshiftNoticeText() const;
    Q_INVOKABLE QVariantMap debugOverlaySnapshot();
    Q_INVOKABLE QVariantList audioTracks();
    Q_INVOKABLE QVariantList subtitleTracks();
    Q_INVOKABLE void selectAudioTrack(int id);
    Q_INVOKABLE void selectSubtitleTrack(int id);
    Q_INVOKABLE bool takeScreenshot(const QString &outputDir, const QString &channelName, const QString &programmeName);
    Q_INVOKABLE bool startRecording(const QString &outputDir, const QString &channelName, const QString &programmeName);
    Q_INVOKABLE void stopRecording();
    bool isRecording() const;
    bool isRemuxing() const;

    static QString debugStreamHostFromUrl(const QString &url);
    static QString debugStreamIdFromUrl(const QString &url);
    static QString debugTimestampNowLocal();
    static QString formatDebugBufferDuration(double bufferDurationSeconds);
    static QString formatDebugFramerate(double framesPerSecond);
    static QString formatDebugBitrate(double bitsPerSecond);
    static double adaptiveSteadyStateCacheLimitSeconds(double bufferTargetSeconds);
    static double adaptiveSteadyStateCacheHysteresisSeconds(double bufferTargetSeconds);
    static qint64 adaptiveSteadyStateMaxBytes(
        double bufferTargetSeconds,
        std::optional<double> averageBitsPerSecond);
    static int startupBufferFallbackTimeoutMs(double bufferTargetSeconds, int segmentSecondsHint = 0);
    static int reconnectDepletionTimeoutMsForWaitSeconds(double waitForDataStreamSeconds);
    static bool shouldStartPreemptiveReconnect(
        std::optional<double> cacheDurationSeconds,
        double bufferTargetSeconds,
        std::optional<double> throughputBitsPerSecond,
        bool playbackAdvanced,
        bool backendBuffering);
    static bool deadStreamLikelyDisconnected(
        std::optional<double> cacheDurationSeconds,
        std::optional<double> throughputBitsPerSecond);

    Player::MpvPlayer *player();
    void setTimeshiftController(TimeshiftController *controller);
    void applySettings(
        const QString &mpvDllPath,
        const QMap<QString, QString> &mpvOptions,
        double waitForDataStreamSeconds,
        bool deinterlaceEnabled,
        double bufferSizeSeconds,
        const QString &playerUserAgent,
        bool remuxRecordingsToMkv = true);
    void playChannel(const Core::Channel &channel);
    void playCatchupChannel(const Core::Channel &channel, const QString &catchupUrl, const QString &programLabel);
    void playCatchupChannel(
        const Core::Channel &channel,
        const QString &catchupUrl,
        const QString &programLabel,
        const QDateTime &programStartUtc,
        const QDateTime &programStopUtc,
        const QString &canonicalCatchupUrl = {});
    void playCurrentPlaybackUrl(const QString &url, bool pauseWhenReady = false, const QString &loadfileOptions = {});
    void refreshCurrentChannelMetadata(const Core::Channel &channel);
    void attachSharedPlayback(
        Player::MpvPlayer *sharedPlayer,
        const Core::Channel &channel,
        bool protectedSession,
        bool stopBasePlayerOnInitialAttach = true);
    void detachSharedPlayback(bool clearChannel = true);
    void adoptExistingPlaybackChannel(const Core::Channel &channel);
    Player::MpvPlayer *primaryBasePlayer();
    bool isSharedPlaybackPlayer(const Player::MpvPlayer *candidate) const;
    bool usingSharedPlayback() const;
    QString currentPlaybackUrl() const;
    double playbackPositionSeconds() const;
    bool inCatchupMode() const;
    Q_INVOKABLE void returnToLiveFromCatchup();

    const std::optional<Core::Channel> &currentChannelValue() const;

public slots:
    void shutdownForApplicationExit();
    void stop();
    void togglePause();
    Q_INVOKABLE void toggleMute();
    void seekRelative(double seconds);
    Q_INVOKABLE void jumpToLiveEdge();
    Q_INVOKABLE void seekTimeshiftRelative(double seconds);
    Q_INVOKABLE void seekTimeshiftToFraction(double fraction);

signals:
    void isRecordingChanged();
    void isRemuxingChanged();
    void screenshotTaken(const QString &path);
    void recordingStarted(const QString &path);
    void recordingStopped();
    void isPlayingChanged();
    void isLoadingChanged();
    void isBufferingChanged();
    void channelSwitchInProgressChanged();
    void channelLoadFailedChanged();
    void volumeChanged();
    void mutedChanged();
    void positionTextChanged();
    void nowPlayingNameChanged();
    void currentChannelChanged();
    void currentPlaybackUrlChanged();
    void playbackModeChanged();
    void catchupProgramLabelChanged();
    void catchupTimelineChanged();
    void timeshiftStateChanged();
    void playbackChannelActivated(int channelId);
    void playbackFileLoaded();
    void playbackError(const QString &message);

private:
    static int reconnectAttemptIntervalMs();
    void ensurePlaybackSignalConnections(Player::MpvPlayer *player);
    void beginDeferredLoadingIndicator();
    void stopDeferredLoadingIndicator();
    void startReconnectLoop(const QString &reason);
    void stopReconnectLoop(const QString &reason);
    void clearReconnectAttemptInFlight(const QString &reason);
    void recoverPendingPlaybackLoadFailure(const QString &reason, const QString &logMessage = {});
    void handleReconnectAttemptTick();
    void failReconnect(const QString &reason);
    void evaluateReconnectRecovery();
    void schedulePauseStateResync(int retries = 8);
    void stopPauseStateResync();
    void syncIsPlayingFromBackend();
    void syncIsBufferingFromBackend();
    void clearPlaybackStallTracking();
    void refreshBufferingState();
    void setIsPlaying(bool value);
    void setIsLoading(bool value);
    void setIsBuffering(bool value);
    void setChannelSwitchInProgress(bool value);
    void updatePosition();
    void setChannelLoadFailed(bool value);
    void startStartupBufferFallbackWatchdog();
    void stopStartupBufferFallbackWatchdog(bool resetTuneState);
    void handleStartupBufferFallbackTimeout();
    void startStartupBufferProbe();
    void stopStartupBufferProbe();
    void evaluateStartupBufferAndResumeIfReady();
    void handleHwdecFallbackCheck();
    void resetBitrateAverageWindow();
    void resetAdaptiveSteadyStateBufferingState();
    std::optional<double> updateBitrateAverageBitsPerSecond(std::optional<double> instantaneousBitsPerSecond);
    void maybeRetuneSteadyStateBuffering(std::optional<double> cacheDurationSeconds, double bufferTargetSeconds);
    void startPlaybackRequest(
        Player::MpvPlayer *activePlayer,
        const QString &url,
        bool pauseWhenReady,
        const QString &loadfileOptions = {});
    void clearCatchupState();
    void setCatchupState(const QString &liveUrl, const QString &programLabel);
    QString catchupLoadfileOptions(double streamBaseOffsetSeconds = -1.0) const;
    void syncCatchupTimelineState();
    bool seekCatchupToTimelinePosition(double targetSeconds);
    bool shouldReloadCatchupForSeek(double targetSeconds) const;
    bool reloadCatchupForTimelineSeek(double targetSeconds);
    QString regeneratedXtreamCatchupUrl(double targetSeconds, double *streamBaseOffsetSeconds = nullptr) const;
    QString recoveryPlaybackUrl() const;
    QString recoveryLoadfileOptions() const;
    Player::MpvPlayer *playbackPlayer();
    const Player::MpvPlayer *playbackPlayer() const;

    Player::MpvPlayer m_player;
    QPointer<Player::MpvPlayer> m_sharedPlaybackPlayer;
    bool m_sharedPlaybackProtected { false };
    QTimer m_positionTimer;
    QTimer m_pauseStateSyncTimer;
    QTimer m_loadingIndicatorDelayTimer;
    QTimer m_startupBufferFallbackTimer;
    QTimer m_startupBufferProbeTimer;
    QTimer m_reconnectAttemptTimer;
    QTimer m_hwdecFallbackTimer;
    QElapsedTimer m_tuneAttemptTimer;
    QElapsedTimer m_reconnectAttemptIssuedTimer;
    QElapsedTimer m_reconnectWatchdogCooldownTimer;
    QElapsedTimer m_steadyStateBufferRetuneTimer;
    QElapsedTimer m_catchupSeekSettleTimer;
    bool m_isPlaying { false };
    bool m_isLoading { false };
    bool m_loadingIndicatorPending { false };
    bool m_isBuffering { false };
    bool m_channelSwitchInProgress { false };
    bool m_channelLoadFailed { false };
    bool m_backendBuffering { false };
    bool m_playbackStalled { false };
    bool m_resumePlaybackAfterLoad { false };
    double m_volume { 100.0 };
    bool m_muted { false };
    double m_lastNonZeroVolume { 100.0 };
    double m_lastPlaybackPositionSeconds { -1.0 };
    int m_stalledPlaybackTickCount { 0 };
    int m_pauseStateSyncRetriesRemaining { 0 };
    bool m_reconnectActive { false };
    bool m_reconnectAttemptInFlight { false };
    bool m_reconnectStabilizing { false };
    int m_reconnectAttemptCount { 0 };
    int m_reconnectRecoveryHealthyTickCount { 0 };
    int m_reconnectRecoveryUnhealthyTickCount { 0 };
    int m_reconnectStabilizationRefillTickCount { 0 };
    int m_noRefillConsecutiveCount { 0 };
    int m_videoFreezeConsecutiveCount { 0 };
    bool m_pauseToggleRequested { false };
    bool m_userPausedManually { false };
    bool m_startupBufferFallbackAppliedForTune { false };
    double m_startupBufferFallbackTargetSeconds { 0.0 };
    double m_waitForDataStreamSeconds { 5.0 };
    bool m_hwdecFallbackApplied { false };
    std::optional<double> m_lastObservedCacheDurationSeconds;
    std::optional<double> m_lastReconnectStabilizationCacheDurationSeconds;
    std::optional<double> m_lastDisplayedVideoFramePtsSeconds;
    std::optional<double> m_averageBitrateBitsPerSecond;
    std::deque<std::pair<qint64, double>> m_debugBitrateSamples;
    double m_lastAdaptiveCacheLimitSeconds { -1.0 };
    double m_lastAdaptiveCacheHysteresisSeconds { -1.0 };
    qint64 m_lastAdaptiveDemuxerMaxBytes { -1 };
    bool m_pauseAfterLoad { false };
    QString m_positionText { QStringLiteral("00:00") };
    QString m_nowPlayingName { QStringLiteral("No channel") };
    std::optional<Core::Channel> m_currentChannel;
    QString m_currentPlaybackUrl;
    QString m_currentLoadfileOptions;
    QString m_playbackMode { QStringLiteral("live") };
    QString m_catchupProgramLabel;
    QString m_livePlaybackUrlBeforeCatchup;
    QString m_catchupCanonicalPlaybackUrl;
    QDateTime m_catchupProgramStartUtc;
    QDateTime m_catchupProgramStopUtc;
    bool m_catchupProgramBoundaryReached { false };
    double m_catchupStreamBaseOffsetSeconds { 0.0 };
    std::optional<double> m_catchupPendingStreamRelativeSeekSeconds;
    qint64 m_catchupTimelineStartEpochMs { 0 };
    qint64 m_catchupTimelineAvailableEdgeEpochMs { 0 };
    double m_catchupTimelineAvailableSeconds { 0.0 };
    double m_catchupTimelinePositionSeconds { 0.0 };
    bool m_catchupTimelineAtLiveEdge { true };
    QString m_catchupTimelineNoticeText;
    bool m_isRecording { false };
    bool m_isRemuxing { false };
    bool m_remuxToMkv { true };
    QString m_remuxTempPath;
    QString m_remuxFinalPath;
    QPointer<TimeshiftController> m_timeshiftController;
    void startRemux(const QString &tempPath, const QString &finalPath);
    void deleteTempRecording(const QString &path, int retriesLeft);
    static QString findFfmpegBinary();
};

} // namespace OKILTV::App
