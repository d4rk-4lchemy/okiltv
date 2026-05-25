#pragma once

#include "models.h"
#include "sourcestore.h"

#include <QHash>
#include <QSet>
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
    QList<SourceSummary> sourceSummaries() const;

    std::optional<ServerProfile> activeProfile() const;
    std::optional<ServerProfile> profileById(const QUuid &id) const;
    std::optional<ServerProfile> profileDetailById(const QUuid &id) const;
    void setActiveProfileId(const std::optional<QUuid> &profileId);
    bool addProfile(const ServerProfile &profile);
    bool replaceProfile(const QUuid &id, const ServerProfile &profile);
    bool removeProfile(const QUuid &id);
    bool setProfileLastRefreshed(const QUuid &id, const QDateTime &lastRefreshed);
    bool setProfileGroupCount(const QUuid &id, int groupCount);
    bool setProfileXtreamServerTimezone(const QUuid &id, const QString &timezone);

    QString settingsFilePath() const;
    QString lastLoadError() const;
    QString lastSaveError() const;

private:
    void resetToDefaultsWithError(const QString &loadError);
    void backupInvalidSettingsFile() const;
    void syncProfileActivityFlagsAndMirror();
    void rebuildSummaryMirrorFromSourceSummaries();
    bool saveSourceSummaries();
    void clearProfileDetailCache(const QUuid &id);
    void clearMissingProfileArtifacts(const QSet<QUuid> &knownIds);
    void migrateLegacyProfilesIfNeeded(const AppSettings &legacySettings);
    QList<SourceSummary> buildSummariesFromProfiles(const QList<ServerProfile> &profiles) const;
    static SourceSummary toSummary(const ServerProfile &profile);
    static ServerProfile mergeSummaryAndDetail(const SourceSummary &summary, const ServerProfile &detail);

    QString m_settingsFilePath;
    AppSettings m_current;
    mutable QList<SourceSummary> m_sourceSummaries;
    mutable QHash<QUuid, ServerProfile> m_profileDetailCache;
    SourceStore m_sourceStore;
    QString m_lastLoadError;
    mutable QString m_lastSaveError;
};

} // namespace OKILTV::Core
