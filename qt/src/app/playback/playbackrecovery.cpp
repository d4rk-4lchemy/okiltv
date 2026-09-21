#include "playbackrecovery.h"
#include "../../core/debuglogger.h"
#include <algorithm>
#include <cmath>
namespace OKILTV::App::Playback {
namespace {
constexpr double kPreemptiveReconnectBufferRatioThreshold = 0.40;
constexpr double kDeadStreamLowThroughputBitsPerSecond = 1024.0;
constexpr int kReconnectPostDepletionGraceMs = 10000;
constexpr int kReconnectMaxAttempts = 5;
constexpr int kReconnectTransportSettleMs = 450;
constexpr int kReconnectStabilizationStableTickThreshold = 12;
constexpr int kReconnectStabilizationUnstableTickThreshold = 3;
constexpr int kReconnectStabilizationMinRefillTicks = 2;
constexpr int kNoRefillTickThreshold = 3;
constexpr int kVideoFreezeTickThreshold = 3;
constexpr int kDecoderStallTickThreshold = 6;
constexpr double kVideoFramePtsEpsilonSeconds = 0.03;
constexpr double kReconnectDepletedBufferThresholdSeconds = 0.05;
constexpr double kNoRefillCacheSpeedThresholdBytesPerSecond = 1024.0;
constexpr double kNoRefillCacheIncreaseEpsilonSeconds = 0.05;
constexpr int kSoftReconnectWatchdogCooldownMs = 15000;
bool playerTraceEnabled() { static const bool enabled = qEnvironmentVariableIsSet("OKILTV_TRACE_PLAYER"); return enabled; }
double normalizedBufferTargetSeconds(double value) { return std::isfinite(value) ? std::clamp(value, 0.1, 60.0) : 3.0; }
double normalizedWaitForDataStreamSeconds(double value) { return std::isfinite(value) ? std::clamp(std::round(value * 10.0) / 10.0, 0.1, 120.0) : 5.0; }
}
PlaybackRecovery::PlaybackRecovery() { m_attemptTimer.setInterval(1000); }
void PlaybackRecovery::start() {
    m_phase = Phase::Ready;
    m_reconnectAttemptCount = 0;
    clearAttempt();
    clearWatchdogs();
}
void PlaybackRecovery::stop(bool live, qint64 nowMs) {
    m_attemptTimer.stop();
    const auto wasActive = active();
    clearAttempt();
    m_phase = Phase::Idle;
    m_reconnectAttemptCount = 0;
    clearWatchdogs();
    if (wasActive && live) m_cooldownMs = nowMs;
}
void PlaybackRecovery::resetStabilization() {
    if (stabilizing()) m_phase = Phase::Loading;
    m_reconnectRecoveryHealthyTickCount = 0;
    m_reconnectRecoveryUnhealthyTickCount = 0;
    m_reconnectStabilizationRefillTickCount = 0;
    m_lastReconnectStabilizationCacheDurationSeconds.reset();
}
void PlaybackRecovery::clearAttempt() {
    if (active()) m_phase = Phase::Ready;
    m_reconnectFileLoaded = false;
    m_reconnectReserveReady = false;
    resetStabilization();
}
bool PlaybackRecovery::attemptInFlight() const {
    return m_phase == Phase::Loading || m_phase == Phase::LoadingReserve
        || m_phase == Phase::RebuildingReserve || m_phase == Phase::Stabilizing;
}
PlaybackRecovery::Command PlaybackRecovery::nextAttempt(qint64 nowMs) {
    if (!active() || attemptInFlight()) return Command::None;
    if (m_reconnectAttemptCount >= kReconnectMaxAttempts) return Command::Exhausted;
    if (!waitingStop()) {
        m_phase = Phase::WaitingStop;
        m_stopIssuedMs = nowMs;
        return Command::Stop;
    }
    if (nowMs - m_stopIssuedMs < kReconnectTransportSettleMs) return Command::None;
    return Command::Load;
}
// Preserve the established positional contract; parameter names identify their roles.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void PlaybackRecovery::beginLoad(bool reserve, double target, qint64 nowMs) {
    ++m_reconnectAttemptCount;
    resetStabilization();
    m_phase = reserve ? Phase::LoadingReserve : Phase::Loading;
    m_reconnectFileLoaded = false;
    m_reconnectReserveReady = false;
    m_reserveTarget = std::max(3.0, target);
    m_reserveHighWater = 0.0;
    m_reserveProgressMs = m_totalStartedMs = m_attemptStartedMs = nowMs;
}
void PlaybackRecovery::loaded(qint64 nowMs) {
    m_reconnectFileLoaded = true;
    m_reserveProgressMs = nowMs;
}
bool PlaybackRecovery::totalExpired(int waitMs, qint64 nowMs) const {
    return attemptInFlight() && nowMs - m_totalStartedMs >= std::max(120000, waitMs * 3);
}
// Preserve the established positional contract; parameter names identify their roles.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QString PlaybackRecovery::attemptTimeout(int waitMs, int sampleIntervalMs, qint64 nowMs) {
    if (!attemptInFlight()) return {};
    const auto limit = stabilizing() ? std::max(waitMs, (kReconnectStabilizationStableTickThreshold + 3) * sampleIntervalMs) : waitMs;
    if (nowMs - m_attemptStartedMs < limit) return {};
    return stabilizing() ? QStringLiteral("stabilization-timeout") : QStringLiteral("attempt-timeout");
}
PlaybackRecovery::ReserveResult PlaybackRecovery::reserve(std::optional<double> cache, bool idle, bool eof,
// Preserve the established positional contract; parameter names identify their roles.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    bool buffering, double waitSeconds, qint64 nowMs) {
    if (!active() || !attemptInFlight() || !m_reconnectFileLoaded) return {};
    const auto valid = cache.has_value() && std::isfinite(*cache) && *cache >= 0.0;
    auto action = ReserveResult::Action::None;
    if (!reservePending()) {
        if (!valid || (*cache > 1.0 && !buffering)) return {};
        m_phase = Phase::RebuildingReserve;
        m_reconnectReserveReady = false;
        m_reserveHighWater = 0.0;
        m_reserveProgressMs = nowMs;
        action = ReserveResult::Action::Pause;
    }
    if (valid && *cache > m_reserveHighWater + 0.1) {
        m_reserveHighWater = *cache;
        m_reserveProgressMs = nowMs;
    }
    const auto capacity = idle && !eof && nowMs - m_reserveProgressMs >= 1000 && valid && *cache > 1.0;
    if (!eof && ((valid && *cache >= m_reserveTarget) || capacity)) {
        m_phase = Phase::Loading;
        m_reconnectReserveReady = true;
        resetStabilization();
        m_attemptStartedMs = nowMs;
        return {ReserveResult::Action::Resume, false, {}};
    }
    const auto limit = static_cast<qint64>(1000.0 * std::max(normalizedWaitForDataStreamSeconds(waitSeconds),
        std::clamp(m_reserveTarget * 2.0 + 2.0, 10.0, 120.0)));
    if (eof || nowMs - m_reserveProgressMs >= limit) {
        clearAttempt();
        return {ReserveResult::Action::Stop, true, eof ? QStringLiteral("reserve-eof") : QStringLiteral("reserve-no-progress")};
    }
    return {action, true, {}};
}
bool PlaybackRecovery::recovered() const {
    return stabilizing() && m_reconnectRecoveryHealthyTickCount >= kReconnectStabilizationStableTickThreshold
        && m_reconnectStabilizationRefillTickCount >= kReconnectStabilizationMinRefillTicks;
}
bool PlaybackRecovery::unstable() const {
    return stabilizing() && m_reconnectRecoveryUnhealthyTickCount >= kReconnectStabilizationUnstableTickThreshold;
}
void PlaybackRecovery::resetNoRefill() { m_noRefillConsecutiveCount = 0; m_lastObservedCacheDurationSeconds.reset(); }
void PlaybackRecovery::resetVideoFreeze() { m_videoFreezeConsecutiveCount = 0; }
void PlaybackRecovery::clearWatchdogs() {
    resetNoRefill(); resetVideoFreeze(); m_lastDisplayedVideoFramePtsSeconds.reset();
}
std::optional<bool> PlaybackRecovery::observeFrame(std::optional<double> frame) {
    if (!frame || !std::isfinite(*frame)) { m_lastDisplayedVideoFramePtsSeconds.reset(); return std::nullopt; }
    const auto previous = m_lastDisplayedVideoFramePtsSeconds;
    m_lastDisplayedVideoFramePtsSeconds = frame;
    if (!previous) return std::nullopt;
    return std::abs(*frame - *previous) > kVideoFramePtsEpsilonSeconds;
}

