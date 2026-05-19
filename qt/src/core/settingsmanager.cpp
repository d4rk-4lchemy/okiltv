#include "settingsmanager.h"

#include "appdatapaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

namespace OKILTV::Core {

SettingsManager::SettingsManager(QString settingsFilePath)
    : m_settingsFilePath(settingsFilePath.isEmpty() ? AppDataPaths::settingsFile() : std::move(settingsFilePath))
{
}

void SettingsManager::load()
{
    m_lastLoadError.clear();
    QFile file(m_settingsFilePath);
    if (!file.exists()) {
        m_current = {};
        syncProfileActivityFlags();
        return;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        resetToDefaultsWithError(
            QStringLiteral("Failed to open settings file for read: %1")
                .arg(file.errorString()));
        return;
    }

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

    m_current = appSettingsFromJson(document.object());
    syncProfileActivityFlags();
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

    const QJsonDocument document(toJson(m_current));
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

std::optional<ServerProfile> SettingsManager::activeProfile() const
{
    if (!m_current.activeProfileId.has_value()) {
        return std::nullopt;
    }

    return profileById(m_current.activeProfileId.value());
}

std::optional<ServerProfile> SettingsManager::profileById(const QUuid &id) const
{
    for (const auto &profile : m_current.profiles) {
        if (profile.id == id) {
            return profile;
        }
    }

    return std::nullopt;
}

void SettingsManager::setActiveProfileId(const std::optional<QUuid> &profileId)
{
    m_current.activeProfileId = profileId;
    syncProfileActivityFlags();
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
    m_lastLoadError = loadError.trimmed();
    syncProfileActivityFlags();
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

void SettingsManager::syncProfileActivityFlags()
{
    for (auto &profile : m_current.profiles) {
        profile.isActive = m_current.activeProfileId.has_value() && profile.id == m_current.activeProfileId.value();
    }
}

} // namespace OKILTV::Core
