#pragma once

#include "models.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QLocale>
#include <QVariantList>

namespace OKILTV::Core {

inline QString trackPreferenceChannelKey(const Channel &channel)
{
    if (channel.source == ChannelSource::Xtream) {
        return QStringLiteral("xtream:%1").arg(channel.id);
    }
    if (channel.streamUrl.trimmed().isEmpty()) {
        return {};
    }
    return QStringLiteral("m3u:") + QString::fromLatin1(QCryptographicHash::hash(
        channel.streamUrl.trimmed().toUtf8(), QCryptographicHash::Sha256).toHex());
}

inline QString normalizedTrackLabel(const QVariant &value)
{
    return value.toString().trimmed().toCaseFolded();
}

inline QString normalizedTrackLanguage(const QVariant &value)
{
    const auto code = normalizedTrackLabel(value);
    const auto language = QLocale::codeToLanguage(code, QLocale::ISO639);
    // ffprobe preserves ISO 639-2 tags; newer mpv versions expose ISO 639-1.
    // Keep unknown/empty tags distinct instead of mapping them all to C.
    return language == QLocale::AnyLanguage || language == QLocale::C
        ? code : QLocale::languageToCode(language);
}

inline QJsonArray trackLayout(const QVariantList &tracks, const QString &type)
{
    QJsonArray layout;
    for (const auto &value : tracks) {
        const auto track = value.toMap();
        if (track.value(QStringLiteral("type")).toString() == type) {
            layout.append(QJsonObject {
                { QStringLiteral("id"), track.value(QStringLiteral("id")).toInt() },
                { QStringLiteral("title"), normalizedTrackLabel(track.value(QStringLiteral("title"))) },
                { QStringLiteral("lang"), normalizedTrackLabel(track.value(QStringLiteral("lang"))) }
            });
        }
    }
    return layout;
}

inline QJsonObject makeTrackPreference(const QVariantList &tracks, const QString &type, const int id)
{
    if (type == QLatin1String("sub") && id == 0) {
        return { { QStringLiteral("mode"), QStringLiteral("off") } };
    }
    int ordinal = 0;
    for (const auto &value : tracks) {
        const auto track = value.toMap();
        if (track.value(QStringLiteral("type")).toString() != type) {
            continue;
        }
        if (track.value(QStringLiteral("id")).toInt() != id || id <= 0) { ++ordinal; continue; }
        return {
            { QStringLiteral("mode"), QStringLiteral("track") },
            { QStringLiteral("id"), id },
            { QStringLiteral("ordinal"), ordinal },
            { QStringLiteral("title"), normalizedTrackLabel(track.value(QStringLiteral("title"))) },
            { QStringLiteral("lang"), normalizedTrackLabel(track.value(QStringLiteral("lang"))) },
            { QStringLiteral("codec"), normalizedTrackLabel(track.value(QStringLiteral("codec"))) },
            { QStringLiteral("layout"), trackLayout(tracks, type) }
        };
    }
    return {};
}

// No language-only approximation: an audio description/commentary track must
// not silently replace the user's original track in the same language.
inline int matchTrackPreference(const QVariantList &tracks, const QString &type, const QJsonObject &preference)
{
    const auto mode = preference.value(QStringLiteral("mode")).toString();
    if (mode == QLatin1String("off") && type == QLatin1String("sub")) {
        return 0;
    }
    const auto savedId = preference.value(QStringLiteral("id")).toInt(-1);
    const auto savedOrdinal = preference.value(QStringLiteral("ordinal")).toInt(-1);
    if (mode != QLatin1String("track") || (savedId <= 0 && savedOrdinal < 0)) {
        return -1;
    }
    const auto title = preference.value(QStringLiteral("title")).toString();
    const auto language = normalizedTrackLanguage(preference.value(QStringLiteral("lang")).toString());
    const auto codec = preference.value(QStringLiteral("codec")).toString();
    if (title.isEmpty() && language.isEmpty() && codec.isEmpty()
        && preference.contains(QStringLiteral("layout"))
        && preference.value(QStringLiteral("layout")).toArray() != trackLayout(tracks, type)) {
        return -1;
    }
    QList<int> candidates;
    int ordinal = 0;
    int ordinalMatch = -1;
    for (const auto &value : tracks) {
        const auto track = value.toMap();
        if (track.value(QStringLiteral("type")).toString() != type) continue;
        const bool metadataMatches = normalizedTrackLabel(track.value(QStringLiteral("title"))) == title
            && normalizedTrackLanguage(track.value(QStringLiteral("lang"))) == language
            && (codec.isEmpty() || normalizedTrackLabel(track.value(QStringLiteral("codec"))) == codec);
        if (metadataMatches) {
            candidates.append(track.value(QStringLiteral("id")).toInt());
            if (ordinal == savedOrdinal) ordinalMatch = track.value(QStringLiteral("id")).toInt();
        }
        ++ordinal;
    }
    if (savedId <= 0 && ordinalMatch > 0) return ordinalMatch;
    if (title.isEmpty() && language.isEmpty()) {
        return candidates.contains(savedId) ? savedId : -1;
    }
    if (candidates.size() == 1) {
        return candidates.front();
    }
    return candidates.contains(savedId) ? savedId : -1;
}

inline int selectedTrackId(const QVariantList &tracks, const QString &type)
{
    for (const auto &value : tracks) {
        const auto track = value.toMap();
        if (track.value(QStringLiteral("type")).toString() == type
            && track.value(QStringLiteral("selected")).toBool()) {
            return track.value(QStringLiteral("id")).toInt();
        }
    }
    return 0;
}

} // namespace OKILTV::Core
