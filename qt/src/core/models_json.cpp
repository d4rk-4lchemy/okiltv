#include "models.h"

#include <QJsonArray>
#include <QJsonValue>

#include <algorithm>

namespace OKILTV::Core {

namespace {

QString dateTimeToJsonString(const QDateTime &value)
{
    if (!value.isValid()) {
        return QStringLiteral("0001-01-01T00:00:00");
    }

    return value.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime dateTimeFromJsonValue(const QJsonValue &value)
{
    const auto raw = value.toString();
    if (raw.isEmpty()) {
        return {};
    }

    auto parsed = QDateTime::fromString(raw, Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(raw, Qt::ISODate);
    }
    if (!parsed.isValid()) {
        return {};
    }

    return parsed.toUTC();
}

int profileTypeToInt(const ProfileType type)
{
    return static_cast<int>(type);
}

ProfileType profileTypeFromJsonValue(const QJsonValue &value)
{
    if (value.isDouble()) {
        switch (value.toInt()) {
        case 1:
            return ProfileType::M3UUrl;
        case 2:
            return ProfileType::M3UFile;
        case 0:
        default:
            return ProfileType::Xtream;
        }
    }

    const auto lowered = value.toString().trimmed().toLower();
    if (lowered == QStringLiteral("m3uurl")) {
        return ProfileType::M3UUrl;
    }
    if (lowered == QStringLiteral("m3ufile")) {
        return ProfileType::M3UFile;
    }
    return ProfileType::Xtream;
}

QJsonObject stringMapToJson(const QMap<QString, QString> &map)
{
    QJsonObject object;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        object.insert(it.key(), it.value());
    }
    return object;
}

QMap<QString, QString> stringMapFromJson(const QJsonObject &object)
{
    QMap<QString, QString> map;
    for (auto it = object.begin(); it != object.end(); ++it) {
        map.insert(it.key(), it.value().toString());
    }
    return map;
}

QJsonObject intMapToJson(const QMap<QString, int> &map)
{
    QJsonObject object;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        object.insert(it.key(), it.value());
    }
    return object;
}

QMap<QString, int> intMapFromJson(const QJsonObject &object)
{
    QMap<QString, int> map;
    for (auto it = object.begin(); it != object.end(); ++it) {
        map.insert(it.key(), it.value().toInt());
    }
    return map;
}

QJsonObject intListMapToJson(const QMap<QString, QList<int>> &map)
{
    QJsonObject object;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        QJsonArray values;
        for (const auto value : it.value()) {
            values.append(value);
        }
        object.insert(it.key(), values);
    }
    return object;
}

QMap<QString, QList<int>> intListMapFromJson(const QJsonObject &object)
{
    QMap<QString, QList<int>> map;
    for (auto it = object.begin(); it != object.end(); ++it) {
        QList<int> values;
        const auto array = it.value().toArray();
        values.reserve(array.size());
        for (const auto &value : array) {
            values.push_back(value.toInt());
        }
        map.insert(it.key(), values);
    }
    return map;
}

QJsonObject stringListMapToJson(const QMap<QString, QStringList> &map)
{
    QJsonObject object;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        QJsonArray values;
        for (const auto &value : it.value()) {
            values.append(value);
        }
        object.insert(it.key(), values);
    }
    return object;
}

QMap<QString, QStringList> stringListMapFromJson(const QJsonObject &object)
{
    QMap<QString, QStringList> map;
    for (auto it = object.begin(); it != object.end(); ++it) {
        QStringList values;
        const auto array = it.value().toArray();
        values.reserve(array.size());
        for (const auto &value : array) {
            values.push_back(value.toString());
        }
        map.insert(it.key(), values);
    }
    return map;
}

QJsonObject boolMapToJson(const QMap<QString, bool> &map)
{
    QJsonObject object;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        object.insert(it.key(), it.value());
    }
    return object;
}

QMap<QString, bool> boolMapFromJson(const QJsonObject &object)
{
    QMap<QString, bool> map;
    for (auto it = object.begin(); it != object.end(); ++it) {
        map.insert(it.key(), it.value().toBool(false));
    }
    return map;
}

