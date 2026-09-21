#pragma once
#include <QString>
#include <QTimer>
#include <QtTypes>
#include <optional>

namespace OKILTV::App::Playback {
struct StreamHealth {
    std::optional<double> cacheDurationSeconds;
    std::optional<double> cacheSpeedBytesPerSecond;
    double bufferTargetSeconds { 0.0 };
};
struct RecoveryContext {
    bool catchup { false };
    bool hasChannel { false };
    bool failed { false };
    bool manuallyPaused { false };
    bool startupPending { false };
    bool buffering { false };
    bool stalled { false };
    bool hasVideo { false };
    std::optional<qint64> tuneElapsedMs;
    double waitSeconds { 5.0 };
};
class PlaybackRecovery final {
public:
    static bool deadStreamLikelyDisconnected(
    const std::optional<double> cacheDurationSeconds,
    const std::optional<double> throughputBitsPerSecond);
    static bool shouldStartPreemptiveReconnect(
    const std::optional<double> cacheDurationSeconds,
    const double bufferTargetSeconds,
    const std::optional<double> throughputBitsPerSecond,
    const bool playbackAdvanced,
    const bool backendBuffering);
    static int reconnectDepletionTimeoutMsForWaitSeconds(const double waitForDataStreamSeconds);
    void clearProgress();
    bool observePosition(double seconds, bool detectStall);
    void markPausedStall(bool manuallyPaused);
    bool stalled() const { return m_playbackStalled; }
    double lastPosition() const { return m_lastPlaybackPositionSeconds; }
    PlaybackRecovery();
    QTimer &attemptTimer() { return m_attemptTimer; }
    enum class Phase { Idle, Ready, WaitingStop, Loading, LoadingReserve, RebuildingReserve, Stabilizing };
    enum class Command { None, Stop, Load, Exhausted };
    struct ReserveResult {
        enum class Action { None, Pause, Resume, Stop };
        Action action { Action::None };
        bool holdSample { false };
        QString reason;
    };
    void start();
    void stop(bool live, qint64 nowMs);
    void clearAttempt();
    Command nextAttempt(qint64 nowMs);
    void loaded(qint64 nowMs);
    void beginLoad(bool reserve, double target, qint64 nowMs);
    bool totalExpired(int waitMs, qint64 nowMs) const;
    QString attemptTimeout(int waitMs, int sampleIntervalMs, qint64 nowMs);
    ReserveResult reserve(std::optional<double> cache, bool idle, bool eof, bool buffering,
                          double waitSeconds, qint64 nowMs);
    bool active() const { return m_phase != Phase::Idle; }
    bool attemptInFlight() const;
    bool waitingStop() const { return m_phase == Phase::WaitingStop; }
    bool stabilizing() const { return m_phase == Phase::Stabilizing; }
    bool reservePending() const { return m_phase == Phase::LoadingReserve || m_phase == Phase::RebuildingReserve; }
    bool fileLoaded() const { return m_reconnectFileLoaded; }
    int attempts() const { return m_reconnectAttemptCount; }
    bool recovered() const;
    bool unstable() const;
    void resetStabilization();
    void resetNoRefill();
    void resetVideoFreeze();
    void clearWatchdogs();
    std::optional<bool> observeFrame(std::optional<double> frame);
    QString noRefill(const StreamHealth &, const RecoveryContext &, bool playbackAdvanced, std::optional<bool> framePtsAdvanced, qint64 nowMs);
    QString videoFreeze(const StreamHealth &, const RecoveryContext &, bool playbackAdvanced, std::optional<bool> framePtsAdvanced, qint64 nowMs);
    bool observeRecovery(const StreamHealth &, const RecoveryContext &, bool playbackAdvanced, std::optional<bool> framePtsAdvanced, qint64 nowMs);
private:
    int m_stalledPlaybackTickCount { 0 };
    double m_lastPlaybackPositionSeconds { -1.0 };
    bool m_playbackStalled { false };
    QTimer m_attemptTimer;
    Phase m_phase { Phase::Idle };
    int m_reconnectAttemptCount { 0 };
    bool m_reconnectFileLoaded { false };
    bool m_reconnectReserveReady { false };
    double m_reserveTarget { 0.0 };
    double m_reserveHighWater { 0.0 };
    qint64 m_reserveProgressMs { 0 };
    qint64 m_totalStartedMs { 0 };
    qint64 m_attemptStartedMs { 0 };
    qint64 m_stopIssuedMs { 0 };
    std::optional<qint64> m_cooldownMs;
    int m_reconnectRecoveryHealthyTickCount { 0 };
    int m_reconnectRecoveryUnhealthyTickCount { 0 };
    int m_reconnectStabilizationRefillTickCount { 0 };
    int m_noRefillConsecutiveCount { 0 };
    int m_videoFreezeConsecutiveCount { 0 };
    std::optional<double> m_lastObservedCacheDurationSeconds;
    std::optional<double> m_lastReconnectStabilizationCacheDurationSeconds;
    std::optional<double> m_lastDisplayedVideoFramePtsSeconds;
};
} // namespace OKILTV::App::Playback
