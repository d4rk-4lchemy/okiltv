#pragma once

#include "models.h"

#include <QHash>
#include <QDateTime>
#include <QString>

namespace OKILTV::Core {

class DatabaseService
{
public:
    explicit DatabaseService(QString databaseFilePath = {});

    QString databaseFilePath() const;

    void ensureSchema() const;
    void upsertChannels(const QList<Channel> &channels) const;
    void replaceChannelsForProfile(const QUuid &profileId, const QList<Channel> &channels) const;
    QList<Channel> loadChannels(const QUuid &profileId) const;
    void updateCachedIcon(int channelId, const QUuid &profileId, const QString &localPath) const;
    QHash<int, qint64> loadWatchSecondsByProfile(const QUuid &profileId) const;
    void incrementWatchSeconds(const QUuid &profileId, int channelId, qint64 deltaSeconds) const;

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
