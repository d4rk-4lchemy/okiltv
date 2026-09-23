#pragma once

#include "../core/models.h"
#include "../player/catchupstreamsession.h"
#include "../player/mpvplayer.h"
#include "playback/playbackbuffering.h"
#include "playback/playbackrecovery.h"
#include "playback/catchupplaybacksession.h"

#include <QObject>
#include <QSet>
#include <QElapsedTimer>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <deque>
#include <optional>
#include <utility>

namespace OKILTV::Core { class SettingsManager; }

namespace OKILTV::App {

class TimeshiftController;

struct CatchupProgressSample
{
    Core::Channel channel;
    QDateTime programStart;
    QDateTime programStop;
    QDateTime watchedTime;
    bool endless { false };
};

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
    Q_PROPERTY(QObject *playbackPlayerObject READ playbackPlayerObject NOTIFY playbackPlayerObjectChanged)
    Q_PROPERTY(QObject *seamlessStandbyPlayerObject READ seamlessStandbyPlayerObject NOTIFY seamlessStandbyPlayerObjectChanged)
    Q_PROPERTY(bool seamlessStandbyPrewarmActive READ seamlessStandbyPrewarmActive NOTIFY seamlessStandbyPrewarmActiveChanged)
    Q_PROPERTY(QString playbackMode READ playbackMode NOTIFY playbackModeChanged)
    Q_PROPERTY(QString catchupProgramLabel READ catchupProgramLabel NOTIFY catchupProgramLabelChanged)
    Q_PROPERTY(QVariantMap catchupCurrentProgram READ catchupCurrentProgram NOTIFY catchupTimelineChanged)
    Q_PROPERTY(bool catchupTimelineActive READ catchupTimelineActive NOTIFY catchupTimelineChanged)
    Q_PROPERTY(qint64 catchupTimelineStartEpochMs READ catchupTimelineStartEpochMs NOTIFY catchupTimelineChanged)
    Q_PROPERTY(qint64 catchupTimelineAvailableEdgeEpochMs READ catchupTimelineAvailableEdgeEpochMs NOTIFY catchupTimelineChanged)
    Q_PROPERTY(double catchupTimelineAvailableSeconds READ catchupTimelineAvailableSeconds NOTIFY catchupTimelineChanged)
    Q_PROPERTY(qint64 catchupTimelineEndEpochMs READ catchupTimelineEndEpochMs NOTIFY catchupTimelineChanged)
    Q_PROPERTY(double catchupTimelineDurationSeconds READ catchupTimelineDurationSeconds NOTIFY catchupTimelineChanged)
    Q_PROPERTY(double catchupTimelinePositionSeconds READ catchupTimelinePositionSeconds NOTIFY catchupTimelineChanged)
    Q_PROPERTY(bool catchupTimelineAtLiveEdge READ catchupTimelineAtLiveEdge NOTIFY catchupTimelineChanged)
    Q_PROPERTY(QString catchupTimelineNoticeText READ catchupTimelineNoticeText NOTIFY catchupTimelineChanged)
    Q_PROPERTY(bool liveBufferActive READ liveBufferActive NOTIFY liveBufferStateChanged)
    Q_PROPERTY(qint64 liveBufferWindowStartEpochMs READ liveBufferWindowStartEpochMs NOTIFY liveBufferStateChanged)
    Q_PROPERTY(qint64 liveBufferLiveEdgeEpochMs READ liveBufferLiveEdgeEpochMs NOTIFY liveBufferStateChanged)
    Q_PROPERTY(double liveBufferAvailableSeconds READ liveBufferAvailableSeconds NOTIFY liveBufferStateChanged)
    Q_PROPERTY(double liveBufferPositionSeconds READ liveBufferPositionSeconds NOTIFY liveBufferStateChanged)
    Q_PROPERTY(double liveBufferBehindLiveSeconds READ liveBufferBehindLiveSeconds NOTIFY liveBufferStateChanged)
    Q_PROPERTY(bool liveBufferAtLiveEdge READ liveBufferAtLiveEdge NOTIFY liveBufferStateChanged)
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
    void setTrackPreferenceSettings(Core::SettingsManager *settings);