QJsonObject dvrScheduleEntryToJson(const DvrScheduleEntry &entry)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), entry.id);
    object.insert(QStringLiteral("profileId"), entry.profileId);
    object.insert(QStringLiteral("channelId"), entry.channelId);
    object.insert(QStringLiteral("channelName"), entry.channelName);
    object.insert(QStringLiteral("streamUrl"), entry.streamUrl);
    object.insert(QStringLiteral("tvgId"), entry.tvgId);
    object.insert(QStringLiteral("title"), entry.title);
    object.insert(QStringLiteral("subTitle"), entry.subTitle);
    object.insert(QStringLiteral("description"), entry.description);
    object.insert(QStringLiteral("start"), dateTimeToJsonString(entry.start));
    object.insert(QStringLiteral("stop"), dateTimeToJsonString(entry.stop));
    object.insert(QStringLiteral("createdAt"), dateTimeToJsonString(entry.createdAt));
    return object;
}

DvrScheduleEntry dvrScheduleEntryFromJson(const QJsonObject &object)
{
    DvrScheduleEntry entry;
    entry.id = object.value(QStringLiteral("id")).toString().trimmed();
    entry.profileId = object.value(QStringLiteral("profileId")).toString().trimmed();
    entry.channelId = object.value(QStringLiteral("channelId")).toInt(entry.channelId);
    entry.channelName = object.value(QStringLiteral("channelName")).toString();
    entry.streamUrl = object.value(QStringLiteral("streamUrl")).toString();
    entry.tvgId = object.value(QStringLiteral("tvgId")).toString();
    entry.title = object.value(QStringLiteral("title")).toString();
    entry.subTitle = object.value(QStringLiteral("subTitle")).toString();
    entry.description = object.value(QStringLiteral("description")).toString();
    entry.start = dateTimeFromJsonValue(object.value(QStringLiteral("start")));
    entry.stop = dateTimeFromJsonValue(object.value(QStringLiteral("stop")));
    entry.createdAt = dateTimeFromJsonValue(object.value(QStringLiteral("createdAt")));
    return entry;
}

} // namespace

QJsonObject toJson(const ServerProfile &profile)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), guidToString(profile.id));
    object.insert(QStringLiteral("name"), profile.name);
    object.insert(QStringLiteral("type"), profileTypeToInt(profile.type));
    object.insert(QStringLiteral("xtreamBaseUrl"), profile.xtreamBaseUrl);
    object.insert(QStringLiteral("xtreamUsername"), profile.xtreamUsername);
    object.insert(QStringLiteral("xtreamPassword"), profile.xtreamPassword);
    object.insert(QStringLiteral("xtreamServerTimezone"), profile.xtreamServerTimezone);
    object.insert(QStringLiteral("m3UUrl"), profile.m3uUrl);
    object.insert(QStringLiteral("m3UFilePath"), profile.m3uFilePath);
    object.insert(QStringLiteral("xmltvUrl"), profile.xmltvUrl);
    object.insert(
        QStringLiteral("autoRefreshIntervalHours"),
        normalizeAutoRefreshIntervalHours(profile.autoRefreshIntervalHours));
    object.insert(QStringLiteral("lastRefreshed"), dateTimeToJsonString(profile.lastRefreshed));
    object.insert(QStringLiteral("isActive"), profile.isActive);
    return object;
}

ServerProfile serverProfileFromJson(const QJsonObject &object)
{
    ServerProfile profile;
    profile.id = parseGuid(object.value(QStringLiteral("id")).toString());
    if (profile.id.isNull()) {
        profile.id = QUuid::createUuid();
    }

    profile.name = object.value(QStringLiteral("name")).toString();
    profile.type = profileTypeFromJsonValue(object.value(QStringLiteral("type")));
    profile.xtreamBaseUrl = object.value(QStringLiteral("xtreamBaseUrl")).toString();
    profile.xtreamUsername = object.value(QStringLiteral("xtreamUsername")).toString();
    profile.xtreamPassword = object.value(QStringLiteral("xtreamPassword")).toString();
    profile.xtreamServerTimezone = object.value(QStringLiteral("xtreamServerTimezone")).toString().trimmed();
    profile.m3uUrl = object.value(QStringLiteral("m3UUrl")).toString();
    profile.m3uFilePath = object.value(QStringLiteral("m3UFilePath")).toString();
    profile.xmltvUrl = object.value(QStringLiteral("xmltvUrl")).toString();
    profile.autoRefreshIntervalHours = normalizeAutoRefreshIntervalHours(
        object.value(QStringLiteral("autoRefreshIntervalHours")).toInt(profile.autoRefreshIntervalHours));
    profile.lastRefreshed = dateTimeFromJsonValue(object.value(QStringLiteral("lastRefreshed")));
    profile.isActive = object.value(QStringLiteral("isActive")).toBool(false);
    return profile;
}

