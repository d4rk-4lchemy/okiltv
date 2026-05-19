#include "profilesmodel.h"

namespace OKILTV::App {

using namespace Core;

ProfilesModel::ProfilesModel(SettingsManager *settings, QObject *parent)
    : QAbstractListModel(parent)
    , m_settings(settings)
{
}

int ProfilesModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_settings->current().profiles.size());
}

QVariant ProfilesModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_settings->current().profiles.size()) {
        return {};
    }

    const auto &profile = m_settings->current().profiles.at(index.row());
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
        { LastRefreshedRole, "lastRefreshed" }
    };
}

QString ProfilesModel::activeProfileId() const
{
    const auto &activeProfileId = m_settings->current().activeProfileId;
    return activeProfileId.has_value() ? guidToString(*activeProfileId) : QString {};
}

QVariantMap ProfilesModel::get(const int row) const
{
    if (row < 0 || row >= m_settings->current().profiles.size()) {
        return {};
    }

    return toVariantMap(m_settings->current().profiles.at(row));
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

    beginInsertRows({}, rowCount(), rowCount());
    m_settings->current().profiles.push_back(profile);
    endInsertRows();
    m_settings->save();
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

    beginInsertRows({}, rowCount(), rowCount());
    m_settings->current().profiles.push_back(profile);
    endInsertRows();
    m_settings->save();
    return guidToString(profile.id);
}

QString ProfilesModel::addM3uFileProfile(const QString &name, const QString &filePath, const QString &xmltvUrl)
{
    ServerProfile profile;
    profile.name = name.trimmed();
    profile.type = ProfileType::M3UFile;
    profile.m3uFilePath = filePath.trimmed();
    profile.xmltvUrl = xmltvUrl.trimmed();

    beginInsertRows({}, rowCount(), rowCount());
    m_settings->current().profiles.push_back(profile);
    endInsertRows();
    m_settings->save();
    return guidToString(profile.id);
}

bool ProfilesModel::replaceProfile(const QString &profileId, const QVariantMap &changes)
{
    const auto parsedId = parseGuid(profileId);
    const auto row = indexOfProfile(parsedId);
    if (row < 0) {
        return false;
    }

    auto &profile = m_settings->current().profiles[row];
    if (changes.contains(QStringLiteral("name"))) {
        profile.name = changes.value(QStringLiteral("name")).toString().trimmed();
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

    m_settings->save();
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

    beginRemoveRows({}, row, row);
    m_settings->current().profiles.removeAt(row);
    endRemoveRows();

    const auto &activeProfileId = m_settings->current().activeProfileId;
    if (activeProfileId.has_value() && *activeProfileId == parsedId) {
        m_settings->setActiveProfileId(std::nullopt);
        emit activeProfileIdChanged();
    }

    m_settings->save();
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
    for (auto index = 0; index < m_settings->current().profiles.size(); ++index) {
        if (m_settings->current().profiles.at(index).id == profileId) {
            return index;
        }
    }

    return -1;
}

} // namespace OKILTV::App
