#include "playbackbuffering.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace OKILTV::App::Playback {
namespace {
constexpr double kCatchupStandbyCacheRefillMarginSeconds = 5.0;
constexpr qint64 kCatchupStandbyDemuxerMaxBytes = 32LL * 1024LL * 1024LL;
constexpr qint64 kCatchupStandbyDemuxerMaxBackBytes = 8LL * 1024LL * 1024LL;
constexpr qsizetype kCatchupOwnedQueueActiveHighWaterBytes = 16LL * 1024 * 1024;
constexpr qsizetype kCatchupOwnedQueueActiveLowWaterBytes = 8LL * 1024 * 1024;
constexpr qint64 kCatchupOwnedQueueActiveReplyReadBufferBytes = 16LL * 1024 * 1024;
constexpr qsizetype kCatchupOwnedQueueStandbyHighWaterBytes = 8LL * 1024 * 1024;
constexpr qsizetype kCatchupOwnedQueueStandbyLowWaterBytes = 4LL * 1024 * 1024;
constexpr qint64 kCatchupOwnedQueueStandbyReplyReadBufferBytes = 8LL * 1024 * 1024;
constexpr double kMinimumBufferSeconds = 0.1;
constexpr double kMaximumBufferSeconds = 60.0;
constexpr qint64 kDebugBitrateWindowMs = 5000;
constexpr double kAdaptiveSteadyStateMaxBytesSafetyMultiplier = 1.25;
constexpr double kAdaptiveSteadyStateSecondsRetuneThreshold = 0.5;
constexpr double kAdaptiveSteadyStateBytesRetuneThresholdRatio = 0.15;
constexpr int kAdaptiveSteadyStateRetuneIntervalMs = 2000;
constexpr qint64 kAdaptiveSteadyStateMinBytes = 8LL * 1024 * 1024;
constexpr double kCatchupActiveCacheHeadSeconds = 90.0;
constexpr double kCatchupActiveCacheRefillMarginSeconds = 5.0;
constexpr qint64 kCatchupActiveDemuxerMaxBytes = 96LL * 1024LL * 1024LL;
constexpr qint64 kCatchupActiveDemuxerMaxBackBytes = 32LL * 1024LL * 1024LL;
constexpr double kCatchupStandbyCacheHeadSeconds = 30.0;
double normalizedBufferTargetSeconds(double value)
{
    return std::isfinite(value) ? std::clamp(value, kMinimumBufferSeconds, kMaximumBufferSeconds) : 3.0;
}
bool validCache(std::optional<double> cache)
{
    return cache.has_value() && std::isfinite(*cache) && *cache >= 0.0;
}
}
double PlaybackBuffering::adaptiveSteadyStateCacheLimitSeconds(const double bufferTargetSeconds)
{
    return Player::MpvPlayer::steadyStateCacheLimitSecondsForBufferTarget(bufferTargetSeconds);
}

double PlaybackBuffering::adaptiveSteadyStateCacheHysteresisSeconds(const double bufferTargetSeconds)
{
    return Player::MpvPlayer::steadyStateCacheHysteresisSecondsForBufferTarget(bufferTargetSeconds);
}

qint64 PlaybackBuffering::adaptiveSteadyStateMaxBytes(
    const double bufferTargetSeconds,
    const std::optional<double> averageBitsPerSecond)
{
    const auto totalLiveWindowSeconds =
        adaptiveSteadyStateCacheLimitSeconds(bufferTargetSeconds) + Player::MpvPlayer::steadyStateBackBufferSeconds();
    if (!averageBitsPerSecond.has_value()
        || !std::isfinite(averageBitsPerSecond.value())
        || averageBitsPerSecond.value() <= 0.0) {
        return Player::MpvPlayer::demuxerMaxBytesForBufferSeconds(totalLiveWindowSeconds);
    }

    const auto dynamicBudgetBytes = static_cast<qint64>(std::llround(
        (averageBitsPerSecond.value() / 8.0) * totalLiveWindowSeconds * kAdaptiveSteadyStateMaxBytesSafetyMultiplier));
    const auto maxBudgetBytes = std::numeric_limits<qint64>::max();
    return std::clamp(dynamicBudgetBytes, kAdaptiveSteadyStateMinBytes, maxBudgetBytes);
}

