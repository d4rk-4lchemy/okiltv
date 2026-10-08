#include "vodmodels.h"
#include <algorithm>
#include <climits>
#include <QDataStream>
#include <QIODevice>

namespace OKILTV::Vod {
QString Error::message() const
{
    switch (code) {
    case ErrorCode::Cancelled: return QStringLiteral("Operation cancelled.");
    case ErrorCode::Timeout: return QStringLiteral("Operation timed out.");
    case ErrorCode::Unauthorized: return QStringLiteral("Source authentication required.");
    case ErrorCode::Forbidden: return QStringLiteral("Source access denied.");
    case ErrorCode::RateLimited: return QStringLiteral("Source request limit reached.");
    case ErrorCode::ProviderUnavailable: return QStringLiteral("Source unavailable.");
    case ErrorCode::UnsupportedCapability: return QStringLiteral("VOD capability unavailable.");
    case ErrorCode::InvalidResponse: return QStringLiteral("Invalid source response.");
    case ErrorCode::ResponseTooLarge: return QStringLiteral("Source response exceeds the limit.");
    case ErrorCode::ContentUnavailable: return QStringLiteral("Content unavailable.");
    case ErrorCode::StorageUnavailable: return QStringLiteral("VOD storage unavailable.");
    case ErrorCode::StorageBusy: return QStringLiteral("VOD database is busy. Try again.");
    case ErrorCode::SecretUnavailable: return QStringLiteral("Source secrets unavailable.");
    case ErrorCode::PlaybackConflict: return QStringLiteral("Playback resources are in use.");
    case ErrorCode::PlaybackFailed: return QStringLiteral("VOD playback failed.");
    }
    return QStringLiteral("VOD operation failed.");
}
bool ContentRef::valid() const
{
    return (kind == ContentKind::Movie || kind == ContentKind::Series || kind == ContentKind::Episode)
        && !profileId.isNull() && !catalogNamespace.isNull() && !providerItemId.isEmpty()
        && (!parentNamespace || !parentNamespace->isEmpty());
}
bool ContentRef::playable() const { return valid() && kind != ContentKind::Series; }
QByteArray ContentRef::key() const
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << quint8(1) << profileId << catalogNamespace << qint32(kind)
           << providerItemId << parentNamespace.has_value();
    if (const auto parent = parentNamespace) stream << *parent;
    return bytes;
}
std::optional<Error> RequestContext::interruption() const
{
    if (policyCurrent && !policyCurrent()) return Error{ErrorCode::Cancelled, operationId};
    if (cancelled->load()) return Error{ErrorCode::Cancelled, operationId};
    if (deadline.hasExpired()) return Error{ErrorCode::Timeout, operationId};
    return std::nullopt;
}
}

namespace OKILTV::Vod {
ContentRef parentSeries(const ContentRef &ref) {
    if (ref.kind == ContentKind::Series) return ref;
    if (ref.kind != ContentKind::Episode || !ref.parentNamespace) return {};
    return {ref.profileId, ref.catalogNamespace, ContentKind::Series, *ref.parentNamespace, {}};
}
bool specialEpisode(const VodDetails &details, const EpisodeSummary &episode) {
    for (const auto &season : details.seasons)
        if (episode.seasonId == std::optional<QString>(season.id)) return season.number == std::optional<int>(0);
    return episode.seasonId == std::optional<QString>(QStringLiteral("0"));
}
bool seriesWatched(const VodDetails &details, const SeriesProgress &history) {
    bool hasOrdinary = false;
    for (const auto &episode : details.episodes) {
        if (specialEpisode(details, episode)) continue;
        hasOrdinary = true;
        const auto progress = history.episodes.value(episode.ref.key());
        if (progress.status != WatchStatus::Watched && !watchedByPosition(progress.positionMs, progress.durationMs)) return false;
    }
    return hasOrdinary;
}
QList<EpisodeSummary> orderedEpisodes(const VodDetails &details, bool includeSpecials) {
    QList<EpisodeSummary> result;
    for (const auto &episode : details.episodes)
        if (episode.availability != Availability::Unavailable && (includeSpecials || !specialEpisode(details, episode))) result.append(episode);
    const auto seasonOrder = [&](const EpisodeSummary &episode) {
        for (const auto &season : details.seasons) if (episode.seasonId == std::optional<QString>(season.id))
            return std::pair<int,int>{season.number.value_or(INT_MAX), season.order};
        return std::pair<int,int>{INT_MAX, INT_MAX};
    };
    std::stable_sort(result.begin(), result.end(), [&](const auto &a, const auto &b) {
        if (seasonOrder(a) != seasonOrder(b)) return seasonOrder(a) < seasonOrder(b);
        if (a.number != b.number) return a.number.value_or(INT_MAX) < b.number.value_or(INT_MAX);
        return a.order < b.order;
    });
    return result;
}
std::optional<EpisodeSummary> adjacentEpisode(const VodDetails &details, const ContentRef &ref, int direction) {
    const auto episodes = orderedEpisodes(details);
    for (qsizetype i = 0; i < episodes.size(); ++i) if (episodes[i].ref == ref) {
        const auto target = i + direction;
        if (target >= 0 && target < episodes.size()) return episodes[target];
        return {};
    }
    return {};
}
std::optional<EpisodeSummary> continueEpisode(const VodDetails &details, const SeriesProgress &progress) {
    if (progress.continuationHidden) return {};
    if (progress.lastEpisode) {
        for (const auto &episode : orderedEpisodes(details, true)) if (episode.ref == *progress.lastEpisode) {
            const auto saved = progress.episodes.value(episode.ref.key());
            if (saved.status != WatchStatus::Watched && !watchedByPosition(saved.positionMs, saved.durationMs)) return episode;
            return adjacentEpisode(details, episode.ref, 1);
        }
        return {};
    }
    const auto episodes = orderedEpisodes(details);
    return episodes.isEmpty() ? std::nullopt : std::optional<EpisodeSummary>(episodes.first());
}
}
