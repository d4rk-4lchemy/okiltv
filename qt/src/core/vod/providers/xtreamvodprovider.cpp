#include "xtreamvodprovider.h"
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>
#include <QSet>
#include <QUrlQuery>
#include <algorithm>

namespace OKILTV::Vod {
namespace {
QString identifier(const QJsonValue &value)
{
    if (value.isString()) {
        const auto id = value.toString();
        if (id.size() <= 256 && !id.isEmpty()
            && std::none_of(id.cbegin(), id.cend(), [](QChar c) { return c.isSpace() || c.category() == QChar::Other_Control; })) return id;
    } else if (value.isDouble()) {
        const auto id = value.toInteger(-1);
        if (id >= 0 && value == QJsonValue(id)) return QString::number(id);
    }
    return {};
}
QString safeText(const QJsonValue &value, const SourceContext &source)
{
    auto text = value.toString();
    if (text.size() > 16384) text.truncate(16384);
    static const QRegularExpression urls(QStringLiteral(R"((?i)https?://[^\s<>]+)"));
    text.replace(urls, QStringLiteral("[redacted URL]"));
    for (const auto &secret : {source.username, source.password})
        if (!secret.isEmpty()) text.replace(secret, QStringLiteral("[redacted]"));
    text.remove(QChar::Null);
    return text;
}
bool matches(const SourceContext &source, const CatalogScope &scope)
{ return scope.profileId == source.revision.profileId && scope.catalogNamespace == source.revision.catalogNamespace; }
bool matches(const SourceContext &source, const ContentRef &ref)
{ return ref.valid() && ref.profileId == source.revision.profileId && ref.catalogNamespace == source.revision.catalogNamespace; }
std::optional<int> number(const QJsonValue &value)
{
    bool ok = false;
    const int parsed = identifier(value).toInt(&ok);
    return ok && parsed >= 0 ? std::optional<int>(parsed) : std::nullopt;
}
std::optional<int> positiveDimension(const QJsonValue &value)
{
    bool ok = false;
    const auto dimension = value.isString() ? value.toString().toInt(&ok) : static_cast<int>(value.toInteger(-1));
    if (!value.isString()) ok = value.isDouble() && value == QJsonValue(dimension);
    return ok && dimension > 0 && dimension <= 32768 ? std::optional<int>(dimension) : std::nullopt;
}
VodDetails describe(const SourceContext &source, const ContentRef &ref, const QJsonObject &info)
{
    VodDetails details; details.ref = ref;
    details.title=safeText(info.value(QStringLiteral("name")),source);
    for (const auto &field : {QStringLiteral("plot"), QStringLiteral("description"), QStringLiteral("overview")}) {
        details.description = safeText(info.value(field), source);
        if (!details.description.isEmpty()) break;
    }
    details.cast = safeText(info.value(QStringLiteral("cast")), source).split(u',', Qt::SkipEmptyParts);
    details.genres = safeText(info.value(QStringLiteral("genre")), source).split(u',', Qt::SkipEmptyParts);
    const auto seconds = number(info.value(QStringLiteral("duration_secs")));
    if (seconds && *seconds > 0 && *seconds <= 7 * 24 * 3600) details.declaredDurationMs = qint64(*seconds) * 1000;
    auto video = info.value(QStringLiteral("video")).toObject();
    if (video.isEmpty() && info.value(QStringLiteral("video")).isArray()) {
        for (const auto &entry : info.value(QStringLiteral("video")).toArray()) {
            if (entry.isObject()) { video = entry.toObject(); break; }
        }
    }
    details.declaredVideoWidth = positiveDimension(video.value(QStringLiteral("width")));
    details.declaredVideoHeight = positiveDimension(video.value(QStringLiteral("height")));
    if (!details.declaredVideoWidth || !details.declaredVideoHeight) {
        details.declaredVideoWidth.reset();
        details.declaredVideoHeight.reset();
    }
    return details;
}
struct SeriesData { VodDetails details; QHash<QString, QJsonObject> media; };
Result<SeriesData> parseSeries(const SourceContext &source, const ContentRef &series, const QJsonObject &root, const RequestContext &context)
{
    if (!root.value(QStringLiteral("info")).isObject()) return Error{ErrorCode::InvalidResponse, context.operationId};
    SeriesData parsed{describe(source, series, root.value(QStringLiteral("info")).toObject()), {}};
    QHash<QString, Season> seasons;
    const auto seasonList = root.value(QStringLiteral("seasons"));
    if (!seasonList.isUndefined() && !seasonList.isArray()) return Error{ErrorCode::InvalidResponse, context.operationId};
    for (const auto &entry : seasonList.toArray()) {
        const auto object = entry.toObject();
        auto id = identifier(object.value(QStringLiteral("season_number")));
        if (id.isEmpty()) id = identifier(object.value(QStringLiteral("id")));
        if (id.isEmpty() || seasons.contains(id)) return Error{ErrorCode::InvalidResponse, context.operationId};
        seasons.insert(id, Season{series, id, number(object.value(QStringLiteral("season_number"))), static_cast<int>(seasons.size())});
    }
    QMap<QString, QJsonArray> groups;
    const auto episodes = root.value(QStringLiteral("episodes"));
    if (episodes.isObject()) {
        const auto object = episodes.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (identifier(it.key()).isEmpty() || !it.value().isArray()) return Error{ErrorCode::InvalidResponse, context.operationId};
            groups.insert(it.key(), it.value().toArray());
        }
    } else if (episodes.isArray()) {
        for (const auto &entry : episodes.toArray()) {
            if (!entry.isObject()) return Error{ErrorCode::InvalidResponse, context.operationId};
            groups[identifier(entry.toObject().value(QStringLiteral("season")))].append(entry);
        }
    } else return Error{ErrorCode::InvalidResponse, context.operationId};
    for (auto group = groups.cbegin(); group != groups.cend(); ++group) {
        if (!group.key().isEmpty() && !seasons.contains(group.key()))
            seasons.insert(group.key(), Season{series, group.key(), number(group.key()), static_cast<int>(seasons.size())});
        int order = 0;
        for (const auto &entry : group.value()) {
            if (const auto error = context.interruption()) return *error;
            const auto object = entry.toObject();
            const auto id = identifier(object.value(QStringLiteral("id")));
            const auto season = identifier(object.value(QStringLiteral("season")));
            if (id.isEmpty() || parsed.media.contains(id) || !object.value(QStringLiteral("title")).isString()
                || (!season.isEmpty() && !group.key().isEmpty() && season != group.key()))
                return Error{ErrorCode::InvalidResponse, context.operationId};
            EpisodeSummary episode;
            episode.ref = {series.profileId, series.catalogNamespace, ContentKind::Episode, id, series.providerItemId};
            episode.series = series;
            if (!group.key().isEmpty()) episode.seasonId = group.key();
            episode.number = number(object.value(QStringLiteral("episode_num")));
            episode.order = order++;
            episode.title = safeText(object.value(QStringLiteral("title")), source);
            episode.availability = Availability::Available;
            const auto info = describe(source, episode.ref, object.value(QStringLiteral("info")).toObject());
            episode.durationMs = info.declaredDurationMs;
            episode.description = info.description;
            parsed.details.episodes.append(episode);
            parsed.media.insert(id, object);
        }
    }
    parsed.details.seasons = seasons.values();
    std::sort(parsed.details.seasons.begin(), parsed.details.seasons.end(), [](const Season &a, const Season &b) {
        if (a.number != b.number) { if (!a.number) return false; if (!b.number) return true; return *a.number < *b.number; }
        return a.order < b.order;
    });
    QHash<QString, int> order;
    for (qsizetype i = 0; i < parsed.details.seasons.size(); ++i) {
        auto &season = parsed.details.seasons[i]; season.order = static_cast<int>(i); order.insert(season.id, season.order);
    }
    std::stable_sort(parsed.details.episodes.begin(), parsed.details.episodes.end(), [&](const EpisodeSummary &a, const EpisodeSummary &b) {
        const auto aSeason = order.value(a.seasonId.value_or(QString()), static_cast<int>(seasons.size()));
        const auto bSeason = order.value(b.seasonId.value_or(QString()), static_cast<int>(seasons.size()));
        if (aSeason != bSeason) return aSeason < bSeason;
        if (a.number != b.number) { if (!a.number) return false; if (!b.number) return true; return *a.number < *b.number; }
        return a.order < b.order;
    });
    for (qsizetype i = 0; i < parsed.details.episodes.size(); ++i) parsed.details.episodes[i].order = static_cast<int>(i);
    return parsed;
}
QUrl baseUrl(const SourceContext &source)
{
    auto url = source.endpoint;
    auto path = url.path();
    while (path.endsWith(u'/')) path.chop(1);
    url.setPath(path);
    return url;
}
void waitRetry(qint64 milliseconds, const RequestContext &context)
{
    QEventLoop loop;
    QTimer interrupt;
    interrupt.setInterval(10);
    QObject::connect(&interrupt, &QTimer::timeout, &loop, [&]() { if (context.interruption()) loop.quit(); });
    QTimer::singleShot(static_cast<int>(std::clamp(milliseconds, qint64(0), qint64(30000))), &loop, &QEventLoop::quit);
    interrupt.start();
    loop.exec();
}
}
XtreamVodProvider::XtreamVodProvider(std::shared_ptr<IHttpTransport> transport, bool seriesEnabled)
    : m_transport(std::move(transport)), m_seriesEnabled(seriesEnabled) {}