qint64 PlaybackBuffering::adaptiveSteadyStateMaxBackBytes(const std::optional<double> averageBitsPerSecond)
{
    const auto backBufferSeconds = Player::MpvPlayer::steadyStateBackBufferSeconds();
    if (!averageBitsPerSecond.has_value()
        || !std::isfinite(averageBitsPerSecond.value())
        || averageBitsPerSecond.value() <= 0.0) {
        return Player::MpvPlayer::demuxerMaxBytesForBufferSeconds(backBufferSeconds);
    }

    const auto dynamicBudgetBytes = static_cast<qint64>(std::llround(
        (averageBitsPerSecond.value() / 8.0) * backBufferSeconds * kAdaptiveSteadyStateMaxBytesSafetyMultiplier));
    return std::clamp(dynamicBudgetBytes, kAdaptiveSteadyStateMinBytes, std::numeric_limits<qint64>::max());
}

qint64 PlaybackBuffering::adaptiveCatchupMaxBytes(const std::optional<double> averageBitsPerSecond)
{
    if (!averageBitsPerSecond.has_value()
        || !std::isfinite(averageBitsPerSecond.value())
        || averageBitsPerSecond.value() <= 0.0) {
        return kCatchupActiveDemuxerMaxBytes;
    }

    const auto dynamicBudgetBytes =
        (averageBitsPerSecond.value() / 8.0) * kCatchupActiveCacheHeadSeconds * kAdaptiveSteadyStateMaxBytesSafetyMultiplier;
    return static_cast<qint64>(std::llround(std::clamp(
        dynamicBudgetBytes,
        static_cast<double>(kAdaptiveSteadyStateMinBytes),
        static_cast<double>(kCatchupActiveDemuxerMaxBytes))));
}

qint64 PlaybackBuffering::adaptiveCatchupMaxBackBytes(const std::optional<double> averageBitsPerSecond)
{
    if (!averageBitsPerSecond.has_value()
        || !std::isfinite(averageBitsPerSecond.value())
        || averageBitsPerSecond.value() <= 0.0) {
        return kCatchupActiveDemuxerMaxBackBytes;
    }

    const auto dynamicBudgetBytes =
        (averageBitsPerSecond.value() / 8.0) * kCatchupStandbyCacheHeadSeconds * kAdaptiveSteadyStateMaxBytesSafetyMultiplier;
    return static_cast<qint64>(std::llround(std::clamp(
        dynamicBudgetBytes,
        static_cast<double>(kAdaptiveSteadyStateMinBytes),
        static_cast<double>(kCatchupActiveDemuxerMaxBackBytes))));
}

// Inputs retain distinct documented units (seconds, segment count or monotonic milliseconds).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int PlaybackBuffering::startupBufferFallbackTimeoutMs(const double bufferTargetSeconds, const int segmentSecondsHint)
{
    const auto normalized = normalizedBufferTargetSeconds(bufferTargetSeconds);
    const auto baseTimeoutMs = static_cast<int>(std::lround(normalized * 1000.0));
    if (segmentSecondsHint <= 0) {
        return baseTimeoutMs;
    }

    const auto normalizedSegmentSeconds = std::clamp(segmentSecondsHint, 2, 60);
    const auto segmentSafetyMs = static_cast<int>(std::lround((static_cast<double>(normalizedSegmentSeconds) + 1.0) * 1000.0));
    constexpr int kStartupFallbackTimeoutMaxMs = 180000;
    return std::min(kStartupFallbackTimeoutMaxMs, baseTimeoutMs + segmentSafetyMs);
}

