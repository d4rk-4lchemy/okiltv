#pragma once

#include "../../player/livebuffertuner.h"
#include "../../player/mpvplayer.h"
#include "../../player/catchupstreamsession.h"
#include <deque>
#include <QTimer>
#include <optional>

namespace OKILTV::App::Playback {

// Decisions contain backend commands, never a backend or controller reference.
enum class PauseCommand { None, Pause, Resume };
enum class ReservePhase { Idle, Filling };
struct ReserveDecision {
    PauseCommand pause { PauseCommand::None };
    bool pending { false };
};

class PlaybackBuffering final
{
public:
    static QString catchupBufferOptions(bool standby);
    static Player::CatchupStreamSession::BufferingPolicy catchupOwnedStreamPolicy(bool standby);
    enum class StartupPhase { Idle, Filling, BestEffort };
    PlaybackBuffering();
    void setStartupPending(bool pending) { m_startupPhase = pending ? StartupPhase::Filling : StartupPhase::Idle; }
    bool startupPending() const { return m_startupPhase == StartupPhase::Filling; }
    void armStartup(double target, int segmentHint);
    void resetStartupWatchdog(bool resetTuneState);
    void applyStartupFallback() { m_fallbackApplied = true; m_startupPhase = StartupPhase::BestEffort; }
    bool startupFallbackApplied() const { return m_fallbackApplied; }
    double startupTarget() const { return m_startupTarget; }
    bool startupReady(std::optional<double> cache) const;
    QTimer &startupFallbackTimer() { return m_startupFallbackTimer; }
    QTimer &startupProbeTimer() { return m_startupProbeTimer; }
    enum class StartupPolicy { StrictBuffered, FastLive, BestEffort };
    static StartupPolicy startupPolicy(bool catchup, bool ownedPrimary, bool shared,
                                       bool reconnect, bool softwareFallback, bool timeshift);
    static double adaptiveSteadyStateCacheLimitSeconds(const double bufferTargetSeconds);
    static double adaptiveSteadyStateCacheHysteresisSeconds(const double bufferTargetSeconds);
    static qint64 adaptiveSteadyStateMaxBytes(
    const double bufferTargetSeconds,
    const std::optional<double> averageBitsPerSecond);
    static qint64 adaptiveSteadyStateMaxBackBytes(const std::optional<double> averageBitsPerSecond);
    static qint64 adaptiveCatchupMaxBytes(const std::optional<double> averageBitsPerSecond);
    static qint64 adaptiveCatchupMaxBackBytes(const std::optional<double> averageBitsPerSecond);
    static int startupBufferFallbackTimeoutMs(const double bufferTargetSeconds, const int segmentSecondsHint);

    void resetAverage();
    std::optional<double> observeBitrate(std::optional<double> bitrate, qint64 nowMs);
    std::optional<double> averageBitrate() const { return m_average; }
    void resetAdaptation();
    void interruptDelivery() { m_tuner.interruptObservation(); }
    void observeDelivery(double nowSeconds, double endSeconds, bool idle);
    double targetSeconds(double configured) const { return m_tuner.targetSeconds(configured); }
    double detectedIntervalSeconds() const { return m_tuner.detectedIntervalSeconds(); }
    std::optional<Player::MpvPlayer::SteadyStateBufferingPolicy> retune(
        bool catchup, double configured, std::optional<double> cache, bool fastLive, qint64 nowMs);
    Player::MpvPlayer::SteadyStateBufferingPolicy activeCatchupPolicy(qint64 nowMs);
    ReserveDecision beginLiveReserve(std::optional<double> cache, bool readerIdle, double configured, qint64 nowMs);
    ReserveDecision advanceLiveReserve(std::optional<double> cache, bool readerIdle, bool eof, qint64 elapsedMs, qint64 nowMs);
    ReserveDecision resetLiveReserve(bool resume, bool manuallyPaused, bool preserveDelivery = false);
    void raiseLiveTarget(double configured);
    bool liveReservePending() const { return m_livePhase == ReservePhase::Filling; }
    double liveReserveTarget() const { return m_liveTarget; }
    qint64 liveReserveElapsed(qint64 nowMs) const { return nowMs - m_liveStartedMs; }
    ReserveDecision catchupRefill(std::optional<double> cache, bool healthySession, bool eof,
                                 bool manuallyPaused, bool canStart, qint64 nowMs);
    ReserveDecision resetCatchupRefill(bool resume, bool manuallyPaused);
    bool catchupRefilling() const { return m_catchupPhase == ReservePhase::Filling; }
private:
    StartupPhase m_startupPhase { StartupPhase::Idle };
    bool m_fallbackApplied { false };
    double m_startupTarget { 0.0 };
    QTimer m_startupFallbackTimer;
    QTimer m_startupProbeTimer;
    Player::LiveBufferTuner m_tuner;
    std::deque<std::pair<qint64, double>> m_samples;
    std::optional<double> m_average;
    std::optional<Player::MpvPlayer::SteadyStateBufferingPolicy> m_lastPolicy;
    qint64 m_lastRetuneMs { 0 };
    ReservePhase m_livePhase { ReservePhase::Idle };
    double m_liveTarget { 0.0 };
    qint64 m_liveStartedMs { 0 };
    std::optional<qint64> m_liveRetryMs;
    ReservePhase m_catchupPhase { ReservePhase::Idle };
    qint64 m_catchupStartedMs { 0 };
};
} // namespace OKILTV::App::Playback
