#include "settingsmanager.h"

#include "appdatapaths.h"
#include "secretprotection.h"
#include "database_service.h"
#include <stdexcept>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

#include <algorithm>

namespace OKILTV::Core {

namespace {

constexpr auto kProfilesMigratedKey = "profilesMigratedToSourceStore";

} // namespace

SettingsManager::SettingsManager(QString settingsFilePath)
    : m_settingsFilePath(settingsFilePath.isEmpty() ? AppDataPaths::settingsFile() : std::move(settingsFilePath))
    , m_sourceStore(QFileInfo(m_settingsFilePath).dir().filePath(QStringLiteral("source-summaries.json")),
                    QFileInfo(m_settingsFilePath).dir().filePath(QStringLiteral("sources")))
{
}

void SettingsManager::load()
{
    m_lastLoadError.clear();
    m_unavailableProtectedSettings = {};
    m_profileDetailCache.clear();

    AppSettings parsedSettings;
    QFile file(m_settingsFilePath);
    if (!file.exists()) {
        parsedSettings = {};
    } else if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("Cannot read saved settings; original data was preserved.");
    } else {
        const auto bytes = file.readAll();
        // Migration below replaces this file with QSaveFile. An open reader
        // prevents that atomic replacement on Windows.
        file.close();
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            backupInvalidSettingsFile();
            m_lastLoadError = QStringLiteral("Invalid settings JSON was preserved in an encrypted backup; restored readable source files and default settings.");
        } else {
            auto root = document.object();
            m_unavailableProtectedSettings = root.value(QStringLiteral("unavailableProtectedSettings")).toArray();
            // A temporarily locked keyring must recover automatically after unlock.
            // Never replace new settings created on another account with old ones.
            if (!root.contains(QStringLiteral("protectedSettings"))) {
                for (qsizetype index = m_unavailableProtectedSettings.size(); index > 0; --index) {
                    const auto stored = m_unavailableProtectedSettings.at(index - 1).toString();
                    if (!isProtectedSecret(stored)) continue;
                    try {
                        if (!QJsonDocument::fromJson(unprotectSecret(stored).toUtf8()).isObject()) continue;
                        root.insert(QStringLiteral("protectedSettings"), stored);
                        m_unavailableProtectedSettings.removeAt(index - 1);
                        break;
                    } catch (const std::exception &) {
                        // Keep the opaque recovery copy untouched; try an older copy.
                        continue;
                    }
                }
            }
            if (root.contains(QStringLiteral("protectedSettings"))) {
                const auto stored = root.value(QStringLiteral("protectedSettings")).toString();
                if (!isProtectedSecret(stored)) throw std::runtime_error("Invalid protected settings.");
                try {
                    const auto secrets = QJsonDocument::fromJson(unprotectSecret(stored).toUtf8());
                    if (!secrets.isObject()) throw std::runtime_error("Invalid protected settings data.");
                    const auto object = secrets.object();
                    for (auto it = object.begin(); it != object.end(); ++it) root.insert(it.key(), it.value());
                } catch (const std::exception &) {
                    if (!m_unavailableProtectedSettings.contains(stored)) m_unavailableProtectedSettings.append(stored);
                    m_lastLoadError = QStringLiteral("Protected DVR and player settings cannot be unlocked on this account. Original data has been preserved.");
                }
            }
            parsedSettings = appSettingsFromJson(root);
        }
    }

    m_current = parsedSettings;
    auto migratedFlag = false;
    {
        QFile settingsFile(m_settingsFilePath);
        if (settingsFile.exists() && settingsFile.open(QIODevice::ReadOnly)) {
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(settingsFile.readAll(), &parseError);
            if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                migratedFlag = document.object().value(QLatin1String(kProfilesMigratedKey)).toBool(false);
            }
        }
    }

    if (!migratedFlag && !m_current.profiles.isEmpty()) {
        migrateLegacyProfilesIfNeeded(m_current);
    }

    m_sourceSummaries = m_sourceStore.loadSummaries();
    if (m_sourceSummaries.isEmpty() && !m_current.profiles.isEmpty()) {
        // Fallback path for legacy state that may not have been migrated.
        migrateLegacyProfilesIfNeeded(m_current);
        m_sourceSummaries = m_sourceStore.loadSummaries();
    }

    // Protect orphan files too, but never overwrite already protected sources
    // when their original account/keyring is unavailable.
    m_sourceStore.migrateLegacyDetails();
    for (const auto &summary : m_sourceSummaries) {
        if (!m_sourceStore.detailIsProtected(summary.id)) {
            throw std::runtime_error("A saved source is missing; migration stopped without removing legacy settings.");
        }
    }
    m_current.profiles.clear();
    rebuildSummaryMirrorFromSourceSummaries();
    syncProfileActivityFlagsAndMirror();

    if (m_current.activeProfileId.has_value()) {
        const auto hasActive = std::any_of(
            m_sourceSummaries.cbegin(),
            m_sourceSummaries.cend(),
            [this](const SourceSummary &summary) {
                return summary.id == m_current.activeProfileId.value();
            });
        if (!hasActive) {
            m_current.activeProfileId = std::nullopt;
            syncProfileActivityFlagsAndMirror();
            save();
        }
    }
    const auto backups = QFileInfo(m_settingsFilePath).dir().entryInfoList(
        { QFileInfo(m_settingsFilePath).fileName() + QStringLiteral(".invalid-*.bak") }, QDir::Files);
    for (const auto &backupInfo : backups) {
        QFile backup(backupInfo.filePath());
        if (!backup.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read legacy settings backup.");
        const auto bytes = backup.readAll();
        backup.close();
        if (QJsonDocument::fromJson(bytes).object().contains(QStringLiteral("protectedBackup"))) continue;
        const auto encoded = QString::fromLatin1(bytes.toBase64());
        const auto encrypted = protectSecret(encoded);
        if (unprotectSecret(encrypted) != encoded) throw std::runtime_error("Backup protection verification failed.");
        QSaveFile output(backupInfo.filePath());
        const auto payload = QJsonDocument(QJsonObject { { QStringLiteral("protectedBackup"), encrypted } }).toJson();
        if (!output.open(QIODevice::WriteOnly)
            || !output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
            || output.write(payload) != payload.size() || !output.commit()) {
            throw std::runtime_error("Cannot protect legacy settings backup.");
        }
    }
    save(); // Complete legacy migration only after all source details are durable.
    if (!m_lastSaveError.isEmpty()) throw std::runtime_error(m_lastSaveError.toStdString());
}