QJsonObject toJson(const SourceSummary &summary)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), guidToString(summary.id));
    object.insert(QStringLiteral("name"), summary.name);
    object.insert(QStringLiteral("type"), profileTypeToInt(summary.type));
    object.insert(
        QStringLiteral("autoRefreshIntervalHours"),
        normalizeAutoRefreshIntervalHours(summary.autoRefreshIntervalHours));
    object.insert(QStringLiteral("xtreamServerTimezone"), summary.xtreamServerTimezone);
    object.insert(QStringLiteral("lastRefreshed"), dateTimeToJsonString(summary.lastRefreshed));
    object.insert(QStringLiteral("groupCount"), std::max(0, summary.groupCount));
    object.insert(QStringLiteral("isActive"), summary.isActive);
    return object;
}

SourceSummary sourceSummaryFromJson(const QJsonObject &object)
{
    SourceSummary summary;
    summary.id = parseGuid(object.value(QStringLiteral("id")).toString());
    if (summary.id.isNull()) {
        summary.id = QUuid::createUuid();
    }

    summary.name = object.value(QStringLiteral("name")).toString();
    summary.type = profileTypeFromJsonValue(object.value(QStringLiteral("type")));
    summary.autoRefreshIntervalHours = normalizeAutoRefreshIntervalHours(
        object.value(QStringLiteral("autoRefreshIntervalHours")).toInt(summary.autoRefreshIntervalHours));
    summary.xtreamServerTimezone = object.value(QStringLiteral("xtreamServerTimezone")).toString().trimmed();
    summary.lastRefreshed = dateTimeFromJsonValue(object.value(QStringLiteral("lastRefreshed")));
    summary.groupCount = std::max(0, object.value(QStringLiteral("groupCount")).toInt(0));
    summary.isActive = object.value(QStringLiteral("isActive")).toBool(false);
    return summary;
}

