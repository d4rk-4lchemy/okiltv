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

MpvPlayer::PlaybackBufferSnapshot MpvPlayer::playbackBufferForCacheState(
    const double positionSeconds, const QVariantMap &state)
{
    PlaybackBufferSnapshot result;
    const auto number = [](const QVariantMap &map, const QString &key) -> std::optional<double> {
        bool ok = false;
        const auto value = map.value(key).toDouble(&ok);
        return ok && std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
    };
    const auto duration = number(state, QStringLiteral("cache-duration"));
    const auto reader = number(state, QStringLiteral("reader-pts"));
    auto end = number(state, QStringLiteral("cache-end"));
    const auto rate = number(state, QStringLiteral("raw-input-rate"));
    if (rate && *rate >= 0.0) result.inputBytesPerSecond = rate;
    if (state.value(QStringLiteral("seeking")).toBool()) return result;

    // Older backends may expose only duration. Preserve it as an explicit estimate.
    if (!end || !reader) {
        if ((state.contains(QStringLiteral("cache-end")) && !end)
            || (state.contains(QStringLiteral("reader-pts")) && !reader)) return result;
        if (duration && *duration >= 0.0) result.seconds = duration;
        return result;
    }
    if (!std::isfinite(positionSeconds) || positionSeconds < 0.0 || *reader > *end
        || !duration || *duration < 0.0 || std::abs((*end - *reader) - *duration) > 0.25) {
        return result;
    }
    // No unread demux packets: do not manufacture reserve from old cache bounds.
    if (*duration == 0.0) {
        result.seconds = 0.0;
        result.estimated = false;
        return result;
    }

    // mpv rebases the aggregate clocks and seekable ranges, but some versions
    // leave ts-per-stream clocks in the demuxer's original timestamp epoch.
    // Match the aggregate duration to its stream in this same atomic cache state
    // to derive the translation. Never infer it from the playback/reader gap.
    constexpr double clockEpsilon = 0.000001;
    std::optional<double> streamOffset;
    bool ambiguousOffset = false;
    std::optional<double> trackEnd;
    for (const auto &entry : state.value(QStringLiteral("ts-per-stream")).toList()) {
        const auto stream = entry.toMap();
        const auto type = stream.value(QStringLiteral("type")).toString();
        const auto av = type == QLatin1String("audio") || type == QLatin1String("video");
        if (!av && type != QLatin1String("subtitle")) continue;
        const auto streamDuration = number(stream, QStringLiteral("cache-duration"));
        if (!streamDuration || *streamDuration < 0.0) continue; // Unselected/absent stream.
        const auto streamEnd = number(stream, QStringLiteral("cache-end"));
        const auto streamReader = number(stream, QStringLiteral("reader-pts"));
        if (!streamEnd || !streamReader || *streamReader > *streamEnd
            || std::abs((*streamEnd - *streamReader) - *streamDuration) > 0.25) {
            if (av) return result;
            continue;
        }
        if (std::abs(*streamDuration - *duration) <= clockEpsilon) {
            const auto offset = *reader - *streamReader;
            if (std::abs((*end - *streamEnd) - offset) <= clockEpsilon) {
                if (streamOffset && std::abs(*streamOffset - offset) > clockEpsilon) ambiguousOffset = true;
                else streamOffset = offset;
            }
        }
        if (av) trackEnd = trackEnd ? std::min(*trackEnd, *streamEnd) : *streamEnd;
    }
    const auto alignedTracks = trackEnd && streamOffset && !ambiguousOffset;
    if (alignedTracks) end = *trackEnd + *streamOffset;
    if (*end < positionSeconds) return result;

    QList<std::pair<double, double>> ranges;
    for (const auto &entry : state.value(QStringLiteral("seekable-ranges")).toList()) {
        const auto range = entry.toMap();
        const auto start = number(range, QStringLiteral("start"));
        const auto stop = number(range, QStringLiteral("end"));
        if (start && stop && *start <= *stop) ranges.append({*start, *stop});
    }
    std::sort(ranges.begin(), ranges.end());
    QList<std::pair<double, double>> continuousRanges;
    for (const auto &range : ranges) {
        if (!continuousRanges.isEmpty() && range.first <= continuousRanges.last().second) {
            continuousRanges.last().second = std::max(continuousRanges.last().second, range.second);
        } else continuousRanges.append(range);
    }
    if (!continuousRanges.isEmpty()) {
        bool contained = false;
        for (qsizetype i = 0; i < continuousRanges.size(); ++i) {
            const auto &range = continuousRanges[i];
            // The final seekable range excludes its last GOP; the packet clock
            // still describes that playable tail. Never include a later disjoint range.
            const auto rangeEnd = i + 1 == continuousRanges.size() ? *end : std::min(*end, range.second);
            if (positionSeconds >= range.first && positionSeconds <= rangeEnd) {
                end = rangeEnd;
                contained = true;
                break;
            }
        }
        if (!contained) return result;
    }
    result.seconds = *end - positionSeconds;
    result.endSeconds = end;
    result.estimated = !alignedTracks;
    return result;
}

} // namespace OKILTV::Player
