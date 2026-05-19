#include "models.h"

namespace OKILTV::Core {

namespace {

int profileTypeToInt(const ProfileType type)
{
    return static_cast<int>(type);
}

} // namespace

QVariantMap toVariantMap(const ServerProfile &profile)
{
    return {
        { QStringLiteral("id"), guidToString(profile.id) },
        { QStringLiteral("name"), profile.name },
        { QStringLiteral("type"), profileTypeToInt(profile.type) },
        { QStringLiteral("typeLabel"), profileTypeLabel(profile.type) },
        { QStringLiteral("xtreamBaseUrl"), profile.xtreamBaseUrl },
        { QStringLiteral("xtreamUsername"), profile.xtreamUsername },
        { QStringLiteral("xtreamPassword"), profile.xtreamPassword },
        { QStringLiteral("xtreamServerTimezone"), profile.xtreamServerTimezone },
        { QStringLiteral("m3UUrl"), profile.m3uUrl },
        { QStringLiteral("m3UFilePath"), profile.m3uFilePath },
        { QStringLiteral("xmltvUrl"), profile.xmltvUrl },
        { QStringLiteral("autoRefreshIntervalHours"), normalizeAutoRefreshIntervalHours(profile.autoRefreshIntervalHours) },
        { QStringLiteral("lastRefreshed"), profile.lastRefreshed.toUTC().toString(Qt::ISODateWithMs) },
        { QStringLiteral("isActive"), profile.isActive }
    };
}

QVariantMap toVariantMap(const ChannelCategory &category)
{
    return {
        { QStringLiteral("id"), category.id },
        { QStringLiteral("name"), category.name },
        { QStringLiteral("parentId"), category.parentId }
    };
}

QVariantMap toVariantMap(const Channel &channel)
{
    return {
        { QStringLiteral("id"), channel.id },
        { QStringLiteral("name"), channel.name },
        { QStringLiteral("categoryId"), channel.categoryId },
        { QStringLiteral("tvgId"), channel.tvgId },
        { QStringLiteral("tvgName"), channel.tvgName },
        { QStringLiteral("iconUrl"), channel.iconUrl },
        { QStringLiteral("cachedIconPath"), channel.cachedIconPath },
        { QStringLiteral("sortOrder"), channel.sortOrder },
        { QStringLiteral("profileId"), guidToString(channel.profileId) },
        { QStringLiteral("source"), channelSourceToString(channel.source) },
        { QStringLiteral("streamUrl"), channel.streamUrl },
        { QStringLiteral("catchupSupported"), channel.catchupSupported },
        { QStringLiteral("catchupWindowHours"), channel.catchupWindowHours },
        { QStringLiteral("catchupMode"), channel.catchupMode },
        { QStringLiteral("catchupSourceTemplate"), channel.catchupSourceTemplate }
    };
}

QVariantMap toVariantMap(const EpgEntry &entry)
{
    return {
        { QStringLiteral("channelId"), entry.channelId },
        { QStringLiteral("title"), entry.title },
        { QStringLiteral("subTitle"), entry.subTitle },
        { QStringLiteral("description"), entry.description },
        { QStringLiteral("episodeNum"), entry.episodeNum },
        { QStringLiteral("start"), entry.start.toUTC().toString(Qt::ISODateWithMs) },
        { QStringLiteral("stop"), entry.stop.toUTC().toString(Qt::ISODateWithMs) },
        { QStringLiteral("isNow"), epgEntryIsNow(entry) },
        { QStringLiteral("progressPercent"), epgEntryProgressPercent(entry) },
        { QStringLiteral("timeRange"), epgEntryTimeRange(entry) },
        { QStringLiteral("startTimeLabel"), epgEntryStartTimeLabel(entry) }
    };
}

QVariantList toVariantList(const QList<EpgEntry> &entries)
{
    QVariantList list;
    list.reserve(entries.size());
    for (const auto &entry : entries) {
        list.push_back(toVariantMap(entry));
    }
    return list;
}

} // namespace OKILTV::Core