QString PlaybackRecovery::noRefill(const StreamHealth &health, const RecoveryContext &context,
    bool playbackAdvanced, std::optional<bool> framePtsAdvanced, qint64 nowMs)
{

    if (context.catchup) {
        m_noRefillConsecutiveCount = 0;
        m_lastObservedCacheDurationSeconds = std::nullopt;
        return {};
    }
    if (!context.hasChannel
        || context.failed
        || context.manuallyPaused
        || context.startupPending
        || active()
        || (context.tuneElapsedMs.has_value()
            && static_cast<double>(context.tuneElapsedMs.value_or(0)) < normalizedWaitForDataStreamSeconds(context.waitSeconds) * 1000.0)) {
        m_noRefillConsecutiveCount = 0;
        m_lastObservedCacheDurationSeconds = std::nullopt;
        return {};
    }

    const auto hasCacheSample = health.cacheDurationSeconds.has_value()
        && std::isfinite(health.cacheDurationSeconds.value())
        && health.cacheDurationSeconds.value() >= 0.0;
    const auto normalizedCacheDuration = hasCacheSample
        ? std::max(0.0, health.cacheDurationSeconds.value())
        : 0.0;
    const auto normalizedBufferTarget = normalizedBufferTargetSeconds(health.bufferTargetSeconds);
    const auto bufferNeedsRefill = hasCacheSample
        && std::isfinite(health.bufferTargetSeconds)
        && health.bufferTargetSeconds > 0.0
        && normalizedCacheDuration <= normalizedBufferTarget + kNoRefillCacheIncreaseEpsilonSeconds;
    const auto cacheCriticallyLow = hasCacheSample
        && normalizedCacheDuration <= kReconnectDepletedBufferThresholdSeconds;
    const auto hasCacheSpeedSample = health.cacheSpeedBytesPerSecond.has_value()
        && std::isfinite(health.cacheSpeedBytesPerSecond.value())
        && health.cacheSpeedBytesPerSecond.value() >= 0.0;
    const auto normalizedCacheSpeed = hasCacheSpeedSample
        ? std::max(0.0, health.cacheSpeedBytesPerSecond.value())
        : 0.0;
    const auto cacheSpeedRefilling = hasCacheSpeedSample
        && normalizedCacheSpeed > kNoRefillCacheSpeedThresholdBytesPerSecond;

    bool cacheRefilling = false;
    bool cacheBaselineOnly = false;
    bool cacheDraining = false;
    if (hasCacheSample) {
        if (m_lastObservedCacheDurationSeconds.has_value()) {
            cacheRefilling = normalizedCacheDuration
                > m_lastObservedCacheDurationSeconds.value() + kNoRefillCacheIncreaseEpsilonSeconds;
            cacheDraining = normalizedCacheDuration + kNoRefillCacheIncreaseEpsilonSeconds
                < m_lastObservedCacheDurationSeconds.value();
        } else {
            cacheBaselineOnly = true;
        }
        m_lastObservedCacheDurationSeconds = normalizedCacheDuration;
    } else {
        m_lastObservedCacheDurationSeconds = std::nullopt;
    }

    const auto frameHealthy = framePtsAdvanced.value_or(playbackAdvanced);
    if (!context.buffering && !context.stalled && playbackAdvanced && frameHealthy && !cacheDraining && !cacheCriticallyLow) {
        m_noRefillConsecutiveCount = 0;
        return {};
    }

    if (m_cooldownMs.has_value()
        && (nowMs - m_cooldownMs.value_or(nowMs)) < kSoftReconnectWatchdogCooldownMs) {
        m_noRefillConsecutiveCount = 0;
        return {};
    }

    if (!bufferNeedsRefill && !cacheCriticallyLow) {
        m_noRefillConsecutiveCount = 0;
        return {};
    }

    if (cacheSpeedRefilling || cacheRefilling) {
        m_noRefillConsecutiveCount = 0;
        return {};
    }

    if (!hasCacheSample && !hasCacheSpeedSample) {
        m_noRefillConsecutiveCount = 0;
        return {};
    }

    if (cacheBaselineOnly && !hasCacheSpeedSample) {
        m_noRefillConsecutiveCount = 0;
        return {};
    }

    const auto starvationSignal = cacheCriticallyLow || context.buffering || context.stalled;
    if (!starvationSignal) {
        m_noRefillConsecutiveCount = 0;
        return {};
    }

    m_noRefillConsecutiveCount += 1;
    if (playerTraceEnabled() || m_noRefillConsecutiveCount >= kNoRefillTickThreshold) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "No-refill watchdog sample %1/%2: cache=%3 target=%4 cache-speed=%5B/s buffering=%6 stalled=%7 draining=%8.")
                .arg(m_noRefillConsecutiveCount)
                .arg(kNoRefillTickThreshold)
                .arg(
                    hasCacheSample
                        ? QString::number(normalizedCacheDuration, 'f', 3)
                        : QStringLiteral("N/A"))
                .arg(QString::number(normalizedBufferTarget, 'f', 3))
                .arg(
                    hasCacheSpeedSample
                        ? QString::number(normalizedCacheSpeed, 'f', 0)
                        : QStringLiteral("N/A"))
                .arg(context.buffering ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(context.stalled ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(cacheDraining ? QStringLiteral("true") : QStringLiteral("false")));
    }
    if (m_noRefillConsecutiveCount >= kNoRefillTickThreshold) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("No-refill watchdog threshold reached; triggering playback recovery."));
        return QStringLiteral("no-refill-watchdog");
    }
    return {};
}