    bool isPlaying() const;
    bool isLoading() const;
    bool isBuffering() const;
    bool channelSwitchInProgress() const;
    bool channelLoadFailed() const;
    double volume() const;
    bool muted() const;
    void setVolume(double value);
    void copyVolumeStateFrom(const PlayerController &other);
    QString positionText() const;
    QString nowPlayingName() const;
    QVariantMap currentChannel() const;
    QObject *playbackPlayerObject() const;
    QObject *seamlessStandbyPlayerObject() const;
    bool seamlessStandbyPrewarmActive() const;
    QString playbackMode() const;
    QString catchupProgramLabel() const;
    void setDateTimeFormat(Core::DateTimeFormatOptions options);
    QVariantMap catchupCurrentProgram() const;
    bool catchupTimelineActive() const;
    qint64 catchupTimelineStartEpochMs() const;
    qint64 catchupTimelineAvailableEdgeEpochMs() const;
    double catchupTimelineAvailableSeconds() const;
    qint64 catchupTimelineEndEpochMs() const;
    double catchupTimelineDurationSeconds() const;
    double catchupTimelinePositionSeconds() const;
    bool catchupTimelineAtLiveEdge() const;
    QString catchupTimelineNoticeText() const;
    bool liveBufferActive() const;
    qint64 liveBufferWindowStartEpochMs() const;
    qint64 liveBufferLiveEdgeEpochMs() const;
    double liveBufferAvailableSeconds() const;
    double liveBufferPositionSeconds() const;
    double liveBufferBehindLiveSeconds() const;
    bool liveBufferAtLiveEdge() const;
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
    static qint64 adaptiveSteadyStateMaxBackBytes(std::optional<double> averageBitsPerSecond);
    static qint64 adaptiveCatchupMaxBytes(std::optional<double> averageBitsPerSecond);
    static qint64 adaptiveCatchupMaxBackBytes(std::optional<double> averageBitsPerSecond);
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
        bool remuxRecordingsToMkv = true,
        bool imageSmoothingEnabled = false,
        const QString &picturePreset = QStringLiteral("standard"));
    void playChannel(const Core::Channel &channel);
    void playCatchupChannel(const Core::Channel &channel, const QString &catchupUrl, const QString &programLabel);
    void playCatchupChannel(
        const Core::Channel &channel,
        const QString &catchupUrl,
        const QString &programLabel,
        const QDateTime &programStartUtc,
        const QDateTime &programStopUtc,
        const QString &canonicalCatchupUrl = {},
        std::optional<double> initialProgramSeekSeconds = std::nullopt,
        std::optional<double> initialStreamBaseOffsetSeconds = std::nullopt,
        std::optional<double> initialTimelinePositionSeconds = std::nullopt,
        int safetySeconds = 180,
        bool endless = false,
        std::optional<Core::EpgEntry> program = std::nullopt);
    std::optional<Core::EpgEntry> validatedCatchupProgramme() const { return m_catchupSession.validatedProgram(); }
    void updateCatchupProgramme(const std::optional<Core::EpgEntry> &program);
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
    void checkpointCatchupProgress();
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
    void catchupPlaybackTimeChanged(const QDateTime &time);
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
    void playbackPlayerObjectChanged();
    void seamlessStandbyPlayerObjectChanged();
    void seamlessStandbyPrewarmActiveChanged();
    void playbackModeChanged();
    void catchupProgramLabelChanged();
    void catchupProgressObserved(const OKILTV::App::CatchupProgressSample &sample);
    void catchupProgressFlushRequested();
    void catchupTimelineChanged();
    void timeshiftStateChanged();
    void liveBufferStateChanged();
    void playbackChannelActivated(int channelId);
    void playbackFileLoaded();
    void playbackError(const QString &message);