PlaybackBuffering::StartupPolicy PlaybackBuffering::startupPolicy(bool catchup, bool ownedPrimary,
    bool shared, bool reconnect, bool softwareFallback, bool timeshift)
{
    if (catchup) return StartupPolicy::BestEffort;
    if (!ownedPrimary || shared || reconnect || softwareFallback || timeshift) return StartupPolicy::StrictBuffered;
    return StartupPolicy::FastLive;
}

void PlaybackBuffering::resetAverage()
{
    m_samples.clear();
    m_average.reset();
}
std::optional<double> PlaybackBuffering::observeBitrate(std::optional<double> bitrate, qint64 nowMs)
{
    while (!m_samples.empty() && m_samples.front().first < nowMs - kDebugBitrateWindowMs) m_samples.pop_front();
    if (!bitrate.has_value() || !std::isfinite(*bitrate) || *bitrate < 0.0) {
        resetAverage();
        return std::nullopt;
    }
    m_samples.emplace_back(nowMs, *bitrate);
    double total = 0.0;
    for (const auto &sample : m_samples) total += sample.second;
    m_average = total / static_cast<double>(m_samples.size());
    return m_average;
}
void PlaybackBuffering::resetAdaptation()
{
    resetLiveReserve(false, false);
    m_tuner.reset();
    m_lastPolicy.reset();
}
void PlaybackBuffering::observeDelivery(double nowSeconds, double endSeconds, bool idle)
{
    m_tuner.observe(nowSeconds, endSeconds, idle);
}
std::optional<Player::MpvPlayer::SteadyStateBufferingPolicy> PlaybackBuffering::retune(
    bool catchup, double configured, std::optional<double> cache, bool fastLive, qint64 nowMs)
{
    const auto target = targetSeconds(configured);
    Player::MpvPlayer::SteadyStateBufferingPolicy next {
        .cacheLimitSeconds = catchup ? kCatchupActiveCacheHeadSeconds : adaptiveSteadyStateCacheLimitSeconds(target),
        .hysteresisSeconds = catchup ? kCatchupActiveCacheHeadSeconds - kCatchupActiveCacheRefillMarginSeconds
                                    : adaptiveSteadyStateCacheHysteresisSeconds(target),
        .maxBytes = catchup ? adaptiveCatchupMaxBytes(m_average) : adaptiveSteadyStateMaxBytes(target, m_average),
        .maxBackBytes = catchup ? adaptiveCatchupMaxBackBytes(m_average) : adaptiveSteadyStateMaxBackBytes(m_average),
        .refillSeconds = !catchup && fastLive ? std::optional<double>(0.0) : std::nullopt,
    };
    if (m_lastPolicy) {
        const auto &old = *m_lastPolicy;
        const auto bytesChanged = [](qint64 before, qint64 after) {
            return before <= 0 || std::abs(static_cast<double>(after - before)) >=
                static_cast<double>(std::max<qint64>(1, std::llround(std::abs(static_cast<double>(before))
                    * kAdaptiveSteadyStateBytesRetuneThresholdRatio)));
        };
        const auto changed = std::abs(next.cacheLimitSeconds - old.cacheLimitSeconds) >= kAdaptiveSteadyStateSecondsRetuneThreshold
            || std::abs(next.hysteresisSeconds - old.hysteresisSeconds) >= kAdaptiveSteadyStateSecondsRetuneThreshold
            || bytesChanged(old.maxBytes, next.maxBytes) || bytesChanged(old.maxBackBytes, next.maxBackBytes);
        if (!changed) return std::nullopt;
        const auto below = cache.has_value() && std::isfinite(*cache)
            && std::max(0.0, *cache) + 0.05 < next.hysteresisSeconds;
        if (nowMs - m_lastRetuneMs < kAdaptiveSteadyStateRetuneIntervalMs && !below) return std::nullopt;
    }
    m_lastPolicy = next;
    m_lastRetuneMs = nowMs;
    return next;
}
Player::MpvPlayer::SteadyStateBufferingPolicy PlaybackBuffering::activeCatchupPolicy(qint64 nowMs)
{
    Player::MpvPlayer::SteadyStateBufferingPolicy policy {
        .cacheLimitSeconds = kCatchupActiveCacheHeadSeconds,
        .hysteresisSeconds = kCatchupActiveCacheHeadSeconds - kCatchupActiveCacheRefillMarginSeconds,
        .maxBytes = adaptiveCatchupMaxBytes(m_average),
        .maxBackBytes = adaptiveCatchupMaxBackBytes(m_average),
    };
    m_lastPolicy = policy;
    m_lastRetuneMs = nowMs;
    return policy;
}
ReserveDecision PlaybackBuffering::resetLiveReserve(bool resume, bool manuallyPaused, bool preserveDelivery)
{
    const auto pending = liveReservePending();
    m_livePhase = ReservePhase::Idle;
    m_liveTarget = 0.0;
    m_liveRetryMs.reset();
    if (!preserveDelivery) interruptDelivery();
    return {pending && resume && !manuallyPaused ? PauseCommand::Resume : PauseCommand::None, false};
}
// Inputs retain distinct documented units (seconds, segment count or monotonic milliseconds).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ReserveDecision PlaybackBuffering::beginLiveReserve(std::optional<double> cache, bool readerIdle, double configured, qint64 nowMs)
{
    if (liveReservePending() || readerIdle || !validCache(cache) || cache.value_or(0.0) > 0.1
        || (m_liveRetryMs && nowMs - *m_liveRetryMs < 10000)) return {PauseCommand::None, liveReservePending()};
    m_liveTarget = std::max(3.0, targetSeconds(configured));
    m_livePhase = ReservePhase::Filling;
    m_liveStartedMs = nowMs;
    return {PauseCommand::Pause, true};
}
void PlaybackBuffering::raiseLiveTarget(double configured)
{
    m_liveTarget = std::max(m_liveTarget, targetSeconds(configured));
}
// Inputs retain distinct documented units (seconds, segment count or monotonic milliseconds).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ReserveDecision PlaybackBuffering::advanceLiveReserve(std::optional<double> cache, bool readerIdle, bool eof,
// Preserve the established positional contract; parameter names identify their roles.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                                     qint64 elapsedMs, qint64 nowMs)
{
    if (!liveReservePending()) return {};
    const auto ready = validCache(cache) && cache.value_or(0.0) >= m_liveTarget;
    const auto capacity = elapsedMs >= 1000 && readerIdle && validCache(cache) && cache.value_or(0.0) > 1.0;
    const auto timeout = elapsedMs >= static_cast<qint64>(1000.0 * std::clamp(m_liveTarget * 2.0 + 2.0, 10.0, 30.0));
    if (!ready && !capacity && !timeout && !eof) return {PauseCommand::None, true};
    const auto result = resetLiveReserve(true, false, ready);
    if (!ready) m_liveRetryMs = nowMs;
    return result;
}
ReserveDecision PlaybackBuffering::resetCatchupRefill(bool resume, bool manuallyPaused)
{
    const auto pending = catchupRefilling();
    m_catchupPhase = ReservePhase::Idle;
    return {pending && resume && !manuallyPaused ? PauseCommand::Resume : PauseCommand::None, false};
}
ReserveDecision PlaybackBuffering::catchupRefill(std::optional<double> cache, bool healthySession, bool eof,
                                               bool manuallyPaused, bool canStart, qint64 nowMs)
{
    if (!healthySession || eof || manuallyPaused) return resetCatchupRefill(!manuallyPaused, manuallyPaused);
    if (catchupRefilling()) {
        if (validCache(cache) && (cache.value_or(0.0) >= 10.0 || (cache.value_or(0.0) > 1.0 && nowMs - m_catchupStartedMs >= 30000)))
            return resetCatchupRefill(true, manuallyPaused);
    } else if (canStart && validCache(cache) && cache.value_or(0.0) <= 0.1) {
        m_catchupPhase = ReservePhase::Filling;
        m_catchupStartedMs = nowMs;
        return {PauseCommand::Pause, true};
    }
    return {PauseCommand::None, catchupRefilling()};
}
PlaybackBuffering::PlaybackBuffering()
{
    m_startupFallbackTimer.setSingleShot(true);
    m_startupProbeTimer.setInterval(100);
}
// Inputs retain distinct documented units (seconds, segment count or monotonic milliseconds).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void PlaybackBuffering::armStartup(double target, int segmentHint)
{
    m_fallbackApplied = false;
    m_startupTarget = normalizedBufferTargetSeconds(target);
    m_startupFallbackTimer.start(startupBufferFallbackTimeoutMs(m_startupTarget, segmentHint));
}
void PlaybackBuffering::resetStartupWatchdog(bool resetTuneState)
{
    m_startupFallbackTimer.stop();
    if (resetTuneState) {
        m_fallbackApplied = false;
        m_startupTarget = 0.0;
    }
}
bool PlaybackBuffering::startupReady(std::optional<double> cache) const
{
    return cache && std::isfinite(*cache) && std::max(0.0, *cache) >= m_startupTarget;
}
Player::CatchupStreamSession::BufferingPolicy PlaybackBuffering::catchupOwnedStreamPolicy(const bool standby)
{
    if (standby) {
        return {
            .queueHighWaterBytes = kCatchupOwnedQueueStandbyHighWaterBytes,
            .queueLowWaterBytes = kCatchupOwnedQueueStandbyLowWaterBytes,
            .replyReadBufferBytes = kCatchupOwnedQueueStandbyReplyReadBufferBytes,
            .roleLabel = QStringLiteral("standby"),
        };
    }

    return {
        .queueHighWaterBytes = kCatchupOwnedQueueActiveHighWaterBytes,
        .queueLowWaterBytes = kCatchupOwnedQueueActiveLowWaterBytes,
        .replyReadBufferBytes = kCatchupOwnedQueueActiveReplyReadBufferBytes,
        .roleLabel = QStringLiteral("active"),
    };
}

