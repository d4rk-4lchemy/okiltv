#pragma once

#include "epgservice.h"
#include "models.h"

#include <QDateTime>
#include <QUuid>
#include <atomic>

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

    using Cancellation = std::shared_ptr<std::atomic_bool>;
    static Cancellation beginImport(const QUuid &profileId);
    static void cancel(const Cancellation &token);
    static void invalidateSource(const QUuid &profileId);
    static QString manifestFile(const QUuid &profileId);
    CacheData build(const QUuid &profileId, const QString &fingerprint,
        const EpgStore::Producer &producer, bool deduplicate = false,
        const Cancellation &token = {}) const;
    LoadResult load(const QUuid &profileId, const Cancellation &token = {}) const;
    void save(const CacheData &data, const Cancellation &token = {}) const;
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