QString PlaybackRecovery::videoFreeze(const StreamHealth &health, const RecoveryContext &context,
    bool playbackAdvanced, std::optional<bool> framePtsAdvanced, qint64 nowMs)
{

    if (context.catchup) {
        m_videoFreezeConsecutiveCount = 0;
        return {};
    }
    if (!context.hasChannel
        || context.failed
        || context.manuallyPaused
        || context.startupPending
        || active()
        || (context.tuneElapsedMs.has_value()
            && static_cast<double>(context.tuneElapsedMs.value_or(0)) < normalizedWaitForDataStreamSeconds(context.waitSeconds) * 1000.0)) {
        m_videoFreezeConsecutiveCount = 0;
        return {};
    }

    if (m_cooldownMs.has_value()
        && (nowMs - m_cooldownMs.value_or(nowMs)) < kSoftReconnectWatchdogCooldownMs) {
        m_videoFreezeConsecutiveCount = 0;
        return {};
    }

    const auto hasCacheSpeedSample = health.cacheSpeedBytesPerSecond.has_value()
        && std::isfinite(health.cacheSpeedBytesPerSecond.value())
        && health.cacheSpeedBytesPerSecond.value() >= 0.0;
    const auto normalizedCacheSpeed = hasCacheSpeedSample
        ? std::max(0.0, health.cacheSpeedBytesPerSecond.value())
        : 0.0;
    const auto hasCacheSample = health.cacheDurationSeconds.has_value()
        && std::isfinite(health.cacheDurationSeconds.value())
        && health.cacheDurationSeconds.value() >= 0.0;
    const auto normalizedCacheDuration =
        hasCacheSample ? std::max(0.0, health.cacheDurationSeconds.value()) : 0.0;
    const auto streamHealthyEnough = (hasCacheSpeedSample
                                      && normalizedCacheSpeed > kNoRefillCacheSpeedThresholdBytesPerSecond)
        || (hasCacheSample && normalizedCacheDuration > kReconnectDepletedBufferThresholdSeconds);
    if (!streamHealthyEnough) {
        m_videoFreezeConsecutiveCount = 0;
        return {};
    }

    const auto hasVideoTrack = context.hasVideo;
    if (!hasVideoTrack) {
        m_videoFreezeConsecutiveCount = 0;
        return {};
    }

    const auto frameFrozen = !framePtsAdvanced.has_value() || !framePtsAdvanced.value();
    const auto classicFreezeSignal = playbackAdvanced
        && framePtsAdvanced.has_value()
        && !framePtsAdvanced.value()
        && !context.buffering
        && !context.stalled;
    const auto decoderStallSignal = frameFrozen
        && (context.buffering || context.stalled || !playbackAdvanced);
    if (!classicFreezeSignal && !decoderStallSignal) {
        m_videoFreezeConsecutiveCount = 0;
        return {};
    }

    m_videoFreezeConsecutiveCount += 1;
    const auto threshold = decoderStallSignal ? kDecoderStallTickThreshold : kVideoFreezeTickThreshold;
    auto reason = decoderStallSignal
        ? QStringLiteral("decoder-stall-watchdog")
        : QStringLiteral("video-freeze-watchdog");
    if (playerTraceEnabled()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Video-freeze watchdog sample %1/%2 (%3): cache=%4 cache-speed=%5B/s buffering=%6 stalled=%7 playback-advanced=%8 frame-frozen=%9.")
                .arg(m_videoFreezeConsecutiveCount)
                .arg(threshold)
                .arg(reason)
                .arg(
                    hasCacheSample
                        ? QString::number(normalizedCacheDuration, 'f', 3)
                        : QStringLiteral("N/A"))
                .arg(
                    hasCacheSpeedSample
                        ? QString::number(normalizedCacheSpeed, 'f', 0)
                        : QStringLiteral("N/A"))
                .arg(context.buffering ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(context.stalled ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(playbackAdvanced ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(frameFrozen ? QStringLiteral("true") : QStringLiteral("false")));
    }
    if (m_videoFreezeConsecutiveCount >= threshold) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Video-freeze watchdog threshold reached (%1); triggering playback recovery.")
                .arg(reason));
        return reason;
    }
    return {};
}