QString PlaybackBuffering::catchupBufferOptions(bool standby)
{
    const auto cacheTargetSeconds = standby ? kCatchupStandbyCacheHeadSeconds : kCatchupActiveCacheHeadSeconds;
    const auto cacheRefillFloorSeconds = cacheTargetSeconds
        - (standby ? kCatchupStandbyCacheRefillMarginSeconds : kCatchupActiveCacheRefillMarginSeconds);
    const auto demuxerMaxBytes = standby ? kCatchupStandbyDemuxerMaxBytes : kCatchupActiveDemuxerMaxBytes;
    const auto demuxerMaxBackBytes = standby ? kCatchupStandbyDemuxerMaxBackBytes : kCatchupActiveDemuxerMaxBackBytes;
    const auto sharedOptions = QStringLiteral(
        "force-seekable=yes,hr-seek=no,cache=yes,cache-pause=no,cache-pause-wait=0,cache-secs=%1,demuxer-readahead-secs=%2,demuxer-hysteresis-secs=%3,demuxer-seekable-cache=yes,demuxer-max-bytes=%4,demuxer-max-back-bytes=%5")
                                   .arg(cacheTargetSeconds, 0, 'f', 0)
                                   .arg(cacheTargetSeconds, 0, 'f', 0)
                                   .arg(cacheRefillFloorSeconds, 0, 'f', 0)
                                   .arg(demuxerMaxBytes)
                                   .arg(demuxerMaxBackBytes);
    return sharedOptions;
}
} // namespace OKILTV::App::Playback