QJsonObject SettingsManager::channelTrackPreferences(const QString &profileId, const QString &channelKey) const
{
    return m_current.channelTrackPreferences.value(profileId).toObject().value(channelKey).toObject();
}

void SettingsManager::setChannelTrackPreference(const QString &profileId, const QString &channelKey,
                                               const QString &type, const QJsonObject &preference)
{
    if (parseGuid(profileId).isNull() || channelKey.isEmpty()
        || (type != QLatin1String("audio") && type != QLatin1String("sub"))) {
        return;
    }
    auto profile = m_current.channelTrackPreferences.value(profileId).toObject();
    auto channel = profile.value(channelKey).toObject();
    if (channel.value(type).toObject() == preference) {
        return;
    }
    if (preference.isEmpty()) {
        channel.remove(type);
    } else {
        channel.insert(type, preference);
    }
    if (channel.isEmpty()) {
        profile.remove(channelKey);
    } else {
        profile.insert(channelKey, channel);
    }
    if (profile.isEmpty()) {
        m_current.channelTrackPreferences.remove(profileId);
    } else {
        m_current.channelTrackPreferences.insert(profileId, profile);
    }
    save();
}

void SettingsManager::save() const
{
    m_lastSaveError.clear();
    const QFileInfo fileInfo(m_settingsFilePath);
    if (!QDir().mkpath(fileInfo.absolutePath())) {
        m_lastSaveError =
            QStringLiteral("Failed to create settings directory: %1").arg(fileInfo.absolutePath());
        return;
    }

    QSaveFile file(m_settingsFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_lastSaveError =
            QStringLiteral("Failed to open settings file for write: %1").arg(file.errorString());
        return;
    }

    auto settingsToPersist = m_current;
    settingsToPersist.profiles.clear();

    auto root = toJson(settingsToPersist);
    root.insert(QLatin1String(kProfilesMigratedKey), true);

    try {
        if (!m_unavailableProtectedSettings.isEmpty()) {
            root.insert(QStringLiteral("unavailableProtectedSettings"), m_unavailableProtectedSettings);
        }
        const bool needsProtection = !m_current.dvrSchedules.isEmpty() || !m_current.mpvOptions.isEmpty()
            || m_current.playerUserAgent != defaultPlayerUserAgent();
        if (needsProtection) {
            QJsonObject secrets;
            for (const auto &key : { QStringLiteral("dvrSchedules"), QStringLiteral("mpvOptions"), QStringLiteral("playerUserAgent") }) {
                secrets.insert(key, root.take(key));
            }
            const auto plain = QString::fromUtf8(QJsonDocument(secrets).toJson(QJsonDocument::Compact));
            const auto encrypted = protectSecret(plain);
            if (unprotectSecret(encrypted) != plain) throw std::runtime_error("Settings protection verification failed.");
            root.insert(QStringLiteral("protectedSettings"), encrypted);
        }
    } catch (const std::exception &error) {
        m_lastSaveError = QString::fromUtf8(error.what());
        file.cancelWriting();
        return;
    }
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        m_lastSaveError = QStringLiteral("Cannot restrict settings file permissions.");
        file.cancelWriting();
        return;
    }
    const QJsonDocument document(root);
    const auto payload = document.toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
        m_lastSaveError =
            QStringLiteral("Failed to write settings file: %1").arg(file.errorString());
        file.cancelWriting();
        return;
    }

    if (!file.commit()) {
        m_lastSaveError =
            QStringLiteral("Failed to commit settings file: %1").arg(file.errorString());
    }
}