QJsonObject toJson(const AppSettings &settings)
{
    QJsonObject object;
    if (settings.activeProfileId.has_value()) {
        object.insert(QStringLiteral("activeProfileId"), guidToString(settings.activeProfileId.value()));
    } else {
        object.insert(QStringLiteral("activeProfileId"), QJsonValue());
    }

    QJsonArray profiles;
    for (const auto &profile : settings.profiles) {
        profiles.append(toJson(profile));
    }

    object.insert(QStringLiteral("profiles"), profiles);
    object.insert(QStringLiteral("theme"), settings.theme);
    object.insert(QStringLiteral("lastSection"), settings.lastSection);
    object.insert(QStringLiteral("showOnTopModeIndicator"), settings.showOnTopModeIndicator);
    object.insert(QStringLiteral("preventDisplaySleep"), settings.preventDisplaySleep);
    object.insert(QStringLiteral("guidePreviewEnabled"), settings.guidePreviewEnabled);
    object.insert(QStringLiteral("overlayAutoHide"), settings.overlayAutoHide);
    object.insert(QStringLiteral("overlayAutoHideSeconds"), settings.overlayAutoHideSeconds);
    object.insert(QStringLiteral("guidePastHours"), normalizeGuideHours(settings.guidePastHours));
    object.insert(QStringLiteral("epgLookAheadHours"), normalizeGuideHours(settings.epgLookAheadHours));
    object.insert(QStringLiteral("autoRefreshEpg"), settings.autoRefreshEpg);
    object.insert(QStringLiteral("refreshIntervalMinutes"), settings.refreshIntervalMinutes);
    object.insert(
        QStringLiteral("playerWaitForStreamSeconds"),
        normalizePlayerWaitForStreamSeconds(settings.playerWaitForStreamSeconds));
    object.insert(QStringLiteral("playerDeinterlaceEnabled"), settings.playerDeinterlaceEnabled);
    object.insert(
        QStringLiteral("playerBufferSeconds"),
        normalizePlayerBufferSeconds(settings.playerBufferSeconds));
    object.insert(QStringLiteral("playerUserAgent"), settings.playerUserAgent);
    object.insert(QStringLiteral("timeshiftEnabled"), settings.timeshiftEnabled);
    object.insert(
        QStringLiteral("timeshiftWindowMinutes"),
        normalizeTimeshiftWindowMinutes(settings.timeshiftWindowMinutes));
    object.insert(
        QStringLiteral("timeshiftSegmentSeconds"),
        normalizeTimeshiftSegmentSeconds(settings.timeshiftSegmentSeconds));
    object.insert(QStringLiteral("timeshiftStorageDirectory"), settings.timeshiftStorageDirectory);
    object.insert(
        QStringLiteral("timeshiftMaxDiskGb"),
        normalizeTimeshiftMaxDiskGb(settings.timeshiftMaxDiskGb));
    object.insert(QStringLiteral("catchupEnabled"), settings.catchupEnabled);
    object.insert(QStringLiteral("catchupDefaultWindowHours"), settings.catchupDefaultWindowHours);
    object.insert(QStringLiteral("mpvDllPath"), settings.mpvDllPath);
    object.insert(QStringLiteral("mpvOptions"), stringMapToJson(settings.mpvOptions));
    object.insert(QStringLiteral("multiviewEnabled"), settings.multiviewEnabled);
    object.insert(
        QStringLiteral("multiviewMaxTiles"),
        normalizeMultiviewMaxTiles(settings.multiviewMaxTiles));
    object.insert(QStringLiteral("multiviewPreferHwdec"), settings.multiviewPreferHwdec);
    object.insert(
        QStringLiteral("multiviewRetainSelectionOnPromotion"),
        settings.multiviewRetainSelectionOnPromotion);
    object.insert(QStringLiteral("screenshotsDirectory"), settings.screenshotsDirectory);
    object.insert(QStringLiteral("recordingsDirectory"), settings.recordingsDirectory);
    object.insert(QStringLiteral("remuxRecordingsToMkv"), settings.remuxRecordingsToMkv);
    object.insert(QStringLiteral("minimizeToTrayOnMinimize"), settings.minimizeToTrayOnMinimize);
    object.insert(QStringLiteral("reopenMaximizedOnLaunch"), settings.reopenMaximizedOnLaunch);
    object.insert(QStringLiteral("dvrRecordingsDirectory"), settings.dvrRecordingsDirectory);
    object.insert(QStringLiteral("dvrRemuxToMkv"), settings.dvrRemuxToMkv);
    object.insert(QStringLiteral("dvrStartOffsetMinutes"), settings.dvrStartOffsetMinutes);
    object.insert(QStringLiteral("dvrEndOffsetMinutes"), settings.dvrEndOffsetMinutes);
    QJsonArray dvrSchedules;
    for (const auto &entry : settings.dvrSchedules) {
        dvrSchedules.push_back(dvrScheduleEntryToJson(entry));
    }
    object.insert(QStringLiteral("dvrSchedules"), dvrSchedules);
    object.insert(QStringLiteral("lastWatchedChannelId"), intMapToJson(settings.lastWatchedChannelId));
    object.insert(
        QStringLiteral("favoriteChannelIdsByProfile"),
        intListMapToJson(settings.favoriteChannelIdsByProfile));
    object.insert(QStringLiteral("hiddenGroupsByProfile"), stringListMapToJson(settings.hiddenGroupsByProfile));
    object.insert(QStringLiteral("groupOrderByProfile"), stringListMapToJson(settings.groupOrderByProfile));
    object.insert(
        QStringLiteral("hideUncheckedGroupsByProfile"),
        boolMapToJson(settings.hideUncheckedGroupsByProfile));
    return object;
}