private:
    void configurePlaybackTrackPreferences(Player::MpvPlayer *player, bool discardMissing = true);
    Core::SettingsManager *m_trackPreferenceSettings { nullptr };
    using StartupPolicy = Playback::PlaybackBuffering::StartupPolicy;

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
    bool advanceReconnectReserve();
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
    StartupPolicy startupPolicyForPlaybackRequest(Player::MpvPlayer *activePlayer) const;
    static QString startupPolicyLabel(StartupPolicy policy);
    void updatePosition();
    bool evaluateCatchupRebuffering(std::optional<double> cacheSeconds, bool readerEof);
    void resetCatchupRebuffering(bool resume);
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
    void sampleLiveDelivery();
    void observeLiveDelivery(const Player::MpvPlayer::CacheReadState &readState);
    bool evaluateLiveReserve();
    bool advanceLiveReserve(std::optional<double> cacheSeconds,
                            const std::optional<Player::MpvPlayer::CacheReadState> &readState,
                            qint64 elapsedMs);
    bool beginLiveReserve(std::optional<double> cacheSeconds, bool readerIdle);
    void resetLiveReserve(bool resumePlayback, bool preserveDeliveryObservation = false);
    double effectiveLiveBufferTargetSeconds() const;
    void maybeRetuneSteadyStateBuffering(std::optional<double> cacheDurationSeconds, double bufferTargetSeconds);
    void maybeRetuneCatchupBuffering(std::optional<double> cacheDurationSeconds);
    void applyActiveCatchupBufferingPolicy(Player::MpvPlayer *player);
    void startPlaybackRequest(
        Player::MpvPlayer *activePlayer,
        const QString &url,
        bool pauseWhenReady,
        const QString &loadfileOptions = {});
    void clearCatchupState();
    void setCatchupState(const QString &liveUrl, const QString &programLabel);
    QString catchupLoadfileOptions(double streamBaseOffsetSeconds = -1.0, bool standby = false) const;
    void syncCatchupTimelineState();
    void resetCatchupUrlLoadGuardState(bool initialLoadContext = false);
    void maybeCorrectUnexpectedCatchupRollback(double currentStreamSeconds);
    bool seekCatchupToTimelinePosition(double targetSeconds);
    bool shouldReloadCatchupForSeek(double targetSeconds) const;
    bool reloadCatchupForTimelineSeek(double targetSeconds);
    bool advanceCatchupRecoveryAlignment();
    bool recoverFailedContinuousCatchup();
    bool advanceCatchupMediaPeriod();
    bool recoverCatchupAtAutomaticEof(bool readerEof);
    bool shouldExtendCatchupRollingWindowPredictively(double currentStreamSeconds) const;
    bool extendCatchupRollingWindow(const QString &reason, bool fromPredictiveTrigger);
    bool seamlessCatchupRollingEnabled() const;
    bool canUseSeamlessCatchupRolling() const;
    bool armSeamlessCatchupRollingExtension(
        const QString &reason,
        double targetSeconds,
        const QString &regeneratedUrl,
        double streamBaseOffsetSeconds);
    bool startSeamlessCatchupStandbyLoad();
    bool refreshSeamlessCatchupStandbyRetryUrl();
    bool maybeCommitSeamlessCatchupCutover(const QString &reason, bool forceWithoutNearEdge = false);
    bool standbyCatchupSessionHealthyForCutover() const;
    void markSeamlessStandbyAttemptFailed(const QString &reason);
    void abortSeamlessCatchupRolling(const QString &reason, bool stopStandbyPlayer);
    void setSeamlessStandbyPrewarmState(bool active, Player::MpvPlayer *standbyPlayer = nullptr);
    bool standbySeamlessVideoReady() const;
    bool maybeStopCatchupAtProgrammeBoundary(
        std::optional<double> currentStreamSeconds,
        std::optional<double> remainingBufferedSeconds,
        const QString &reason);
    bool handleCatchupPlaybackEndedRecovery();
    Player::MpvPlayer *seamlessCatchupStandbyPlayer();
    bool launchSeamlessCatchupStandbyLoad(Player::MpvPlayer *standbyPlayer);
    QString prepareCatchupStreamPlaybackUrl(Player::MpvPlayer *targetPlayer, const QString &sourceUrl, bool standby);
    bool activeCatchupProviderConnectionClosed() const;
    bool beginCatchupTimelineReload(double targetSeconds, const QString &regeneratedUrl, double streamBaseOffsetSeconds);
    void runCatchupTimelineReload();
    void processPendingCatchupTimelineReload();
    void finishCatchupTimelineReload(const QString &reason);
    void setCatchupTimelineNoticeText(const QString &noticeText, int autoClearMs = 0);
    void resetCatchupDegradationRecoveryState();
    void evaluateCatchupDegradationRecovery(
        std::optional<double> cacheDurationSeconds,
        std::optional<double> cacheSpeedBytesPerSecond,
        bool playbackAdvanced,
        std::optional<bool> framePtsAdvanced);
    bool hardRestoreCatchupAtCurrentTimelinePoint(const QString &reason);
    bool canRegenerateCatchupUrl() const;
    QString regeneratedCatchupUrl(double targetSeconds, double *streamBaseOffsetSeconds = nullptr) const;
    void clearLiveBufferState();
    void syncLiveBufferState();
    bool seekLiveBufferToPosition(double targetSeconds);
    QString recoveryPlaybackUrl() const;
    QString recoveryLoadfileOptions() const;
    void setSharedPlaybackPlayer(Player::MpvPlayer *player, bool protectedSession);
    Player::MpvPlayer *playbackPlayer();
    const Player::MpvPlayer *playbackPlayer() const;

    Playback::PlaybackBuffering m_buffering;
    Playback::PlaybackRecovery m_recovery;
    Playback::CatchupPlaybackSession m_catchupSession;
    QSet<Player::MpvPlayer *> m_playbackSignalSources;
    Player::MpvPlayer m_player;
    Player::MpvPlayer m_catchupStandbyPlayer;
    QPointer<Player::MpvPlayer> m_sharedPlaybackPlayer;
    bool m_sharedPlaybackProtected { false };
    quint64 m_playbackGeneration { 0 };
    QTimer m_positionTimer;
    QTimer m_liveDeliveryTimer;
    QElapsedTimer m_liveDeliveryClock;
    QTimer m_pauseStateSyncTimer;
    QTimer m_loadingIndicatorDelayTimer;
    QTimer m_catchupTimelineNoticeClearTimer;
    QTimer m_hwdecFallbackTimer;
    QElapsedTimer m_tuneAttemptTimer;
    bool m_isPlaying { false };
    bool m_isLoading { false };
    bool m_loadingIndicatorPending { false };
    bool m_loadingPlaybackFileLoaded { false };
    bool m_loadingPlaybackReady { false };
    bool m_isBuffering { false };
    bool m_channelSwitchInProgress { false };
    bool m_channelLoadFailed { false };
    bool m_backendBuffering { false };
    double m_volume { 100.0 };
    bool m_muted { false };
    double m_lastNonZeroVolume { 100.0 };
    int m_pauseStateSyncRetriesRemaining { 0 };
    bool m_pauseToggleRequested { false };
    bool m_userPausedManually { false };
    double m_waitForDataStreamSeconds { 5.0 };
    bool m_hwdecFallbackApplied { false };
    bool m_pauseAfterLoad { false };
    QString m_positionText { QStringLiteral("00:00") };
    QString m_nowPlayingName { QStringLiteral("No channel") };
    Player::CatchupStreamSession::HeaderList m_catchupRequestHeaders;
    std::optional<Core::Channel> m_currentChannel;
    QString m_currentPlaybackUrl;
    QString m_currentLoadfileOptions;
    QString m_playbackMode { QStringLiteral("live") };
    Core::DateTimeFormatOptions m_dateTimeFormat;
    void publishCatchupProgress(double streamSeconds);
    QPointer<Player::MpvPlayer> m_catchupSeamlessStandbyPlayer;
    QPointer<Player::MpvPlayer> m_catchupSeamlessPrewarmPlayer;
    bool m_catchupSeamlessPrewarmActive { false };
    Player::CatchupStreamSession::Ptr m_catchupActiveStreamSession;
    Player::CatchupStreamSession::Ptr m_catchupStandbyStreamSession;
    QString m_catchupTimelineNoticeText;
    QString m_catchupTimelineNoticeAutoClearText;
    bool m_liveBufferActive { false };
    qint64 m_liveBufferWindowStartEpochMs { 0 };
    qint64 m_liveBufferLiveEdgeEpochMs { 0 };
    double m_liveBufferAvailableSeconds { 0.0 };
    double m_liveBufferPositionSeconds { 0.0 };
    double m_liveBufferBehindLiveSeconds { 0.0 };
    bool m_liveBufferAtLiveEdge { true };
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