AppSettings &SettingsManager::current()
{
    return m_current;
}

const AppSettings &SettingsManager::current() const
{
    return m_current;
}

QList<SourceSummary> SettingsManager::sourceSummaries() const
{
    return m_sourceSummaries;
}

std::optional<ServerProfile> SettingsManager::activeProfile() const
{
    if (!m_current.activeProfileId.has_value()) {
        return std::nullopt;
    }

    return profileById(m_current.activeProfileId.value());
}

std::optional<ServerProfile> SettingsManager::profileById(const QUuid &id) const
{
    const auto detail = profileDetailById(id);
    if (!detail.has_value()) {
        return std::nullopt;
    }

    for (const auto &summary : m_sourceSummaries) {
        if (summary.id == id) {
            return mergeSummaryAndDetail(summary, detail.value());
        }
    }

    return std::nullopt;
}

std::optional<ServerProfile> SettingsManager::profileDetailById(const QUuid &id) const
{
    if (id.isNull()) {
        return std::nullopt;
    }

    if (m_profileDetailCache.contains(id)) {
        return m_profileDetailCache.value(id);
    }

    try {
        auto detail = m_sourceStore.loadDetail(id);
        if (!detail.has_value()) return std::nullopt;
        m_profileDetailCache.insert(id, detail.value());
        return detail;
    } catch (const std::exception &) {
        m_lastLoadError = QStringLiteral("Source credentials cannot be unlocked. Unlock the original system account's secret store or re-enter the source connection details in Settings > Sources.");
        return std::nullopt;
    }
}

void SettingsManager::setActiveProfileId(const std::optional<QUuid> &profileId)
{
    if (m_sourceSummaries.isEmpty() && !m_current.profiles.isEmpty()) {
        m_sourceSummaries = buildSummariesFromProfiles(m_current.profiles);
        for (const auto &profile : m_current.profiles) {
            QString detailSaveError;
            m_sourceStore.saveDetail(profile, &detailSaveError);
        }
    }

    m_current.activeProfileId = profileId;
    syncProfileActivityFlagsAndMirror();
    saveSourceSummaries();
}

bool SettingsManager::addProfile(const ServerProfile &profile)
{
    auto normalized = profile;
    if (normalized.id.isNull()) {
        normalized.id = QUuid::createUuid();
    }
    normalized.autoRefreshIntervalHours = normalizeAutoRefreshIntervalHours(normalized.autoRefreshIntervalHours);

    if (!m_sourceStore.saveDetail(normalized, &m_lastSaveError)) {
        return false;
    }

    m_profileDetailCache.insert(normalized.id, normalized);
    m_sourceSummaries.push_back(toSummary(normalized));
    syncProfileActivityFlagsAndMirror();
    if (!saveSourceSummaries()) {
        return false;
    }

    save();
    return m_lastSaveError.isEmpty();
}

