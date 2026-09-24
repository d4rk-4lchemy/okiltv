#pragma once

#include "models.h"
#include <QCache>
#include <QMutex>
#include <functional>
#include <memory>

namespace OKILTV::Core {

// A published database is immutable. Readers pin this object, never a SQL
// connection; every connection belongs to the thread executing the query.
class EpgStore final
{
public:
    using Cancelled = std::function<bool()>;
    using Sink = std::function<void(const EpgEntry &)>;
    using Producer = std::function<void(const Sink &)>;
    struct Metadata {
        QUuid profileId;
        QString fingerprint;
        QDateTime fetchedAt;
        qint64 entries = 0;
    };

    static std::shared_ptr<EpgStore> create(const QString &path, Metadata metadata,
        const Producer &produce, bool deduplicate = false, const Cancelled &cancelled = {});
    static std::shared_ptr<EpgStore> open(const QString &path);
    static void retireFile(const QString &path);
    ~EpgStore();

    const Metadata &metadata() const { return m_metadata; }
    QString path() const { return m_path; }
    void keep();
    QList<EpgEntry> range(const QString &channel, const QDateTime &from,
        const QDateTime &to, int limit = -1, bool summaries = false) const;
    QHash<QString, QList<EpgEntry>> ranges(const QStringList &channels,
        const QDateTime &from, const QDateTime &to, int limit = -1, bool summaries = false) const;
    QList<EpgEntry> allEntries() const; // Small fixtures/diagnostics only, not an application read path.
    QDateTime maxStop(const QString &channel) const;
    int cachedBytes() const;
    QString diagnostics() const;

private:
    EpgStore(QString path, Metadata metadata, bool temporary);
    QString m_path;
    Metadata m_metadata;
    mutable QMutex m_mutex;
    bool m_temporary;
    mutable qint64 m_cacheHits = 0, m_queryCount = 0, m_queryMilliseconds = 0;
    mutable QCache<QString, QList<EpgEntry>> m_cache { 32LL * 1024 * 1024 };
};
} // namespace OKILTV::Core