Result<QJsonDocument> XtreamVodProvider::api(const SourceContext &source, const QString &action,
    const QMap<QString, QString> &parameters, const RequestContext &context) const
{
    if (const auto error = context.interruption()) return *error;
    if (!m_transport || !source.enabled || source.provider != QStringLiteral("xtream"))
        return Error{ErrorCode::UnsupportedCapability, context.operationId};
    if (source.revision != context.source || !httpUrlAllowed(source.endpoint) || source.endpoint.hasQuery()
        || source.username.isEmpty() || source.password.isEmpty()) return Error{ErrorCode::InvalidResponse, context.operationId};
    auto url = baseUrl(source);
    url.setPath(url.path() + QStringLiteral("/player_api.php"));
    // Encode raw values exactly once, including literal '%' and '+'.
    QByteArray query = "username=" + QUrl::toPercentEncoding(source.username) + "&password=" + QUrl::toPercentEncoding(source.password);
    if (!action.isEmpty()) query += "&action=" + QUrl::toPercentEncoding(action);
    for (auto it = parameters.cbegin(); it != parameters.cend(); ++it) query += '&' + QUrl::toPercentEncoding(it.key()) + '=' + QUrl::toPercentEncoding(it.value());
    url.setQuery(QString::fromLatin1(query), QUrl::StrictMode);
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (const auto error = context.interruption()) return *error;
        auto result = m_transport->get({url, {{"Accept", "application/json"}, {"User-Agent", "OKILTV/" OKILTV_APP_VERSION}}}, context);
        Error failure{ErrorCode::ProviderUnavailable, context.operationId, true};
        if (const auto *error = std::get_if<Error>(&result)) failure = *error;
        else {
            const auto &response = std::get<HttpResponse>(result);
            if (response.body.size() > context.responseByteLimit) return Error{ErrorCode::ResponseTooLarge, context.operationId};
            if (response.status == 200) {
                QJsonParseError parseError;
                auto document = QJsonDocument::fromJson(response.body, &parseError);
                if (parseError.error != QJsonParseError::NoError || document.isNull())
                    return Error{ErrorCode::InvalidResponse, context.operationId};
                return document;
            }
            if (response.status == 401) return Error{ErrorCode::Unauthorized, context.operationId};
            if (response.status == 403) return Error{ErrorCode::Forbidden, context.operationId};
            if (response.status == 404) return Error{ErrorCode::ContentUnavailable, context.operationId};
            failure = {response.status == 429 ? ErrorCode::RateLimited : ErrorCode::ProviderUnavailable,
                context.operationId, response.status == 429 || response.status == 502 || response.status == 503 || response.status == 504,
                retryAfterMs(response.headers.value("retry-after"), QDateTime::currentDateTimeUtc())};
        }
        if (!failure.retryable || attempt == 2) return failure;
        const auto delay = failure.retryAfterMs.value_or(100 * (attempt + 1) + QRandomGenerator::global()->bounded(100));
        if (delay >= context.deadline.remainingTime() || delay > 30000) return failure;
        waitRetry(delay, context);
    }
    return Error{ErrorCode::ProviderUnavailable, context.operationId};
}
Result<ProviderCapabilities> XtreamVodProvider::capabilities(const SourceContext &source, const RequestContext &context)
{
    const auto auth = api(source, {}, {}, context);
    if (const auto *error = std::get_if<Error>(&auth)) return *error;
    const auto object = std::get<QJsonDocument>(auth).object().value(QStringLiteral("user_info")).toObject();
    if (!object.contains(QStringLiteral("auth"))) return Error{ErrorCode::InvalidResponse, context.operationId};
    if (identifier(object.value(QStringLiteral("auth"))) != QStringLiteral("1")) return Error{ErrorCode::Unauthorized, context.operationId};
    ProviderCapabilities result;
    result.authorization = Authorization::Authorized;
    const auto categories = listCategories(source, {source.revision.profileId, source.revision.catalogNamespace, CatalogKind::Movies, {}}, context);
    if (const auto *error = std::get_if<Error>(&categories)) return *error;
    result.movies = Capability::Supported;
    if (m_seriesEnabled) {
        const auto series = listCategories(source, {source.revision.profileId, source.revision.catalogNamespace, CatalogKind::Series, {}}, context);
        result.series = std::holds_alternative<Error>(series) ? Capability::TemporarilyUnavailable : Capability::Supported;
    } else result.series = Capability::Unsupported;
    result.paging = Capability::Unsupported;
    // Category lists do not prove server-side filtering or detail support.
    return result;
}
Result<QList<VodCategory>> XtreamVodProvider::listCategories(const SourceContext &source, const CatalogScope &scope, const RequestContext &context)
{
    if (!matches(source, scope)) return Error{ErrorCode::ContentUnavailable, context.operationId};
    if (scope.kind != CatalogKind::Movies && scope.kind != CatalogKind::Series) return Error{ErrorCode::UnsupportedCapability, context.operationId};
    const auto response = api(source, scope.kind == CatalogKind::Movies ? QStringLiteral("get_vod_categories") : QStringLiteral("get_series_categories"), {}, context);
    if (const auto *error = std::get_if<Error>(&response)) return *error;
    const auto document = std::get<QJsonDocument>(response);
    if (!document.isArray()) return Error{ErrorCode::InvalidResponse, context.operationId};
    QList<VodCategory> categories;
    QSet<QString> seen;
    for (const auto &entry : document.array()) {
        if (const auto error = context.interruption()) return *error;
        const auto object = entry.toObject();
        const auto id = identifier(object.value(QStringLiteral("category_id")));
        if (id.isEmpty()) return Error{ErrorCode::InvalidResponse, context.operationId};
        if (seen.contains(id)) continue;
        seen.insert(id);
        auto categoryScope = scope; categoryScope.categoryId.reset();
        VodCategory category{categoryScope, id, safeText(object.value(QStringLiteral("category_name")), source), {}};
        const auto parent = identifier(object.value(QStringLiteral("parent_id")));
        if (!parent.isEmpty() && parent != QStringLiteral("0")) category.parentId = parent;
        categories.append(category);
    }
    return categories;
}
Result<CatalogBatch> XtreamVodProvider::fetchCatalog(const SourceContext &source, const CatalogScope &scope,
    const std::optional<ProviderCursor> &cursor, const RequestContext &context)
{
    if (!matches(source, scope)) return Error{ErrorCode::ContentUnavailable, context.operationId};
    if ((scope.kind != CatalogKind::Movies && (scope.kind != CatalogKind::Series || !m_seriesEnabled)) || cursor) return Error{ErrorCode::UnsupportedCapability, context.operationId};
    // Request the scope, and still validate membership locally.
    const bool series = scope.kind == CatalogKind::Series;
    const auto response = api(source, series ? QStringLiteral("get_series") : QStringLiteral("get_vod_streams"),
        scope.categoryId ? QMap<QString, QString>{{QStringLiteral("category_id"), *scope.categoryId}} : QMap<QString, QString>{}, context);
    if (const auto *error = std::get_if<Error>(&response)) return *error;
    const auto document = std::get<QJsonDocument>(response);
    if (!document.isArray()) return Error{ErrorCode::InvalidResponse, context.operationId};
    CatalogBatch batch{scope, {}, true, {}};
    QHash<QString, qsizetype> identities;
    for (const auto &entry : document.array()) {
        if (const auto error = context.interruption()) return *error;
        const auto object = entry.toObject();
        const auto id = identifier(object.value(series ? QStringLiteral("series_id") : QStringLiteral("stream_id")));
        if (id.isEmpty() || !object.value(QStringLiteral("name")).isString()) return Error{ErrorCode::InvalidResponse, context.operationId};
        MovieSummary movie;
        movie.ref = {scope.profileId, scope.catalogNamespace, series ? ContentKind::Series : ContentKind::Movie, id, {}};
        movie.title = safeText(object.value(QStringLiteral("name")), source);
        bool yearOk = false;
        const auto year = identifier(object.value(QStringLiteral("year"))).toInt(&yearOk);
        if (yearOk && year >= 1800 && year <= 9999) movie.year = year;
        const auto category = identifier(object.value(QStringLiteral("category_id")));
        if (!category.isEmpty()) movie.categoryIds.append(category);
        for (const auto &value : object.value(QStringLiteral("category_ids")).toArray()) {
            const auto extra = identifier(value);
            if (!extra.isEmpty() && !movie.categoryIds.contains(extra)) movie.categoryIds.append(extra);
        }
        if (registerArtwork) {
            const auto art = registerArtwork(source, QUrl(object.value(series ? QStringLiteral("cover") : QStringLiteral("stream_icon")).toString()), context);
            if (art) movie.artwork.append(*art);
        }
        movie.availability = Availability::Available;
        const auto existing = identities.constFind(id);
        if (existing != identities.cend()) {
            std::visit([&](auto &summary) {
                for (const auto &categoryId : movie.categoryIds) if (!summary.categoryIds.contains(categoryId)) summary.categoryIds.append(categoryId);
            }, batch.items[*existing]);
            continue;
        }
        identities.insert(id, batch.items.size());
        if (series) batch.items.append(SeriesSummary{movie.ref, movie.title, movie.year, movie.artwork, movie.categoryIds, movie.availability});
        else batch.items.append(movie);
    }
    if (scope.categoryId) batch.items.removeIf([&](const CatalogItem &item) {
        return std::visit([&](const auto &summary) { return !summary.categoryIds.contains(*scope.categoryId); }, item);
    });
    return batch;
}
Result<VodDetails> XtreamVodProvider::fetchDetails(const SourceContext &source, const ContentRef &ref, const RequestContext &context)
{
    if (!matches(source, ref)) return Error{ErrorCode::ContentUnavailable, context.operationId};
    if (ref.kind != ContentKind::Movie && m_seriesEnabled) {
        ContentRef series = ref;
        if (ref.kind == ContentKind::Episode) {
            if (!ref.parentNamespace) return Error{ErrorCode::ContentUnavailable, context.operationId};
            series = {ref.profileId, ref.catalogNamespace, ContentKind::Series, *ref.parentNamespace, {}};
        }
        const auto response = api(source, QStringLiteral("get_series_info"), {{QStringLiteral("series_id"), series.providerItemId}}, context);
        if (const auto *error = std::get_if<Error>(&response)) return *error;
        const auto parsed = parseSeries(source, series, std::get<QJsonDocument>(response).object(), context);
        if (const auto *error = std::get_if<Error>(&parsed)) return *error;
        const auto &value = std::get<SeriesData>(parsed);
        if (ref.kind == ContentKind::Series) {
            auto details = value.details;
            if (registerArtwork) {
                const auto info = std::get<QJsonDocument>(response).object().value(QStringLiteral("info")).toObject();
                const auto art = registerArtwork(source, QUrl(info.value(QStringLiteral("cover")).toString()), context);
                if (art) details.artwork.append(*art);
                for (auto &episode : details.episodes) {
                    const auto media = value.media.value(episode.ref.providerItemId).value(QStringLiteral("info")).toObject();
                    const auto thumbnail = registerArtwork(source, QUrl(media.value(QStringLiteral("movie_image")).toString()), context);
                    if (thumbnail) episode.artwork.append(*thumbnail);
                }
            }
            return details;
        }
        if (!value.media.contains(ref.providerItemId)) return Error{ErrorCode::ContentUnavailable, context.operationId};
        return describe(source, ref, value.media.value(ref.providerItemId).value(QStringLiteral("info")).toObject());
    }
    if (ref.kind != ContentKind::Movie) return Error{ErrorCode::UnsupportedCapability, context.operationId};
    const auto response = api(source, QStringLiteral("get_vod_info"), {{QStringLiteral("vod_id"), ref.providerItemId}}, context);
    if (const auto *error = std::get_if<Error>(&response)) return *error;
    const auto root = std::get<QJsonDocument>(response).object();
    const auto movie = root.value(QStringLiteral("movie_data")).toObject();
    if (identifier(movie.value(QStringLiteral("stream_id"))) != ref.providerItemId || !root.value(QStringLiteral("info")).isObject())
        return Error{ErrorCode::InvalidResponse, context.operationId};
    const auto info = root.value(QStringLiteral("info")).toObject();
    auto details = describe(source, ref, info);
    if (registerArtwork) {
        const auto art = registerArtwork(source, QUrl(info.value(QStringLiteral("movie_image")).toString()), context);
        if (art) details.artwork.append(*art);
    }
    return details;
}
Result<PlaybackDescriptor> XtreamVodProvider::resolvePlayback(const SourceContext &source, const ContentRef &ref,
    const PlaybackPreferences &, const RequestContext &context)
{
    if (!matches(source, ref) || !ref.playable()) return Error{ErrorCode::ContentUnavailable, context.operationId};
    QJsonObject movie;
    if (ref.kind == ContentKind::Episode && m_seriesEnabled && ref.parentNamespace) {
        const ContentRef series{ref.profileId, ref.catalogNamespace, ContentKind::Series, *ref.parentNamespace, {}};
        const auto response = api(source, QStringLiteral("get_series_info"), {{QStringLiteral("series_id"), series.providerItemId}}, context);
        if (const auto *error = std::get_if<Error>(&response)) return *error;
        const auto parsed = parseSeries(source, series, std::get<QJsonDocument>(response).object(), context);
        if (const auto *error = std::get_if<Error>(&parsed)) return *error;
        const auto &media = std::get<SeriesData>(parsed).media;
        if (!media.contains(ref.providerItemId)) return Error{ErrorCode::ContentUnavailable, context.operationId};
        movie = media.value(ref.providerItemId);
    } else if (ref.kind == ContentKind::Movie) {
        const auto response = api(source, QStringLiteral("get_vod_info"), {{QStringLiteral("vod_id"), ref.providerItemId}}, context);
        if (const auto *error = std::get_if<Error>(&response)) return *error;
        movie = std::get<QJsonDocument>(response).object().value(QStringLiteral("movie_data")).toObject();
        if (identifier(movie.value(QStringLiteral("stream_id"))) != ref.providerItemId) return Error{ErrorCode::InvalidResponse, context.operationId};
    } else return Error{ErrorCode::UnsupportedCapability, context.operationId};
    const auto direct = movie.value(QStringLiteral("direct_source")).toString();
    QUrl media;
    if (!direct.isEmpty()) {
        media = QUrl(direct, QUrl::StrictMode);
        // Cross-origin direct URLs require an explicit provider policy in B4.
        if (!httpUrlAllowed(media) || !sameOrigin(source.endpoint, media)) return Error{ErrorCode::InvalidResponse, context.operationId};
    } else {
        const auto extension = movie.value(QStringLiteral("container_extension")).toString().toLower();
        static const QRegularExpression allowed(QStringLiteral("^[a-z0-9]{1,16}$"));
        if (!allowed.match(extension).hasMatch()) return Error{ErrorCode::InvalidResponse, context.operationId};
        auto encoded = baseUrl(source).toEncoded();
        encoded += (ref.kind == ContentKind::Episode ? "/series/" : "/movie/") + QUrl::toPercentEncoding(source.username, {}, ".") + '/' + QUrl::toPercentEncoding(source.password, {}, ".")
            + '/' + QUrl::toPercentEncoding(ref.providerItemId, {}, ".") + '.' + extension.toLatin1();
        media = QUrl::fromEncoded(encoded, QUrl::StrictMode);
    }
    return PlaybackDescriptor{ref, source.revision, media, {}, QDateTime::currentDateTimeUtc().addSecs(60), {}};
}
}
