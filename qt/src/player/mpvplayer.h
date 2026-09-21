#pragma once

#include <QTemporaryDir>

#include <QMap>
#include <QJsonObject>
#include <QSet>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <QVariantList>
#include <QtGlobal>

#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace OKILTV::Player {

class CatchupStreamSession;

class MpvPlayer final : public QObject
{
    Q_OBJECT

public:
    struct CacheReadState
    {
        double endSeconds;
        bool idle;
        bool eof;
    };

    struct SteadyStateBufferingPolicy
    {
        double cacheLimitSeconds;
        double hysteresisSeconds;
        qint64 maxBytes;
        qint64 maxBackBytes;
        std::optional<double> refillSeconds {};
    };

    explicit MpvPlayer(QObject *parent = nullptr);
    ~MpvPlayer() override;

    static qint64 demuxerMaxBytesForBufferSeconds(double bufferSeconds);
    static double cacheWindowSecondsForBufferTarget(double bufferTargetSeconds);
    static double steadyStateBackBufferSeconds();
    static double steadyStateCacheLimitSecondsForBufferTarget(double bufferTargetSeconds);
    static double steadyStateCacheHysteresisSecondsForBufferTarget(double bufferTargetSeconds);

    void configureLibraryPath(const QString &path);
    void configureOptions(const QMap<QString, QString> &options);
    void configureImageSmoothing(bool enabled);
    void configurePicturePreset(const QString &preset);
    void configurePlaybackTuning(double waitForDataStreamSeconds, bool deinterlaceEnabled, double bufferSeconds);
    void configureUserAgent(const QString &userAgent);
    void setStartupBufferingStrictMode(bool enabled);
    bool setSteadyStateBufferingPolicy(const SteadyStateBufferingPolicy &policy);
    void resetSteadyStateBuffering();

    QString diagnostics() const;
    bool isAvailable() const;
    bool catchupStreamProtocolAvailable() const;

    void setRenderUpdateTarget(QObject *target);

    bool ensureInitialized();

    void play(const QString &url, const QString &loadfileOptions = {});
    void stop();
    void setHwdec(const QString &mode);
    void togglePause();
    void setPaused(bool paused);
    void setVolume(int volume);
    void setAudioEnabled(bool enabled);
    int requestedVolume() const;
    bool audioEnabledRequested() const;
    void seekRelative(double seconds);
    void seekAbsolute(double seconds);
    void seekAbsoluteFast(double seconds);
    void seekAbsoluteExact(double seconds);
    double position() const;
    std::optional<bool> seekable() const;
    std::optional<bool> pauseState() const;
    std::optional<bool> bufferingState() const;
    std::optional<double> volumePercent() const;
    std::optional<double> demuxerCacheDurationSeconds() const;
    std::optional<std::pair<double, double>> demuxerSeekableRangeSeconds() const;
    std::optional<double> cacheSpeedBytesPerSecond() const;
    std::optional<bool> demuxerCacheReaderEof() const;
    std::optional<CacheReadState> cacheReadState() const;
    double bufferTargetSeconds() const;
    std::optional<int> videoWidth() const;
    std::optional<int> videoHeight() const;
    std::optional<QString> videoCodec() const;
    std::optional<QString> audioCodec() const;
    std::optional<double> videoBitrateBitsPerSecond() const;
    std::optional<double> audioBitrateBitsPerSecond() const;
    std::optional<double> displayedVideoFramePtsSeconds() const;
    std::optional<double> estimatedFrameRateFps() const;
    std::optional<double> sourceFrameRateFps() const;
    std::optional<int> droppedFrameCount() const;
    std::optional<bool> isInterlaced() const;
    bool deinterlaceEnabled() const;

    QVariantList trackList() const;
    void selectAudioTrack(int id, bool remember = false);
    void selectSubtitleTrack(int id, bool remember = false);
    void configureTrackPreferences(const QString &profileId, const QString &channelKey,
                                   const QJsonObject &preferences, bool discardMissing = true);
    bool managesTrackPreferences() const;

