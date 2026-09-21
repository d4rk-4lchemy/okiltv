#include "mpvplayer.h"
#include "../core/models.h"
#include <algorithm>
#include <cmath>
namespace OKILTV::Player {
namespace {
constexpr qint64 kMiB = 1024LL * 1024LL;
constexpr qint64 kDemuxerMaxBytesFloor = 8 * kMiB;
constexpr qint64 kDemuxerMaxBytesCeil = 8LL * 1024 * kMiB;
constexpr double kDemuxerBytesPerSecond = 2.0 * static_cast<double>(kMiB);
constexpr double kSteadyStateBackBufferSeconds = 30.0;
}
qint64 MpvPlayer::demuxerMaxBytesForBufferSeconds(const double bufferSeconds)
{
    const auto normalizedBuffer = Core::normalizePlayerBufferSeconds(bufferSeconds);
    const auto rawBytes = static_cast<qint64>(std::llround(normalizedBuffer * kDemuxerBytesPerSecond));
    return std::clamp(rawBytes, kDemuxerMaxBytesFloor, kDemuxerMaxBytesCeil);
}

double MpvPlayer::cacheWindowSecondsForBufferTarget(const double bufferTargetSeconds)
{
    const auto normalizedTarget = Core::normalizePlayerBufferSeconds(bufferTargetSeconds);
    return std::clamp(std::max(normalizedTarget * 3.0, normalizedTarget + 8.0), 10.0, 120.0);
}

double MpvPlayer::steadyStateBackBufferSeconds()
{
    return kSteadyStateBackBufferSeconds;
}

double MpvPlayer::steadyStateCacheLimitSecondsForBufferTarget(const double bufferTargetSeconds)
{
    // The playback reserve is not a download ceiling. Accept provider bursts
    // beyond it, while keeping read-ahead bounded by time and byte budgets.
    return cacheWindowSecondsForBufferTarget(bufferTargetSeconds);
}

double MpvPlayer::steadyStateCacheHysteresisSecondsForBufferTarget(const double /*bufferTargetSeconds*/)
{
    // Zero disables mpv's refill hysteresis: read whenever cache space opens.
    return 0.0;
}

} // namespace OKILTV::Player
