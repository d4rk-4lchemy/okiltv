#include "settingsmanager.h"

#include "appdatapaths.h"

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
    , m_sourceStore(AppDataPaths::sourceSummariesFile(), AppDataPaths::sourcesDirectory())
{
}

void SettingsManager::load()
{
    m_lastLoadError.clear();

    AppSettings parsedSettings;
    QFile file(m_settingsFilePath);
    if (!file.exists()) {
        parsedSettings = {};
    } else if (!file.open(QIODevice::ReadOnly)) {
        resetToDefaultsWithError(
            QStringLiteral("Failed to open settings file for read: %1")
                .arg(file.errorString()));
        return;
    } else {
        const auto bytes = file.readAll();
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            backupInvalidSettingsFile();
            resetToDefaultsWithError(
                QStringLiteral("Failed to parse settings JSON: %1")
                    .arg(parseError.errorString()));
            return;
        }
        if (!document.isObject()) {
            backupInvalidSettingsFile();
            resetToDefaultsWithError(QStringLiteral("Settings JSON root is not an object."));
            return;
        }

        parsedSettings = appSettingsFromJson(document.object());
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

    const auto detail = m_sourceStore.loadDetail(id);
    if (!detail.has_value()) {
        return std::nullopt;
    }

    m_profileDetailCache.insert(id, detail.value());
    return detail;
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

    m_sourceSummaries.removeAt(index);
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
            QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss")));
    source.copy(backupPath);
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

void SettingsManager::clearMissingProfileArtifacts(const QSet<QUuid> &knownIds)
{
    const auto files = QDir(AppDataPaths::sourcesDirectory()).entryInfoList(
        QStringList() << QStringLiteral("*.json"),
        QDir::Files,
        QDir::Name);
    for (const auto &file : files) {
        const auto baseName = file.baseName();
        const auto id = parseGuid(baseName);
        if (id.isNull() || knownIds.contains(id)) {
            continue;
        }

        QFile::remove(file.filePath());
    }
}

void SettingsManager::migrateLegacyProfilesIfNeeded(const AppSettings &legacySettings)
{
    auto summaries = buildSummariesFromProfiles(legacySettings.profiles);
    QString errorText;
    QSet<QUuid> knownIds;
    for (const auto &profile : legacySettings.profiles) {
        if (!m_sourceStore.saveDetail(profile, &errorText)) {
            continue;
        }
        knownIds.insert(profile.id);
    }

    m_sourceStore.saveSummaries(summaries, &errorText);
    clearMissingProfileArtifacts(knownIds);
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