bool SettingsManager::replaceProfile(const QUuid &id, const ServerProfile &profile)
{
    if (id.isNull()) {
        return false;
    }

    auto index = -1;
    for (auto row = 0; row < m_sourceSummaries.size(); ++row) {
        if (m_sourceSummaries.at(row).id == id) {
            index = row;
            break;
        }
    }
    if (index < 0) {
        return false;
    }

    auto normalized = profile;
    normalized.id = id;
    normalized.autoRefreshIntervalHours = normalizeAutoRefreshIntervalHours(normalized.autoRefreshIntervalHours);

    if (!m_sourceStore.saveDetail(normalized, &m_lastSaveError)) {
        return false;
    }

    m_profileDetailCache.insert(id, normalized);
    auto updatedSummary = toSummary(normalized);
    updatedSummary.groupCount = m_sourceSummaries.at(index).groupCount;
    updatedSummary.isActive = m_sourceSummaries.at(index).isActive;
    m_sourceSummaries[index] = updatedSummary;
    syncProfileActivityFlagsAndMirror();
    if (!saveSourceSummaries()) {
        return false;
    }

    save();
    return m_lastSaveError.isEmpty();
}

bool SettingsManager::removeProfile(const QUuid &id)
{
    if (id.isNull()) {
        return false;
    }

    auto index = -1;
    for (auto row = 0; row < m_sourceSummaries.size(); ++row) {
        if (m_sourceSummaries.at(row).id == id) {
            index = row;
            break;
        }
    }
    if (index < 0) {
        return false;
    }

    const auto databasePath = QFileInfo(m_settingsFilePath).dir().filePath(QStringLiteral("iptv.db"));
    try {
        if (QFileInfo::exists(databasePath)) DatabaseService(databasePath).removeProfileData(id);
    } catch (const std::exception &error) {
        m_lastSaveError = QString::fromUtf8(error.what());
        return false;
    }
    m_sourceSummaries.removeAt(index);
    m_current.dvrSchedules.removeIf([&id](const DvrScheduleEntry &entry) { return entry.profileId == guidToString(id); });
    m_current.channelTrackPreferences.remove(guidToString(id));
    clearProfileDetailCache(id);
    if (!m_sourceStore.removeDetail(id, &m_lastSaveError)) {
        return false;
    }

    if (m_current.activeProfileId.has_value() && m_current.activeProfileId.value() == id) {
        m_current.activeProfileId = std::nullopt;
    }

    syncProfileActivityFlagsAndMirror();
    if (!saveSourceSummaries()) {
        return false;
    }

    save();
    return m_lastSaveError.isEmpty();
}

bool SettingsManager::setProfileLastRefreshed(const QUuid &id, const QDateTime &lastRefreshed)
{
    for (auto row = 0; row < m_sourceSummaries.size(); ++row) {
        if (m_sourceSummaries[row].id == id) {
            m_sourceSummaries[row].lastRefreshed = lastRefreshed.toUTC();
            rebuildSummaryMirrorFromSourceSummaries();
            return saveSourceSummaries();
        }
    }

    return false;
}

bool SettingsManager::setProfileGroupCount(const QUuid &id, const int groupCount)
{
    for (auto row = 0; row < m_sourceSummaries.size(); ++row) {
        if (m_sourceSummaries[row].id == id) {
            m_sourceSummaries[row].groupCount = std::max(0, groupCount);
            rebuildSummaryMirrorFromSourceSummaries();
            return saveSourceSummaries();
        }
    }

    return false;
}

bool SettingsManager::setProfileXtreamServerTimezone(const QUuid &id, const QString &timezone)
{
    for (auto row = 0; row < m_sourceSummaries.size(); ++row) {
        if (m_sourceSummaries[row].id == id) {
            m_sourceSummaries[row].xtreamServerTimezone = timezone.trimmed();
            rebuildSummaryMirrorFromSourceSummaries();
            return saveSourceSummaries();
        }
    }

    return false;
}

QString SettingsManager::settingsFilePath() const
{
    return m_settingsFilePath;
}

QString SettingsManager::lastLoadError() const
{
    return m_lastLoadError;
}

QString SettingsManager::lastSaveError() const
{
    return m_lastSaveError;
}

void SettingsManager::resetToDefaultsWithError(const QString &loadError)
{
    m_current = {};
    m_sourceSummaries.clear();
    m_profileDetailCache.clear();
    m_lastLoadError = loadError.trimmed();
    syncProfileActivityFlagsAndMirror();
}