AppSettings appSettingsFromJson(const QJsonObject &object)
{
    AppSettings settings;
    const auto activeProfileValue = object.value(QStringLiteral("activeProfileId"));
    if (activeProfileValue.isString()) {
        const auto parsed = parseGuid(activeProfileValue.toString());
        if (!parsed.isNull()) {
            settings.activeProfileId = parsed;
        }
    }

    const auto profileArray = object.value(QStringLiteral("profiles")).toArray();
    settings.profiles.reserve(profileArray.size());
    for (const auto &profileValue : profileArray) {
        settings.profiles.push_back(serverProfileFromJson(profileValue.toObject()));
    }

    settings.theme = object.value(QStringLiteral("theme")).toString(settings.theme);
    settings.lastSection = object.value(QStringLiteral("lastSection")).toString(settings.lastSection);
    settings.showOnTopModeIndicator =
        object.value(QStringLiteral("showOnTopModeIndicator")).toBool(settings.showOnTopModeIndicator);
    settings.preventDisplaySleep =
        object.value(QStringLiteral("preventDisplaySleep")).toBool(settings.preventDisplaySleep);
    settings.guidePreviewEnabled =
        object.value(QStringLiteral("guidePreviewEnabled")).toBool(settings.guidePreviewEnabled);
    settings.overlayAutoHide =
        object.value(QStringLiteral("overlayAutoHide")).toBool(settings.overlayAutoHide);
    settings.overlayAutoHideSeconds =
        std::max(1, object.value(QStringLiteral("overlayAutoHideSeconds")).toInt(settings.overlayAutoHideSeconds));
    settings.guidePastHours =
        normalizeGuideHours(object.value(QStringLiteral("guidePastHours")).toInt(settings.guidePastHours));
    settings.epgLookAheadHours =
        normalizeGuideHours(object.value(QStringLiteral("epgLookAheadHours")).toInt(settings.epgLookAheadHours));
    settings.autoRefreshEpg =
        object.value(QStringLiteral("autoRefreshEpg")).toBool(settings.autoRefreshEpg);
    settings.refreshIntervalMinutes =
        object.value(QStringLiteral("refreshIntervalMinutes")).toInt(settings.refreshIntervalMinutes);
    settings.playerWaitForStreamSeconds = normalizePlayerWaitForStreamSeconds(
        object.value(QStringLiteral("playerWaitForStreamSeconds")).toDouble(settings.playerWaitForStreamSeconds));
    settings.playerDeinterlaceEnabled = object.value(QStringLiteral("playerDeinterlaceEnabled"))
                                            .toBool(settings.playerDeinterlaceEnabled);
    settings.playerBufferSeconds = normalizePlayerBufferSeconds(
        object.value(QStringLiteral("playerBufferSeconds")).toDouble(settings.playerBufferSeconds));
    settings.playerUserAgent = object.value(QStringLiteral("playerUserAgent")).toString().trimmed();
    settings.timeshiftEnabled = object.value(QStringLiteral("timeshiftEnabled")).toBool(settings.timeshiftEnabled);
    settings.timeshiftWindowMinutes = normalizeTimeshiftWindowMinutes(
        object.value(QStringLiteral("timeshiftWindowMinutes")).toInt(settings.timeshiftWindowMinutes));
    settings.timeshiftSegmentSeconds = normalizeTimeshiftSegmentSeconds(
        object.value(QStringLiteral("timeshiftSegmentSeconds")).toInt(settings.timeshiftSegmentSeconds));
    settings.timeshiftStorageDirectory = object.value(QStringLiteral("timeshiftStorageDirectory")).toString();
    settings.timeshiftMaxDiskGb = normalizeTimeshiftMaxDiskGb(
        object.value(QStringLiteral("timeshiftMaxDiskGb")).toInt(settings.timeshiftMaxDiskGb));
    const auto catchupEnabledValue = object.value(QStringLiteral("catchupEnabled"));
    settings.catchupEnabled = catchupEnabledValue.isUndefined() ? settings.catchupEnabled : catchupEnabledValue.toBool();
    settings.catchupDefaultWindowHours =
        std::max(1, object.value(QStringLiteral("catchupDefaultWindowHours")).toInt(settings.catchupDefaultWindowHours));
    settings.mpvDllPath = object.value(QStringLiteral("mpvDllPath")).toString();
    settings.mpvOptions = stringMapFromJson(object.value(QStringLiteral("mpvOptions")).toObject());
    settings.multiviewEnabled =
        object.value(QStringLiteral("multiviewEnabled")).toBool(settings.multiviewEnabled);
    settings.multiviewMaxTiles = normalizeMultiviewMaxTiles(
        object.value(QStringLiteral("multiviewMaxTiles")).toInt(settings.multiviewMaxTiles));
    settings.multiviewPreferHwdec = object.value(QStringLiteral("multiviewPreferHwdec"))
                                        .toBool(settings.multiviewPreferHwdec);
    settings.multiviewRetainSelectionOnPromotion = object.value(
        QStringLiteral("multiviewRetainSelectionOnPromotion"))
                                                        .toBool(settings.multiviewRetainSelectionOnPromotion);
    settings.screenshotsDirectory = object.value(QStringLiteral("screenshotsDirectory")).toString();
    settings.recordingsDirectory = object.value(QStringLiteral("recordingsDirectory")).toString();
    const auto remuxVal = object.value(QStringLiteral("remuxRecordingsToMkv"));
    settings.remuxRecordingsToMkv = remuxVal.isUndefined() ? true : remuxVal.toBool();
    settings.minimizeToTrayOnMinimize =
        object.value(QStringLiteral("minimizeToTrayOnMinimize")).toBool(settings.minimizeToTrayOnMinimize);
    settings.reopenMaximizedOnLaunch =
        object.value(QStringLiteral("reopenMaximizedOnLaunch")).toBool(settings.reopenMaximizedOnLaunch);
    settings.dvrRecordingsDirectory = object.value(QStringLiteral("dvrRecordingsDirectory")).toString();
    const auto dvrRemuxVal = object.value(QStringLiteral("dvrRemuxToMkv"));
    settings.dvrRemuxToMkv = dvrRemuxVal.isUndefined() ? true : dvrRemuxVal.toBool();
    settings.dvrStartOffsetMinutes =
        object.value(QStringLiteral("dvrStartOffsetMinutes")).toInt(settings.dvrStartOffsetMinutes);
    settings.dvrEndOffsetMinutes =
        object.value(QStringLiteral("dvrEndOffsetMinutes")).toInt(settings.dvrEndOffsetMinutes);
    const auto dvrSchedules = object.value(QStringLiteral("dvrSchedules")).toArray();
    settings.dvrSchedules.clear();
    settings.dvrSchedules.reserve(dvrSchedules.size());
    for (const auto &value : dvrSchedules) {
        if (!value.isObject()) {
            continue;
        }

        const auto entry = dvrScheduleEntryFromJson(value.toObject());
        if (entry.id.isEmpty()
            || entry.profileId.isEmpty()
            || entry.channelId < 0
            || entry.streamUrl.trimmed().isEmpty()
            || !entry.start.isValid()
            || !entry.stop.isValid()
            || entry.stop <= entry.start) {
            continue;
        }
        settings.dvrSchedules.push_back(entry);
    }
    settings.lastWatchedChannelId =
        intMapFromJson(object.value(QStringLiteral("lastWatchedChannelId")).toObject());
    settings.favoriteChannelIdsByProfile = intListMapFromJson(
        object.value(QStringLiteral("favoriteChannelIdsByProfile")).toObject());
    settings.hiddenGroupsByProfile = stringListMapFromJson(
        object.value(QStringLiteral("hiddenGroupsByProfile")).toObject());
    settings.groupOrderByProfile = stringListMapFromJson(
        object.value(QStringLiteral("groupOrderByProfile")).toObject());
    settings.hideUncheckedGroupsByProfile = boolMapFromJson(
        object.value(QStringLiteral("hideUncheckedGroupsByProfile")).toObject());

    return settings;
}

} // namespace OKILTV::Core
