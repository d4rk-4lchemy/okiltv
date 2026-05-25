#include "profilesmodel.h"

#include <QDateTime>

namespace OKILTV::App {

using namespace Core;

ProfilesModel::ProfilesModel(SettingsManager *settings, QObject *parent)
    : QAbstractListModel(parent)
    , m_settings(settings)
{
}

int ProfilesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_settings->sourceSummaries().size());
}

QVariant ProfilesModel::data(const QModelIndex &index, const int role) const
{
    const auto summaries = m_settings->sourceSummaries();
    if (!index.isValid() || index.row() < 0 || index.row() >= summaries.size()) {
        return {};
    }

    const auto &profile = summaries.at(index.row());
    switch (role) {
    case IdRole:
        return guidToString(profile.id);
    case NameRole:
        return profile.name;
    case TypeRole:
        return static_cast<int>(profile.type);
    case TypeLabelRole:
        return profileTypeLabel(profile.type);
    case IsActiveRole:
        return profile.isActive;
    case LastRefreshedRole:
        return profile.lastRefreshed.toUTC().toString(Qt::ISODateWithMs);
    case GroupCountRole:
        return profile.groupCount;
    default:
        return {};
    }
}

QHash<int, QByteArray> ProfilesModel::roleNames() const
{
    return {
        { IdRole, "id" },
        { NameRole, "name" },
        { TypeRole, "type" },
        { TypeLabelRole, "typeLabel" },
        { IsActiveRole, "isActive" },
        { LastRefreshedRole, "lastRefreshed" },
        { GroupCountRole, "groupCount" }
    };
}

QString ProfilesModel::activeProfileId() const
{
    const auto &activeProfileId = m_settings->current().activeProfileId;
    return activeProfileId.has_value() ? guidToString(*activeProfileId) : QString {};
}

QVariantMap ProfilesModel::get(const int row) const
{
    const auto summaries = m_settings->sourceSummaries();
    if (row < 0 || row >= summaries.size()) {
        return {};
    }
    const auto summary = summaries.at(row);
    auto result = toVariantMap(summary);
    const auto detail = m_settings->profileDetailById(summary.id);
    if (detail.has_value()) {
        const auto detailMap = toVariantMap(detail.value());
        for (auto it = detailMap.cbegin(); it != detailMap.cend(); ++it) {
            result.insert(it.key(), it.value());
        }
    }
    return result;
}

QString ProfilesModel::addXtreamProfile(
    const QString &name,
    const QString &baseUrl,
    const QString &username,
    const QString &password,
    const QString &xmltvUrl,
    const int autoRefreshIntervalHours)
{
    ServerProfile profile;
    profile.name = name.trimmed();
    profile.type = ProfileType::Xtream;
    profile.xtreamBaseUrl = baseUrl.trimmed();
    profile.xtreamUsername = username.trimmed();
    profile.xtreamPassword = password.trimmed();
    profile.xmltvUrl = xmltvUrl.trimmed();
    profile.autoRefreshIntervalHours = normalizeAutoRefreshIntervalHours(autoRefreshIntervalHours);

    if (!m_settings->addProfile(profile)) {
        return {};
    }
    reload();
    return guidToString(profile.id);
}

QString ProfilesModel::addM3uUrlProfile(
    const QString &name,
    const QString &m3uUrl,
    const QString &xmltvUrl,
    const int autoRefreshIntervalHours)
{
    ServerProfile profile;
    profile.name = name.trimmed();
    profile.type = ProfileType::M3UUrl;
    profile.m3uUrl = m3uUrl.trimmed();
    profile.xmltvUrl = xmltvUrl.trimmed();
    profile.autoRefreshIntervalHours = normalizeAutoRefreshIntervalHours(autoRefreshIntervalHours);

    if (!m_settings->addProfile(profile)) {
        return {};
    }
    reload();
    return guidToString(profile.id);
}

QString ProfilesModel::addM3uFileProfile(const QString &name, const QString &filePath, const QString &xmltvUrl)
{
    ServerProfile profile;
    profile.name = name.trimmed();
    profile.type = ProfileType::M3UFile;
    profile.m3uFilePath = filePath.trimmed();
    profile.xmltvUrl = xmltvUrl.trimmed();

    if (!m_settings->addProfile(profile)) {
        return {};
    }
    reload();
    return guidToString(profile.id);
}

