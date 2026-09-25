#pragma once

#include "models.h"

#include <QHash>
#include <QDateTime>
#include <QString>
#include <functional>

namespace OKILTV::Core {

struct CatchupProgress
{
    QString key;
    QUuid profileId;
    qint64 programStartMs { 0 };
    qint64 programStopMs { 0 };
    qint64 positionMs { 0 };
    qint64 expiresAtMs { 0 };

    static QString keyFor(const Channel &channel, const QDateTime &programStart);
    qint64 resumeSeconds(qint64 programEndMs) const;
};

class DatabaseService
{
public:
    using RebuildStarted = std::function<void()>;
    explicit DatabaseService(QString databaseFilePath = {}, const RebuildStarted &rebuildStarted = {});

    QString databaseFilePath() const;

    void ensureSchema(const RebuildStarted &rebuildStarted = {}) const;
    // A process-wide per-source publication gate also covers separate DB instances.
    static quint64 beginChannelImport(const QUuid &profileId);
    static bool channelImportCurrent(const QUuid &profileId, quint64 token);
    bool publishChannels(const QUuid &profileId, quint64 token, QList<Channel> &channels, bool retainM3uIds) const;
    void publishChannelIcon(const Channel &channel, quint64 token, const QString &path) const;
    void removeProfileData(const QUuid &profileId) const;
    void upsertChannels(const QList<Channel> &channels) const;
    void replaceChannelsForProfile(const QUuid &profileId, const QList<Channel> &channels,
                                   std::optional<qint64> nextM3uChannelId = std::nullopt) const;
    qint64 nextM3uChannelId(const QUuid &profileId) const;
    QList<Channel> loadChannels(const QUuid &profileId) const;
    // Normalized, unique groups in channel order; never reads protected URLs.
    QStringList loadChannelGroupIds(const QUuid &profileId) const;
    void updateCachedIcon(int channelId, const QUuid &profileId, const QString &localPath) const;
    QHash<int, qint64> loadWatchSecondsByProfile(const QUuid &profileId) const;
    void incrementWatchSeconds(const QUuid &profileId, int channelId, qint64 deltaSeconds) const;
    QList<CatchupProgress> loadCatchupProgress() const;
    void saveCatchupProgress(const CatchupProgress &progress) const;
    void removeCatchupProgress(const QString &key) const;

    void replaceEpg(const QUuid &profileId, const QList<EpgEntry> &entries) const;
    QList<EpgEntry> queryEpg(
        const QUuid &profileId,
        const QString &channelId,
        const QDateTime &from,
        const QDateTime &to) const;

    QString cachedIconByHash(const QString &urlHash) const;
    void upsertIconCache(const QString &urlHash, const QString &localPath, qint64 fetchedAtUnix) const;

private:
    QString m_databaseFilePath;
};

} // namespace OKILTV::Core