void SettingsManager::backupInvalidSettingsFile() const
{
    QFile source(m_settingsFilePath);
    if (!source.exists()) {
        return;
    }

    const auto backupPath = QStringLiteral("%1.invalid-%2.bak")
        .arg(
            m_settingsFilePath,
            QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz"))
                + u'-' + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!source.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot preserve damaged settings.");
    const auto encoded = QString::fromLatin1(source.readAll().toBase64());
    const auto protectedValue = protectSecret(encoded);
    if (unprotectSecret(protectedValue) != encoded) throw std::runtime_error("Cannot verify settings backup protection.");
    QSaveFile backup(backupPath);
    const auto bytes = QJsonDocument(QJsonObject { { QStringLiteral("protectedBackup"), protectedValue } }).toJson();
    if (!backup.open(QIODevice::WriteOnly)
        || !backup.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || backup.write(bytes) != bytes.size() || !backup.commit()) {
        throw std::runtime_error("Cannot preserve damaged settings securely.");
    }
}

void SettingsManager::syncProfileActivityFlagsAndMirror()
{
    for (auto &summary : m_sourceSummaries) {
        summary.isActive = m_current.activeProfileId.has_value() && summary.id == m_current.activeProfileId.value();
    }
    rebuildSummaryMirrorFromSourceSummaries();
}

void SettingsManager::rebuildSummaryMirrorFromSourceSummaries()
{
    m_current.profiles.clear();
    m_current.profiles.reserve(m_sourceSummaries.size());
    for (const auto &summary : m_sourceSummaries) {
        ServerProfile profile;
        profile.id = summary.id;
        profile.name = summary.name;
        profile.type = summary.type;
        profile.autoRefreshIntervalHours = summary.autoRefreshIntervalHours;
        profile.xtreamServerTimezone = summary.xtreamServerTimezone;
        profile.lastRefreshed = summary.lastRefreshed;
        profile.isActive = summary.isActive;
        m_current.profiles.push_back(profile);
    }
}

bool SettingsManager::saveSourceSummaries()
{
    if (!m_sourceStore.saveSummaries(m_sourceSummaries, &m_lastSaveError)) {
        return false;
    }

    save();
    return m_lastSaveError.isEmpty();
}

void SettingsManager::clearProfileDetailCache(const QUuid &id)
{
    m_profileDetailCache.remove(id);
}

void SettingsManager::migrateLegacyProfilesIfNeeded(const AppSettings &legacySettings)
{
    auto summaries = buildSummariesFromProfiles(legacySettings.profiles);
    QString errorText;
    for (const auto &profile : legacySettings.profiles) {
        if (!m_sourceStore.saveDetail(profile, &errorText)) {
            throw std::runtime_error(errorText.toStdString());
        }
    }

    if (!m_sourceStore.saveSummaries(summaries, &errorText)) throw std::runtime_error(errorText.toStdString());
}

QList<SourceSummary> SettingsManager::buildSummariesFromProfiles(const QList<ServerProfile> &profiles) const
{
    QList<SourceSummary> summaries;
    summaries.reserve(profiles.size());
    for (const auto &profile : profiles) {
        summaries.push_back(toSummary(profile));
    }
    return summaries;
}

SourceSummary SettingsManager::toSummary(const ServerProfile &profile)
{
    SourceSummary summary;
    summary.id = profile.id;
    summary.name = profile.name;
    summary.type = profile.type;
    summary.autoRefreshIntervalHours = normalizeAutoRefreshIntervalHours(profile.autoRefreshIntervalHours);
    summary.xtreamServerTimezone = profile.xtreamServerTimezone.trimmed();
    summary.lastRefreshed = profile.lastRefreshed.toUTC();
    summary.isActive = profile.isActive;
    return summary;
}

ServerProfile SettingsManager::mergeSummaryAndDetail(const SourceSummary &summary, const ServerProfile &detail)
{
    auto merged = detail;
    merged.id = summary.id;
    merged.name = summary.name;
    merged.type = summary.type;
    merged.autoRefreshIntervalHours = summary.autoRefreshIntervalHours;
    merged.xtreamServerTimezone = summary.xtreamServerTimezone;
    merged.lastRefreshed = summary.lastRefreshed;
    merged.isActive = summary.isActive;
    return merged;
}

} // namespace OKILTV::Core
