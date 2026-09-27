#pragma once
#include "player/mpvplayer.h"
#include "core/trackpreferences.h"

namespace OKILTV::App::Playback {
inline QString trackSubtitle(const QVariantMap &track)
{
    auto title = track.value(QStringLiteral("title")).toString().trimmed();
    if (!title.isEmpty()) {
        return title;
    }
    return track.value(QStringLiteral("lang")).toString().trimmed();
}

inline QVariantList audioTracks(const Player::MpvPlayer *activePlayer)
{
    QVariantList result;
    int displayIndex = 1;
    for (const auto &t : (activePlayer ? activePlayer->trackList() : QVariantList{})) {
        const auto tm = t.toMap();
        if (tm.value(QStringLiteral("type")).toString() != QLatin1String("audio")) {
            continue;
        }
        QVariantMap entry;
        entry[QStringLiteral("id")]       = tm.value(QStringLiteral("id"));
        entry[QStringLiteral("name")]     = QStringLiteral("Audio #%1").arg(displayIndex++);
        entry[QStringLiteral("subtitle")] = trackSubtitle(tm);
        entry[QStringLiteral("selected")] = tm.value(QStringLiteral("selected")).toBool();
        result.append(entry);
    }
    return result;
}

inline QVariantList subtitleTracks(const Player::MpvPlayer *activePlayer)
{
    QVariantMap none;
    none[QStringLiteral("id")]       = 0;
    none[QStringLiteral("name")]     = QStringLiteral("Subtitle #0");
    none[QStringLiteral("subtitle")] = QStringLiteral("None");
    const auto tracks = (activePlayer ? activePlayer->trackList() : QVariantList{});
    none[QStringLiteral("selected")] = Core::selectedTrackId(tracks, QStringLiteral("sub")) == 0;
    QVariantList result;
    result.append(none);
    int displayIndex = 1;
    for (const auto &t : tracks) {
        const auto tm = t.toMap();
        if (tm.value(QStringLiteral("type")).toString() != QLatin1String("sub")) {
            continue;
        }
        QVariantMap entry;
        entry[QStringLiteral("id")]       = tm.value(QStringLiteral("id"));
        entry[QStringLiteral("name")]     = QStringLiteral("Subtitle #%1").arg(displayIndex++);
        entry[QStringLiteral("subtitle")] = trackSubtitle(tm);
        entry[QStringLiteral("selected")] = tm.value(QStringLiteral("selected")).toBool();
        result.append(entry);
    }
    return result;
}

}