bool PlaybackRecovery::observeRecovery(const StreamHealth &health, const RecoveryContext &context,
    bool playbackAdvanced, std::optional<bool> framePtsAdvanced, qint64 nowMs)
{
    bool restoreStrict = false;

    if (!active() || !attemptInFlight() || !context.hasChannel || context.startupPending
        || context.failed) {
        resetStabilization();
        m_reconnectRecoveryHealthyTickCount = 0;
        m_reconnectRecoveryUnhealthyTickCount = 0;
        m_reconnectStabilizationRefillTickCount = 0;
        m_lastReconnectStabilizationCacheDurationSeconds = std::nullopt;
        return restoreStrict;
    }

    if (context.catchup) {
        const auto backendHealthy = !context.buffering && !context.stalled;
        const auto frameHealthy = framePtsAdvanced.value_or(playbackAdvanced);
        const auto stableTick = backendHealthy && playbackAdvanced && frameHealthy;
        const auto unstableTick = !backendHealthy || !frameHealthy;

        if (!stabilizing()) {
            if (!stableTick) {
                m_reconnectRecoveryHealthyTickCount = 0;
                m_reconnectRecoveryUnhealthyTickCount = 0;
                m_reconnectStabilizationRefillTickCount = 0;
                return restoreStrict;
            }

            m_phase = Phase::Stabilizing;
            m_attemptStartedMs = nowMs;
            m_reconnectRecoveryHealthyTickCount = 1;
            m_reconnectRecoveryUnhealthyTickCount = 0;
            // Catch-up stabilization intentionally ignores cache-duration/cache-speed samples.
            m_reconnectStabilizationRefillTickCount = kReconnectStabilizationMinRefillTicks;
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Reconnect attempt entered catch-up stabilization phase."));
            return restoreStrict;
        }

        if (stableTick) {
            m_reconnectRecoveryHealthyTickCount += 1;
            m_reconnectRecoveryUnhealthyTickCount = 0;
        } else if (unstableTick) {
            m_reconnectRecoveryUnhealthyTickCount += 1;
            m_reconnectRecoveryHealthyTickCount = 0;
        } else {
            m_reconnectRecoveryHealthyTickCount = 0;
            m_reconnectRecoveryUnhealthyTickCount = 0;
        }

        if (playerTraceEnabled()) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up reconnect stabilization tick: stable=%1/%2 unstable=%3/%4 playback-advanced=%5 frame-healthy=%6 buffering=%7 stalled=%8.")
                    .arg(m_reconnectRecoveryHealthyTickCount)
                    .arg(kReconnectStabilizationStableTickThreshold)
                    .arg(m_reconnectRecoveryUnhealthyTickCount)
                    .arg(kReconnectStabilizationUnstableTickThreshold)
                    .arg(playbackAdvanced ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(frameHealthy ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(context.buffering ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(context.stalled ? QStringLiteral("true") : QStringLiteral("false")));
        }
        return restoreStrict;
    }

    const auto hasCacheSpeedSample = health.cacheSpeedBytesPerSecond.has_value()
        && std::isfinite(health.cacheSpeedBytesPerSecond.value())
        && health.cacheSpeedBytesPerSecond.value() >= 0.0;
    const auto normalizedCacheSpeed = hasCacheSpeedSample
        ? std::max(0.0, health.cacheSpeedBytesPerSecond.value())
        : 0.0;
    const auto hasCacheSample = health.cacheDurationSeconds.has_value()
        && std::isfinite(health.cacheDurationSeconds.value())
        && health.cacheDurationSeconds.value() >= 0.0;
    const auto normalizedCacheDuration = hasCacheSample ? std::max(0.0, health.cacheDurationSeconds.value()) : 0.0;
    const auto normalizedBufferTarget = normalizedBufferTargetSeconds(health.bufferTargetSeconds);

    bool cacheRefilling = false;
    bool cacheDraining = false;
    if (hasCacheSample) {
        if (m_lastReconnectStabilizationCacheDurationSeconds.has_value()) {
            cacheRefilling = normalizedCacheDuration
                > m_lastReconnectStabilizationCacheDurationSeconds.value() + kNoRefillCacheIncreaseEpsilonSeconds;
            cacheDraining = normalizedCacheDuration + kNoRefillCacheIncreaseEpsilonSeconds
                < m_lastReconnectStabilizationCacheDurationSeconds.value();
        }
        m_lastReconnectStabilizationCacheDurationSeconds = normalizedCacheDuration;
    } else {
        m_lastReconnectStabilizationCacheDurationSeconds = std::nullopt;
    }

    const auto backendHealthy = !context.buffering && !context.stalled;
    const auto frameHealthy = framePtsAdvanced.value_or(playbackAdvanced);
    const auto refillPositive = cacheRefilling;
    const auto cacheAtOrAboveTarget = hasCacheSample
        && normalizedCacheDuration + kNoRefillCacheIncreaseEpsilonSeconds >= normalizedBufferTarget;
    const auto cacheCriticallyLow = hasCacheSample
        && normalizedCacheDuration <= kReconnectDepletedBufferThresholdSeconds;

    if (!stabilizing()) {
        const auto candidateHealthy = backendHealthy && playbackAdvanced && frameHealthy
            && !cacheCriticallyLow && (m_reconnectReserveReady || cacheAtOrAboveTarget);
        if (!candidateHealthy) {
            m_reconnectRecoveryHealthyTickCount = 0;
            m_reconnectRecoveryUnhealthyTickCount = 0;
            m_reconnectStabilizationRefillTickCount = 0;
            return restoreStrict;
        }

        m_phase = Phase::Stabilizing;
        m_attemptStartedMs = nowMs;
        m_reconnectRecoveryHealthyTickCount = 1;
        m_reconnectRecoveryUnhealthyTickCount = 0;
        m_reconnectStabilizationRefillTickCount = 1;
        restoreStrict = true;
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Reconnect attempt entered stabilization phase: cache=%1 target=%2 cache-speed=%3B/s.")
                .arg(hasCacheSample ? QString::number(normalizedCacheDuration, 'f', 3) : QStringLiteral("N/A"))
                .arg(QString::number(normalizedBufferTarget, 'f', 3))
                .arg(
                    hasCacheSpeedSample
                        ? QString::number(normalizedCacheSpeed, 'f', 0)
                        : QStringLiteral("N/A")));
        return restoreStrict;
    }

    if (refillPositive) {
        m_reconnectStabilizationRefillTickCount += 1;
    }

    // A healthy player normally consumes cache between provider bursts.
    // Refill evidence is counted separately; draining alone is not failure.
    const auto stableTick = backendHealthy && playbackAdvanced && frameHealthy && !cacheCriticallyLow;
    const auto unstableTick = cacheCriticallyLow || !backendHealthy || !frameHealthy;

    if (stableTick) {
        m_reconnectRecoveryHealthyTickCount += 1;
        m_reconnectRecoveryUnhealthyTickCount = 0;
    } else if (unstableTick) {
        m_reconnectRecoveryUnhealthyTickCount += 1;
        m_reconnectRecoveryHealthyTickCount = 0;
    } else {
        m_reconnectRecoveryHealthyTickCount = 0;
        m_reconnectRecoveryUnhealthyTickCount = 0;
    }

    if (playerTraceEnabled()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Reconnect stabilization tick: stable=%1/%2 unstable=%3/%4 refill=%5/%6 cache=%7 target=%8 cache-speed=%9B/s draining=%10 buffering=%11 stalled=%12.")
                .arg(m_reconnectRecoveryHealthyTickCount)
                .arg(kReconnectStabilizationStableTickThreshold)
                .arg(m_reconnectRecoveryUnhealthyTickCount)
                .arg(kReconnectStabilizationUnstableTickThreshold)
                .arg(m_reconnectStabilizationRefillTickCount)
                .arg(kReconnectStabilizationMinRefillTicks)
                .arg(hasCacheSample ? QString::number(normalizedCacheDuration, 'f', 3) : QStringLiteral("N/A"))
                .arg(QString::number(normalizedBufferTarget, 'f', 3))
                .arg(
                    hasCacheSpeedSample
                        ? QString::number(normalizedCacheSpeed, 'f', 0)
                        : QStringLiteral("N/A"))
                .arg(cacheDraining ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(context.buffering ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(context.stalled ? QStringLiteral("true") : QStringLiteral("false")));
    }
    return restoreStrict;
}
void PlaybackRecovery::clearProgress()
{
    m_playbackStalled = false;
    m_lastPlaybackPositionSeconds = -1.0;
    m_stalledPlaybackTickCount = 0;
}
bool PlaybackRecovery::observePosition(double seconds, bool detectStall)
{
    if (seconds < 0.0) {
        ++m_stalledPlaybackTickCount;
        return false;
    }
    const auto advanced = m_lastPlaybackPositionSeconds < 0.0 || std::abs(seconds - m_lastPlaybackPositionSeconds) > 0.05;
    m_lastPlaybackPositionSeconds = seconds;
    if (advanced) {
        m_playbackStalled = false;
        m_stalledPlaybackTickCount = 0;
    } else {
        ++m_stalledPlaybackTickCount;
        if (detectStall && m_stalledPlaybackTickCount >= 2) m_playbackStalled = true;
    }
    return advanced;
}
void PlaybackRecovery::markPausedStall(bool manuallyPaused)
{
    m_playbackStalled = !manuallyPaused && m_stalledPlaybackTickCount >= 2;
}
int PlaybackRecovery::reconnectDepletionTimeoutMsForWaitSeconds(const double waitForDataStreamSeconds)
{
    const auto normalizedWait = normalizedWaitForDataStreamSeconds(waitForDataStreamSeconds);
    const auto waitTimeoutMs = static_cast<int>(std::lround(normalizedWait * 1000.0));
    return std::max(kReconnectPostDepletionGraceMs, waitTimeoutMs);
}
bool PlaybackRecovery::shouldStartPreemptiveReconnect(
    const std::optional<double> cacheDurationSeconds,
    const double bufferTargetSeconds,
    const std::optional<double> throughputBitsPerSecond,
    const bool playbackAdvanced,
    const bool backendBuffering)
{
    // Some platforms keep backendBuffering=true for a long time after stream loss.
    // Do not suppress reconnect if playback has stopped advancing.
    if (backendBuffering && playbackAdvanced) {
        return false;
    }

    const auto weakThroughput = !throughputBitsPerSecond.has_value()
        || !std::isfinite(throughputBitsPerSecond.value())
        || std::max(0.0, throughputBitsPerSecond.value()) <= kDeadStreamLowThroughputBitsPerSecond;

    bool lowBufferRatio = false;
    if (cacheDurationSeconds.has_value() && std::isfinite(cacheDurationSeconds.value())
        && std::isfinite(bufferTargetSeconds) && bufferTargetSeconds > 0.0) {
        const auto normalizedCacheDuration = std::max(0.0, cacheDurationSeconds.value());
        const auto normalizedBufferTarget = normalizedBufferTargetSeconds(bufferTargetSeconds);
        const auto bufferRatio = normalizedCacheDuration / normalizedBufferTarget;
        lowBufferRatio = bufferRatio <= kPreemptiveReconnectBufferRatioThreshold;
    }

    return weakThroughput && (lowBufferRatio || !playbackAdvanced);
}
bool PlaybackRecovery::deadStreamLikelyDisconnected(
    const std::optional<double> cacheDurationSeconds,
    const std::optional<double> throughputBitsPerSecond)
{
    if (!cacheDurationSeconds.has_value() || !std::isfinite(cacheDurationSeconds.value())) {
        return false;
    }

    const auto normalizedCacheDuration = std::max(0.0, cacheDurationSeconds.value());
    if (normalizedCacheDuration > kReconnectDepletedBufferThresholdSeconds) {
        return false;
    }

    if (!throughputBitsPerSecond.has_value() || !std::isfinite(throughputBitsPerSecond.value())) {
        return true;
    }

    const auto normalizedThroughput = std::max(0.0, throughputBitsPerSecond.value());
    return normalizedThroughput <= kDeadStreamLowThroughputBitsPerSecond;
}
} // namespace OKILTV::App::Playback
