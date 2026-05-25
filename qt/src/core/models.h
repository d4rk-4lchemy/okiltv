#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVariantList>

#include <optional>

namespace OKILTV::Core {

enum class ProfileType
{
    Xtream = 0,
    M3UUrl = 1,
    M3UFile = 2
};

enum class ChannelSource
{
    Xtream,
    M3U
};

struct ServerProfile
{
    QUuid id { QUuid::createUuid() };
    QString name;
    ProfileType type { ProfileType::Xtream };

    QString xtreamBaseUrl;
    QString xtreamUsername;
    QString xtreamPassword;
    QString xtreamServerTimezone;

    QString m3uUrl;
    QString m3uFilePath;

    QString xmltvUrl;
    int autoRefreshIntervalHours { 24 };

    QDateTime lastRefreshed;
    bool isActive { false };
};

struct SourceSummary
{
    QUuid id { QUuid::createUuid() };
    QString name;
    ProfileType type { ProfileType::Xtream };
    int autoRefreshIntervalHours { 24 };
    QString xtreamServerTimezone;
    QDateTime lastRefreshed;
    int groupCount { 0 };
    bool isActive { false };
};

struct ChannelCategory
{
    QString id;
    QString name;
    int parentId { 0 };
};

struct Channel
{
    int id { 0 };
    QString name;
    QString streamUrl;
    QString categoryId;
    QString categoryName;
    QString tvgId;
    QString tvgName;
    QString iconUrl;
    QString cachedIconPath;
    ChannelSource source { ChannelSource::M3U };
    int sortOrder { 0 };
    QUuid profileId;
    bool catchupSupported { false };
    int catchupWindowHours { 0 };
    QString catchupMode;
    QString catchupSourceTemplate;
};

struct EpgEntry
{
    QString channelId;
    QString title;
    QString description;
    QString episodeNum;
    QDateTime start;
    QDateTime stop;
    QString subTitle;
};

struct DvrScheduleEntry
{
    QString id;
    QString profileId;
    int channelId { -1 };
    QString channelName;
    QString streamUrl;
    QString tvgId;
    QString title;
    QString subTitle;
    QString description;
    QDateTime start;
    QDateTime stop;
    QDateTime createdAt;
};

struct AppSettings
{
    std::optional<QUuid> activeProfileId;
    QList<ServerProfile> profiles;

    QString theme { QStringLiteral("Dark") };
    QString lastSection { QStringLiteral("live") };
    bool showOnTopModeIndicator { true };
    bool preventDisplaySleep { true };
    bool guidePreviewEnabled { true };
    bool overlayAutoHide { true };
    int overlayAutoHideSeconds { 3 };
    int guidePastHours { 6 };
    int epgLookAheadHours { 24 };
    bool autoRefreshEpg { true };
    int refreshIntervalMinutes { 360 };
    double playerWaitForStreamSeconds { 5.0 };
    bool playerDeinterlaceEnabled { true };
    double playerBufferSeconds { 3.0 };
    QString playerUserAgent;
    bool timeshiftEnabled { false };
    int timeshiftWindowMinutes { 90 };
    int timeshiftSegmentSeconds { 2 };
    QString timeshiftStorageDirectory;
    int timeshiftMaxDiskGb { 8 };
    bool catchupEnabled { true };
    int catchupDefaultWindowHours { 24 };

    QString mpvDllPath;
    QMap<QString, QString> mpvOptions;
    bool multiviewEnabled { true };
    int multiviewMaxTiles { 4 };
    bool multiviewPreferHwdec { true };
    bool multiviewRetainSelectionOnPromotion { false };

    QString screenshotsDirectory;
    QString recordingsDirectory;
    bool remuxRecordingsToMkv { true };
    bool minimizeToTrayOnMinimize { true };
    bool reopenMaximizedOnLaunch { false };
    QString dvrRecordingsDirectory;
    bool dvrRemuxToMkv { true };
    int dvrStartOffsetMinutes { 2 };
    int dvrEndOffsetMinutes { 2 };
    QList<DvrScheduleEntry> dvrSchedules;

    QMap<QString, int> lastWatchedChannelId;
    QMap<QString, QList<int>> favoriteChannelIdsByProfile;
    QMap<QString, QStringList> hiddenGroupsByProfile;
    QMap<QString, QStringList> groupOrderByProfile;
    QMap<QString, bool> hideUncheckedGroupsByProfile;
};

QString guidToString(const QUuid &value);
QUuid parseGuid(QStringView value);

QString profileTypeLabel(ProfileType type);
QString channelSourceToString(ChannelSource source);
ChannelSource channelSourceFromString(const QString &value);
QString ungroupedCategoryId();
QString normalizeChannelCategoryId(const QString &value);
QString displayNameForCategoryId(const QString &value);
int normalizeAutoRefreshIntervalHours(int value);
int normalizeGuideHours(int value);
double normalizePlayerWaitForStreamSeconds(double value);
double normalizePlayerBufferSeconds(double value);
int normalizeTimeshiftWindowMinutes(int value);
int normalizeTimeshiftSegmentSeconds(int value);
int normalizeTimeshiftMaxDiskGb(int value);
int normalizeMultiviewMaxTiles(int value);

QJsonObject toJson(const ServerProfile &profile);
ServerProfile serverProfileFromJson(const QJsonObject &object);
QJsonObject toJson(const SourceSummary &summary);
SourceSummary sourceSummaryFromJson(const QJsonObject &object);

QJsonObject toJson(const AppSettings &settings);
AppSettings appSettingsFromJson(const QJsonObject &object);

QVariantMap toVariantMap(const ServerProfile &profile);
QVariantMap toVariantMap(const SourceSummary &summary);
QVariantMap toVariantMap(const ChannelCategory &category);
QVariantMap toVariantMap(const Channel &channel);
QVariantMap toVariantMap(const EpgEntry &entry);
QVariantList toVariantList(const QList<EpgEntry> &entries);

bool epgEntryIsNow(const EpgEntry &entry);
double epgEntryProgressPercent(const EpgEntry &entry);
QString epgEntryTimeRange(const EpgEntry &entry);
QString epgEntryStartTimeLabel(const EpgEntry &entry);

} // namespace OKILTV::Core

Q_DECLARE_METATYPE(OKILTV::Core::AppSettings)
Q_DECLARE_METATYPE(OKILTV::Core::Channel)
Q_DECLARE_METATYPE(OKILTV::Core::ChannelCategory)
Q_DECLARE_METATYPE(OKILTV::Core::DvrScheduleEntry)
Q_DECLARE_METATYPE(OKILTV::Core::EpgEntry)
Q_DECLARE_METATYPE(OKILTV::Core::ServerProfile)
