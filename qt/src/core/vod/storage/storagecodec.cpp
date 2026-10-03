#include "storagecodec.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimeZone>

namespace OKILTV::Vod::Storage {
namespace {
QJsonObject encodeRef(const ContentRef &ref)
{
    QJsonObject result{{QStringLiteral("profile"), ref.profileId.toString()}, {QStringLiteral("namespace"), ref.catalogNamespace.toString()},
        {QStringLiteral("kind"), int(ref.kind)}, {QStringLiteral("id"), ref.providerItemId}};
    if (const auto parent = ref.parentNamespace) result.insert(QStringLiteral("parent"), *parent);
    return result;
}
ContentRef decodeRef(const QJsonObject &object)
{
    ContentRef result{QUuid(object.value(QStringLiteral("profile")).toString()), QUuid(object.value(QStringLiteral("namespace")).toString()),
        ContentKind(object.value(QStringLiteral("kind")).toInt(-1)), object.value(QStringLiteral("id")).toString(), {}};
    if (object.contains(QStringLiteral("parent"))) result.parentNamespace = object.value(QStringLiteral("parent")).toString();
    return result;
}
QJsonObject encodeTrack(const VodMediaTrack &track)
{
    return {{QStringLiteral("index"), track.streamIndex}, {QStringLiteral("ordinal"), track.ordinal},
        {QStringLiteral("type"), track.type}, {QStringLiteral("codec"), track.codec},
        {QStringLiteral("title"), track.title}, {QStringLiteral("language"), track.language},
        {QStringLiteral("default"), track.isDefault}, {QStringLiteral("forced"), track.forced}};
}
std::optional<VodMediaTrack> decodeTrack(const QJsonValue &entry, const QString &expectedType, int fallbackOrdinal)
{
    if (!entry.isObject()) return {};
    const auto object = entry.toObject();
    VodMediaTrack track;
    track.streamIndex = object.value(QStringLiteral("index")).toInt(-1);
    track.ordinal = object.value(QStringLiteral("ordinal")).toInt(fallbackOrdinal);
    track.type = object.value(QStringLiteral("type")).toString();
    track.codec = object.value(QStringLiteral("codec")).toString().left(128);
    track.title = object.value(QStringLiteral("title")).toString().left(1024);
    track.language = object.value(QStringLiteral("language")).toString().left(128);
    track.isDefault = object.value(QStringLiteral("default")).toBool();
    track.forced = object.value(QStringLiteral("forced")).toBool();
    if (track.streamIndex < 0 || track.ordinal < 0 || track.type != expectedType) return {};
    return track;
}
}
QByteArray encodeDetails(const VodDetails &details)
{
    QJsonObject object{{QStringLiteral("version"), 1}, {QStringLiteral("ref"), encodeRef(details.ref)},
        {QStringLiteral("description"), details.description}, {QStringLiteral("cast"), QJsonArray::fromStringList(details.cast)},
        {QStringLiteral("genres"), QJsonArray::fromStringList(details.genres)}};
    if (const auto duration = details.declaredDurationMs) object.insert(QStringLiteral("duration"), *duration);
    if (details.declaredVideoWidth && details.declaredVideoHeight) {
        object.insert(QStringLiteral("declaredVideoWidth"), *details.declaredVideoWidth);
        object.insert(QStringLiteral("declaredVideoHeight"), *details.declaredVideoHeight);
    }
    if (const auto &probe = details.mediaProbe) {
        QJsonObject media{{QStringLiteral("observed"), probe->observedAtUtc.toMSecsSinceEpoch()}};
        if (probe->videoWidth && probe->videoHeight) {
            media.insert(QStringLiteral("videoWidth"), *probe->videoWidth);
            media.insert(QStringLiteral("videoHeight"), *probe->videoHeight);
        }
        QJsonArray audio;
        for (const auto &track : probe->audioTracks) audio.append(encodeTrack(track));
        QJsonArray subtitles;
        for (const auto &track : probe->subtitleTracks) subtitles.append(encodeTrack(track));
        media.insert(QStringLiteral("audio"), audio);
        media.insert(QStringLiteral("subtitles"), subtitles);
        object.insert(QStringLiteral("mediaProbe"), media);
    }
    QJsonArray seasons;
    for (const auto &season : details.seasons) {
        QJsonObject value{{QStringLiteral("series"), encodeRef(season.series)}, {QStringLiteral("id"), season.id}, {QStringLiteral("order"), season.order}};
        if (const auto number = season.number) value.insert(QStringLiteral("number"), *number);
        seasons.append(value);
    }
    QJsonArray episodes;
    for (const auto &episode : details.episodes) {
        QJsonObject value{{QStringLiteral("ref"), encodeRef(episode.ref)}, {QStringLiteral("series"), encodeRef(episode.series)},
            {QStringLiteral("title"), episode.title}, {QStringLiteral("order"), episode.order}, {QStringLiteral("availability"), int(episode.availability)}};
        if (const auto season = episode.seasonId) value.insert(QStringLiteral("season"), *season);
        if (const auto number = episode.number) value.insert(QStringLiteral("number"), *number);
        episodes.append(value);
    }
    object.insert(QStringLiteral("seasons"), seasons);
    object.insert(QStringLiteral("episodes"), episodes);
    QJsonArray artwork;
    for (const auto &art : details.artwork)
        artwork.append(QJsonObject{{QStringLiteral("id"), art.id}, {QStringLiteral("role"), art.role}});
    object.insert(QStringLiteral("artwork"), artwork);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
std::optional<VodDetails> decodeDetails(const QByteArray &bytes)
{
    if (bytes.size() > qsizetype(16) * 1024 * 1024) return {};
    const auto object = QJsonDocument::fromJson(bytes).object();
    if (object.value(QStringLiteral("version")).toInt() != 1) return {};
    VodDetails details;
    details.ref = decodeRef(object.value(QStringLiteral("ref")).toObject());
    if (!details.ref.valid()) return {};
    details.description = object.value(QStringLiteral("description")).toString();
    for (const auto &value : object.value(QStringLiteral("cast")).toArray()) details.cast.append(value.toString());
    for (const auto &value : object.value(QStringLiteral("genres")).toArray()) details.genres.append(value.toString());
    if (object.contains(QStringLiteral("duration"))) details.declaredDurationMs = object.value(QStringLiteral("duration")).toInteger();
    const auto declaredWidth = object.value(QStringLiteral("declaredVideoWidth")).toInt();
    const auto declaredHeight = object.value(QStringLiteral("declaredVideoHeight")).toInt();
    if (declaredWidth > 0 && declaredWidth <= 32768 && declaredHeight > 0 && declaredHeight <= 32768) {
        details.declaredVideoWidth = declaredWidth;
        details.declaredVideoHeight = declaredHeight;
    }
    const auto media = object.value(QStringLiteral("mediaProbe")).toObject();
    if (!media.isEmpty()) {
        VodMediaProbe probe;
        probe.observedAtUtc = QDateTime::fromMSecsSinceEpoch(
            media.value(QStringLiteral("observed")).toInteger(), QTimeZone::UTC);
        const auto width = media.value(QStringLiteral("videoWidth")).toInt();
        const auto height = media.value(QStringLiteral("videoHeight")).toInt();
        if (width > 0 && width <= 32768 && height > 0 && height <= 32768) {
            probe.videoWidth = width;
            probe.videoHeight = height;
        }
        const auto decodeTracks = [](const QJsonArray &values, const QString &type, QList<VodMediaTrack> &target) {
            if (values.size() > 128) return false;
            for (int index = 0; index < values.size(); ++index) {
                const auto track = decodeTrack(values.at(index), type, index);
                if (!track) return false;
                target.append(*track);
            }
            return true;
        };
        if (probe.observedAtUtc.isValid()
            && decodeTracks(media.value(QStringLiteral("audio")).toArray(), QStringLiteral("audio"), probe.audioTracks)
            && decodeTracks(media.value(QStringLiteral("subtitles")).toArray(), QStringLiteral("sub"), probe.subtitleTracks)) {
            details.mediaProbe = std::move(probe);
        }
    }
    for (const auto &value : object.value(QStringLiteral("artwork")).toArray()) {
        const auto art = value.toObject();
        details.artwork.append({art.value(QStringLiteral("id")).toString(), art.value(QStringLiteral("role")).toString(), {}});
    }
    QSet<QString> seasonIds;
    QSet<QByteArray> episodeIds;
    for (const auto &entry : object.value(QStringLiteral("seasons")).toArray()) {
        const auto value = entry.toObject();
        Season season{decodeRef(value.value(QStringLiteral("series")).toObject()), value.value(QStringLiteral("id")).toString(), {}, value.value(QStringLiteral("order")).toInt()};
        if (value.contains(QStringLiteral("number"))) season.number = value.value(QStringLiteral("number")).toInt();
        if (season.series != details.ref || season.id.isEmpty() || details.ref.kind != ContentKind::Series || seasonIds.contains(season.id)) return {};
        seasonIds.insert(season.id);
        details.seasons.append(season);
    }
    for (const auto &entry : object.value(QStringLiteral("episodes")).toArray()) {
        const auto value = entry.toObject();
        EpisodeSummary episode;
        episode.ref = decodeRef(value.value(QStringLiteral("ref")).toObject());
        episode.series = decodeRef(value.value(QStringLiteral("series")).toObject());
        episode.title = value.value(QStringLiteral("title")).toString();
        episode.order = value.value(QStringLiteral("order")).toInt();
        episode.availability = Availability(value.value(QStringLiteral("availability")).toInt());
        if (value.contains(QStringLiteral("season"))) episode.seasonId = value.value(QStringLiteral("season")).toString();
        if (value.contains(QStringLiteral("number"))) episode.number = value.value(QStringLiteral("number")).toInt();
        if (!episode.ref.playable() || episode.ref.kind != ContentKind::Episode || episode.series != details.ref
            || details.ref.kind != ContentKind::Series || episode.ref.profileId != details.ref.profileId
            || episode.ref.catalogNamespace != details.ref.catalogNamespace
            || episode.ref.parentNamespace != std::optional<QString>(details.ref.providerItemId)
            || (episode.seasonId && !seasonIds.contains(*episode.seasonId)) || episodeIds.contains(episode.ref.key())) return {};
        episodeIds.insert(episode.ref.key());
        details.episodes.append(episode);
    }
    return details;
}
}
