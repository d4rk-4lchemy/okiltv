#pragma once

#include "epgservice.h"
#include "models.h"

#include <QDateTime>
#include <QUuid>

namespace OKILTV::Core {

class EpgCacheService
{
public:
    struct CacheData
    {
        QUuid profileId;
        QString sourceFingerprint;
        QDateTime fetchedAt;
        EpgService::Snapshot snapshot;
    };

    enum class LoadStatus
    {
        NotFound,
        Loaded,
        Invalid
    };

    struct LoadResult
    {
        LoadStatus status { LoadStatus::NotFound };
        CacheData data;
    };

    LoadResult load(const QUuid &profileId) const;
    void save(const CacheData &data) const;
    void remove(const QUuid &profileId) const;

    static QString sourceFingerprint(const ServerProfile &profile);
    static bool matchesProfile(const CacheData &data, const ServerProfile &profile);
    static QDateTime nextRefreshAt(const QDateTime &fetchedAt, int refreshIntervalMinutes);
    static bool isStale(
        const QDateTime &fetchedAt,
        int refreshIntervalMinutes,
        const QDateTime &now = QDateTime::currentDateTimeUtc());
    static qint64 ageSeconds(
        const QDateTime &fetchedAt,
        const QDateTime &now = QDateTime::currentDateTimeUtc());
};

} // namespace OKILTV::Core
