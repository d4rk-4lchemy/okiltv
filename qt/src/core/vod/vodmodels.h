#pragma once

#include "voderrors.h"
#include <QDateTime>
#include <QDeadlineTimer>
#include <QList>
#include <QJsonObject>
#include <QMap>
#include <QHash>
#include <QUrl>
#include <atomic>
#include <memory>

namespace OKILTV::Vod {
enum class ContentKind { Movie, Series, Episode };
enum class CatalogKind { Movies, Series };
enum class Availability { Unknown, Available, Unavailable };
enum class Capability { Unknown, Supported, Unsupported, TemporarilyUnavailable };
enum class Authorization { Unknown, Authorized, Unauthorized, Forbidden };
struct ProviderCapabilities {
    Capability movies = Capability::Unknown;
    Capability series = Capability::Unknown;
    Capability details = Capability::Unknown;
    Capability categoryFilter = Capability::Unknown;
    Capability paging = Capability::Unknown;
    Authorization authorization = Authorization::Unknown;
    std::optional<QDateTime> recheckAtUtc;
};
struct ContentRef {
    QUuid profileId;
    QUuid catalogNamespace;
    ContentKind kind = ContentKind::Movie;
    QString providerItemId;
    std::optional<QString> parentNamespace;
    bool operator==(const ContentRef &) const = default;
    [[nodiscard]] bool valid() const;
    [[nodiscard]] bool playable() const;
    [[nodiscard]] QByteArray key() const;
};
struct SourceRevision {
    QUuid profileId;
    QUuid catalogNamespace;
    quint64 credentialRevision = 1;
    bool operator==(const SourceRevision &) const = default;
};
// A source's namespace survives ALL endpoint/login/password edits. Only explicit
// source deletion and creation establishes another identity (never a URL hash).
struct SourceContext {
    SourceRevision revision;
    QString provider;
    bool enabled = false;
    QUrl endpoint;
    QString username;
    QString password;
};
struct RequestContext {
    QUuid operationId = QUuid::createUuid();
    SourceRevision source;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    QDeadlineTimer deadline{30000};
    qint64 responseByteLimit = qint64(64) * 1024 * 1024;
    [[nodiscard]] std::optional<Error> interruption() const;
};
struct CatalogScope {
    QUuid profileId;
    QUuid catalogNamespace;
    CatalogKind kind = CatalogKind::Movies;
    std::optional<QString> categoryId;
    bool operator==(const CatalogScope &) const = default;
};
struct ArtworkRef {
    QString id;
    QString role;
    std::optional<QString> cachedPath;
};
struct VodCategory {
    CatalogScope scope;
    QString id;
    QString name;
    std::optional<QString> parentId;
};
// A complete collection, including an explicitly empty provider response.
// Timestamp remains visible so callers can present stale cache while refreshing.
struct CategorySnapshot {
    CatalogScope scope;
    QList<VodCategory> categories;
    QDateTime refreshedAtUtc;
};
struct MovieSummary {
    ContentRef ref;
    QString title;
    std::optional<int> year;
    QList<ArtworkRef> artwork;
    QStringList categoryIds;
    Availability availability = Availability::Unknown;
};
struct SeriesSummary {
    ContentRef ref;
    QString title;
    std::optional<int> year;
    QList<ArtworkRef> artwork;
    QStringList categoryIds;
    Availability availability = Availability::Unknown;
};
struct Season {
    ContentRef series;
    QString id;
    std::optional<int> number;
    int order = 0;
};
struct EpisodeSummary {
    ContentRef ref;
    ContentRef series;
    std::optional<QString> seasonId;
    std::optional<int> number;
    int order = 0;
    QString title;
    Availability availability = Availability::Unknown;
};
using CatalogItem = std::variant<MovieSummary, SeriesSummary>;
struct VodDetails {
    ContentRef ref;
    QString description;
    QStringList cast;
    QStringList genres;
    std::optional<qint64> declaredDurationMs;
    std::optional<int> declaredVideoWidth;
    std::optional<int> declaredVideoHeight;
    QList<ArtworkRef> artwork;
    QList<Season> seasons;
    QList<EpisodeSummary> episodes;
    struct MediaTrack {
        int streamIndex = -1;
        int ordinal = 0;
        QString type;
        QString codec;
        QString title;
        QString language;
        bool isDefault = false;
        bool forced = false;
    };
    struct MediaProbe {
        std::optional<int> videoWidth;
        std::optional<int> videoHeight;
        QList<MediaTrack> audioTracks;
        QList<MediaTrack> subtitleTracks;
        QDateTime observedAtUtc;
    };
    std::optional<MediaProbe> mediaProbe;
};
using VodMediaTrack = VodDetails::MediaTrack;
using VodMediaProbe = VodDetails::MediaProbe;
struct ProviderCursor { QString value; };
struct CatalogBatch {
    CatalogScope scope;
    QList<CatalogItem> items;
    bool complete = false;
    std::optional<ProviderCursor> next;
};
struct LocalPageToken {
    quint64 generation = 0;
    QString lastSortKey;
    QByteArray lastIdentity;
};
enum class CatalogSort { TitleAscending, TitleDescending };
enum class MovieList { None, ToWatch, Favourites };
struct MovieListState {
    bool toWatch = false;
    bool favourite = false;
    bool operator==(const MovieListState &) const = default;
};
struct CatalogQuery {
    CatalogScope scope;
    QString titlePrefix;
    CatalogSort sort = CatalogSort::TitleAscending;
    int pageSize = 100;
    std::optional<LocalPageToken> page;
    QString titleContains;
    bool continueWatchingOnly = false;
    MovieList movieList = MovieList::None;
};
struct CatalogPage {
    CatalogScope scope;
    quint64 generation = 0;
    QList<CatalogItem> items;
    std::optional<LocalPageToken> next;
    QDateTime refreshedAtUtc;
    QHash<QByteArray, MovieListState> movieLists;
};
struct ImportToken {
    QUuid id;
    CatalogScope scope;
    SourceRevision source;
    RequestContext request;
};
struct PlaybackPreferences {
    std::optional<QString> audioLanguage;
    std::optional<QString> subtitleLanguage;
    // Complete session override selected before playback. Persistence still
    // happens only after the backend has loaded and matched the real tracks.
    std::optional<QJsonObject> trackPreferences;
    bool fromBeginning = false;
};
// Trusted, ephemeral values: never part of a facade event or persisted record.
struct PlaybackDescriptor {
    ContentRef ref;
    SourceRevision source;
    QUrl mediaUri;
    QMap<QByteArray, QByteArray> allowedHeaders;
    QDateTime expiresAtUtc;
    std::optional<QString> contentRevision;
};
enum class WatchStatus { InProgress, Watched };
inline bool watchedByPosition(qint64 position, std::optional<qint64> duration)
{
    // Strictly less than 5% remains; exactly 95% is still in progress.
    return duration && *duration > 0 && static_cast<long double>(position) > static_cast<long double>(*duration) * 0.95L;
}
struct VodProgress {
    qint64 positionMs = 0;
    std::optional<qint64> durationMs;
    WatchStatus status = WatchStatus::InProgress;
    std::optional<QString> contentRevision;
    QUuid sessionToken;
    quint64 sequence = 0;
    QDateTime updatedAtUtc;
    QJsonObject trackPreferences; // Confirmed explicit audio/sub choices, including subtitles off.
};
}
