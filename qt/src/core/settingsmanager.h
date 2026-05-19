#pragma once

#include "models.h"

#include <QString>

namespace OKILTV::Core {

class SettingsManager
{
public:
    explicit SettingsManager(QString settingsFilePath = {});

    void load();
    void save() const;

    AppSettings &current();
    const AppSettings &current() const;

    std::optional<ServerProfile> activeProfile() const;
    std::optional<ServerProfile> profileById(const QUuid &id) const;
    void setActiveProfileId(const std::optional<QUuid> &profileId);

    QString settingsFilePath() const;
    QString lastLoadError() const;
    QString lastSaveError() const;

private:
    void resetToDefaultsWithError(const QString &loadError);
    void backupInvalidSettingsFile() const;
    void syncProfileActivityFlags();

    QString m_settingsFilePath;
    AppSettings m_current;
    QString m_lastLoadError;
    mutable QString m_lastSaveError;
};

} // namespace OKILTV::Core