bool ProfilesModel::replaceProfile(const QString &profileId, const QVariantMap &changes)
{
    const auto parsedId = parseGuid(profileId);
    const auto row = indexOfProfile(parsedId);
    if (row < 0) {
        return false;
    }

    const auto summary = m_settings->sourceSummaries().at(row);
    auto profile = m_settings->profileById(summary.id).value_or(ServerProfile {});
    profile.id = summary.id;
    profile.name = summary.name;
    profile.type = summary.type;
    profile.autoRefreshIntervalHours = summary.autoRefreshIntervalHours;
    profile.lastRefreshed = summary.lastRefreshed;
    profile.xtreamServerTimezone = summary.xtreamServerTimezone;
    profile.isActive = summary.isActive;
    if (changes.contains(QStringLiteral("name"))) {
        profile.name = changes.value(QStringLiteral("name")).toString().trimmed();
    }
    if (changes.contains(QStringLiteral("type"))) {
        auto rawType = static_cast<int>(profile.type);
        const auto parsedType = changes.value(QStringLiteral("type")).toInt();
        if (parsedType == static_cast<int>(ProfileType::Xtream)
            || parsedType == static_cast<int>(ProfileType::M3UUrl)
            || parsedType == static_cast<int>(ProfileType::M3UFile)) {
            rawType = parsedType;
        }
        if (rawType == static_cast<int>(ProfileType::M3UUrl)) {
            profile.type = ProfileType::M3UUrl;
        } else if (rawType == static_cast<int>(ProfileType::M3UFile)) {
            profile.type = ProfileType::M3UFile;
        } else {
            profile.type = ProfileType::Xtream;
        }
    }
    if (changes.contains(QStringLiteral("xtreamBaseUrl"))) {
        profile.xtreamBaseUrl = changes.value(QStringLiteral("xtreamBaseUrl")).toString().trimmed();
    }
    if (changes.contains(QStringLiteral("xtreamUsername"))) {
        profile.xtreamUsername = changes.value(QStringLiteral("xtreamUsername")).toString().trimmed();
    }
    if (changes.contains(QStringLiteral("xtreamPassword"))) {
        profile.xtreamPassword = changes.value(QStringLiteral("xtreamPassword")).toString().trimmed();
    }
    if (changes.contains(QStringLiteral("xtreamServerTimezone"))) {
        profile.xtreamServerTimezone = changes.value(QStringLiteral("xtreamServerTimezone")).toString().trimmed();
    }
    if (changes.contains(QStringLiteral("m3UUrl"))) {
        profile.m3uUrl = changes.value(QStringLiteral("m3UUrl")).toString().trimmed();
    }
    if (changes.contains(QStringLiteral("m3UFilePath"))) {
        profile.m3uFilePath = changes.value(QStringLiteral("m3UFilePath")).toString().trimmed();
    }
    if (changes.contains(QStringLiteral("xmltvUrl"))) {
        profile.xmltvUrl = changes.value(QStringLiteral("xmltvUrl")).toString().trimmed();
    }
    if (changes.contains(QStringLiteral("autoRefreshIntervalHours"))) {
        profile.autoRefreshIntervalHours = normalizeAutoRefreshIntervalHours(
            changes.value(QStringLiteral("autoRefreshIntervalHours")).toInt());
    }
    if (changes.contains(QStringLiteral("lastRefreshed"))) {
        auto parsed = QDateTime::fromString(
            changes.value(QStringLiteral("lastRefreshed")).toString(),
            Qt::ISODateWithMs);
        if (!parsed.isValid()) {
            parsed = QDateTime::fromString(
                changes.value(QStringLiteral("lastRefreshed")).toString(),
                Qt::ISODate);
        }
        if (parsed.isValid()) {
            profile.lastRefreshed = parsed.toUTC();
        }
    }

    if (!m_settings->replaceProfile(summary.id, profile)) {
        return false;
    }
    emit dataChanged(index(row, 0), index(row, 0));
    return true;
}

bool ProfilesModel::removeProfile(const QString &profileId)
{
    const auto parsedId = parseGuid(profileId);
    const auto row = indexOfProfile(parsedId);
    if (row < 0) {
        return false;
    }

    const auto oldActive = activeProfileId();
    if (!m_settings->removeProfile(parsedId)) {
        return false;
    }
    if (oldActive != activeProfileId()) {
        emit activeProfileIdChanged();
    }
    reload();
    return true;
}

bool ProfilesModel::selectProfile(const QString &profileId)
{
    const auto parsedId = parseGuid(profileId);
    const auto row = indexOfProfile(parsedId);
    if (row < 0) {
        return false;
    }

    const auto oldActive = activeProfileId();
    m_settings->setActiveProfileId(parsedId);
    m_settings->save();
    emit dataChanged(index(0, 0), index(rowCount() - 1, 0));
    if (oldActive != activeProfileId()) {
        emit activeProfileIdChanged();
    }
    emit profileSelectionRequested(profileId);
    return true;
}

void ProfilesModel::reload()
{
    beginResetModel();
    endResetModel();
    emit activeProfileIdChanged();
}

int ProfilesModel::indexOfProfile(const QUuid &profileId) const
{
    const auto summaries = m_settings->sourceSummaries();
    for (auto index = 0; index < summaries.size(); ++index) {
        if (summaries.at(index).id == profileId) {
            return index;
        }
    }

    return -1;
}

} // namespace OKILTV::App