    void detectAndApplyDeinterlace();

    bool takeScreenshot(const QString &outputPath);
    bool startStreamRecord(const QString &outputPath);
    void stopStreamRecord();

    void renderToFbo(int fbo, int width, int height);
    void reportSwap();
    qint64 lastRenderTimestampMs() const;
    qint64 lastRenderUpdateTimestampMs() const;

signals:
    void trackListReady(quint64 generation, const QVariantList &tracks);
    void trackPreferenceChanged(const QString &profileId, const QString &channelKey,
                                const QString &type, const QJsonObject &preference);
    void fileLoaded();
    void liveMediaPeriodChanged();
    void videoReconfigured();
    void pauseStateChanged(bool paused);
    void bufferingStateChanged(bool buffering);
    void playbackRestarted();
    void playbackStopped();
    void playbackEnded();
    void errorOccurred(const QString &message);

private:
    void beginTrackLoad(const QString &url);
    QString trackLoadOptions(const QString &options) const;
    void acceptTrackSnapshot(quint64 generation, const QString &path, const QVariantList &tracks);
    bool prepareRememberedTrack(const QString &type, int id);
    void updateTrackPreference(const QString &type, const QJsonObject &preference);
    std::atomic<quint64> m_trackGeneration { 0 };
    std::atomic<quint64> m_tracksLoadedGeneration { 0 };
    quint64 m_readyTrackGeneration { 0 };
    QString m_trackExpectedPath;
    QString m_trackProfileId;
    QString m_trackChannelKey;
    QJsonObject m_trackPreferences;
    bool m_discardMissingTrackPreferences { true };
    QVariantList m_readyTracks;
    QMap<QString, int> m_defaultTrackIds;
    QSet<QString> m_restoredTrackTypes;
    struct PendingTrackChoice {
        int id;
        QJsonObject preference;
    };
    QMap<QString, PendingTrackChoice> m_pendingTrackChoices;
    bool m_stopSubmitted = false;
    void applyPicturePresetLocked();
    struct CachedTelemetry
    {
        double positionSeconds { -1.0 };
        std::optional<bool> seekable;
        std::optional<bool> pauseState;
        std::optional<bool> bufferingState;
        std::optional<double> volumePercent;
        std::optional<double> demuxerCacheDurationSeconds;
        std::optional<CacheReadState> cacheReadState;
        std::optional<std::pair<double, double>> demuxerSeekableRangeSeconds;
        std::optional<double> cacheSpeedBytesPerSecond;
        std::optional<int> videoWidth;
        std::optional<int> videoHeight;
        std::optional<QString> videoCodec;
        std::optional<QString> audioCodec;
        std::optional<double> videoBitrateBitsPerSecond;
        std::optional<double> audioBitrateBitsPerSecond;
        std::optional<double> displayedVideoFramePtsSeconds;
        std::optional<double> estimatedFrameRateFps;
        std::optional<double> sourceFrameRateFps;
        std::optional<int> droppedFrameCount;
        QVariantList trackList;
    };

    struct Api;
    struct State;

    void unload();
    bool loadApi();
    QString resolvedLibraryPath() const;
    bool ensureRenderContext();
    bool registerCatchupStreamProtocol();
    void startEventThread();
    void processEvents();
    void closeLiveStream(const QString &reason);
    bool advanceLiveMediaPeriod();
    void releaseRetiredLiveStreams();
    void queueError(const QString &message);
    void refreshCachedTelemetryFast();
    void refreshCachedTelemetrySlow(bool refreshTracks);
    QVariantList queryTrackList() const;
    std::optional<int> queryDroppedFrameCount() const;
    void requestFrameUpdate();
    void logWindowsRenderStats();
    bool setRuntimeDoubleOption(const char *name, double value);
    bool setRuntimeInt64Option(const char *name, qint64 value);
    bool setPlaybackPropertyAsync(const char *name, int format, void *value, quint64 requestId);
    std::optional<double> propertyDouble(const char *name) const;
    std::optional<int> propertyInt(const char *name) const;
    std::optional<bool> propertyFlag(const char *name) const;
    std::optional<bool> propertyNodeBoolField(const char *prop, const char *key) const;
    std::optional<double> propertyNodeDoubleField(const char *prop, const char *key) const;
    std::optional<std::pair<double, double>> propertyDemuxerSeekableRangeSeconds(std::optional<CacheReadState> *readState = nullptr) const;
    std::optional<QString> propertyString(const char *name) const;

    static void *getProcAddress(void *ctx, const char *name);
    static void onRenderUpdate(void *ctx);

    std::unique_ptr<Api> m_api;
    std::unique_ptr<State> m_state;
    QString m_libraryPath;
    QMap<QString, QString> m_options;
    std::shared_ptr<CatchupStreamSession> m_liveStream;
    QList<std::shared_ptr<CatchupStreamSession>> m_retiredLiveStreams;
    bool m_liveStreamCleanupScheduled { false };
    QTimer m_livePeriodTimer;
    bool m_livePeriodLoading { false };
    bool m_pauseRequested { false };
    double m_waitForDataStreamSeconds { 5.0 };
    bool m_deinterlaceEnabled { true };
    bool m_imageSmoothingEnabled { false };
    QString m_picturePreset { QStringLiteral("standard") };
    QString m_appliedPictureShader;
    std::unique_ptr<QTemporaryDir> m_pictureShaderDirectory;
    double m_bufferSeconds { 3.0 };
    double m_steadyStateCacheLimitSeconds { 6.0 };
    double m_steadyStateCacheHysteresisSeconds { 5.0 };
    qint64 m_steadyStateDemuxerMaxBytes { static_cast<qint64>(96) * 1024 * 1024 };
    qint64 m_steadyStateDemuxerMaxBackBytes { static_cast<qint64>(64) * 1024 * 1024 };
    QString m_userAgent;
    bool m_startupBufferingStrictMode { true };
    double m_liveRefillSeconds { 0.0 };
    bool m_reinitializePending { false };
    std::optional<bool> m_sourceInterlaced;
    bool m_recording { false };
    bool m_audioEnabledRequested { true };
    bool m_audioEnableApplied { false };
    int m_volumeRequested { 100 };
    QString m_diagnostics;
    QPointer<QObject> m_updateTarget;
    std::atomic_bool m_frameUpdateQueued { false };
    CachedTelemetry m_cachedTelemetry;
    std::atomic_bool m_eventThreadRunning { false };
    std::unique_ptr<std::thread> m_eventThread;
    std::atomic_bool m_trackListRefreshPending { false };
    std::atomic_bool m_slowTelemetryRefreshPending { false };
    std::atomic_uint m_renderCount { 0 };
    std::atomic_uint m_renderUpdateCount { 0 };
    bool m_windowsPacingDiagEnabled { false };
    QTimer m_windowsRenderStatsTimer;
    std::atomic<qint64> m_lastRenderUpdateTimestampMs { -1 };
    std::atomic<qint64> m_lastRenderTimestampMs { -1 };
    std::atomic<qint64> m_lastPacingDiagnosticLogTimestampMs { -1 };
    std::atomic<qint64> m_lastWindowsRenderStatsLogTimestampMs { -1 };
    std::atomic_uint m_renderCallsSinceLastStats { 0 };
    std::atomic_uint m_renderUpdateCallbacksSinceLastStats { 0 };
    std::atomic<quint64> m_renderTimeTotalMicrosSinceLastStats { 0 };
    std::atomic_uint m_renderTimeMaxMicrosSinceLastStats { 0 };
};

} // namespace OKILTV::Player
