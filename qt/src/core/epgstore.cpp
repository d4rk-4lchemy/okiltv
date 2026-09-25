#include "epgstore.h"
#include <QFile>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QTimeZone>
#include <QVariant>
#include <algorithm>
#include <stdexcept>

namespace OKILTV::Core {
namespace {
QMutex registryMutex;
QHash<QString, std::weak_ptr<EpgStore>> registry;

void check(bool ok, const QSqlQuery &q)
{
    if (!ok) throw std::runtime_error(q.lastError().text().toStdString());
}
class Connection {
public:
    explicit Connection(const QString &path, bool write = false)
        : name(QUuid::createUuid().toString()), db(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name))
    {
        db.setDatabaseName(path);
        if (!write) db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) {
            const auto error = db.lastError().text();
            db = {};
            QSqlDatabase::removeDatabase(name);
            throw std::runtime_error(error.toStdString());
        }
        try {
            QSqlQuery q(db);
            for (const auto *sql : {"PRAGMA cache_size=-8192", "PRAGMA mmap_size=0", "PRAGMA temp_store=FILE"})
                check(q.exec(QString::fromLatin1(sql)), q);
        } catch (...) {
            db.close();
            db = {};
            QSqlDatabase::removeDatabase(name);
            throw;
        }
    }
    ~Connection() { db.close(); db = {}; QSqlDatabase::removeDatabase(name); }
    QString name;
    QSqlDatabase db;
};
EpgEntry readEntry(const QSqlQuery &q)
{
    EpgEntry e;
    e.channelId = q.value(0).toString(); e.title = q.value(1).toString();
    e.subTitle = q.value(2).toString(); e.description = q.value(3).toString();
    e.episodeNum = q.value(4).toString();
    e.start = QDateTime::fromMSecsSinceEpoch(q.value(5).toLongLong(), QTimeZone::UTC);
    e.stop = QDateTime::fromMSecsSinceEpoch(q.value(6).toLongLong(), QTimeZone::UTC);
    return e;
}
constexpr auto columns = "channel,title,subtitle,description,episode,start,stop";
qint64 cost(const EpgEntry &e)
{
    return static_cast<qint64>(sizeof(EpgEntry)) + 128 + 2 * (e.channelId.size() + e.title.size()
        + e.subTitle.size() + e.description.size() + e.episodeNum.size());
}
void checkCancelled(const EpgStore::Cancelled &cancelled)
{
    if (cancelled && cancelled()) throw std::runtime_error("EPG import cancelled.");
}
}

EpgStore::EpgStore(QString path, Metadata metadata, bool temporary)
    : m_path(std::move(path)), m_metadata(std::move(metadata)), m_temporary(temporary) {}
EpgStore::~EpgStore()
{
    if (m_temporary) QFile::remove(m_path);
}
void EpgStore::keep() { QMutexLocker lock(&m_mutex); m_temporary = false; }
void EpgStore::retireFile(const QString &path)
{
    QMutexLocker lock(&registryMutex);
    if (auto store = registry.value(path).lock()) {
        QMutexLocker storeLock(&store->m_mutex);
        store->m_temporary = true;
    } else {
        registry.remove(path);
        QFile::remove(path);
    }
}

std::shared_ptr<EpgStore> EpgStore::create(const QString &path, Metadata metadata,
    const Producer &produce, bool deduplicate, const Cancelled &cancelled)
{
    if (QFile::exists(path)) throw std::runtime_error("EPG generation already exists.");
    auto result = std::shared_ptr<EpgStore>(new EpgStore(path, std::move(metadata), true));
    {
        Connection c(path, true);
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QSqlQuery q(c.db);
        check(q.exec(QStringLiteral("CREATE TABLE programmes(id INTEGER PRIMARY KEY, key TEXT NOT NULL, channel TEXT, title TEXT, subtitle TEXT, description TEXT, episode TEXT, start INTEGER, stop INTEGER)")), q);
        if (deduplicate) check(q.exec(QStringLiteral("CREATE UNIQUE INDEX identity ON programmes(key,start)")), q);
        check(q.exec(QStringLiteral("BEGIN")), q);
        QSqlQuery insert(c.db);
        check(insert.prepare(QStringLiteral("INSERT OR IGNORE INTO programmes(key,channel,title,subtitle,description,episode,start,stop) VALUES(?,?,?,?,?,?,?,?)")), insert);
        int batchCount = 0;
        qint64 batchBytes = 0;
        produce([&](const EpgEntry &e) {
            checkCancelled(cancelled);
            insert.bindValue(0, e.channelId.trimmed().toLower()); insert.bindValue(1, e.channelId);
            insert.bindValue(2, e.title); insert.bindValue(3, e.subTitle);
            insert.bindValue(4, e.description); insert.bindValue(5, e.episodeNum);
            insert.bindValue(6, e.start.toMSecsSinceEpoch()); insert.bindValue(7, e.stop.toMSecsSinceEpoch());
            check(insert.exec(), insert);
            result->m_metadata.entries += insert.numRowsAffected();
            batchBytes += cost(e);
            if (++batchCount >= 1000 || batchBytes >= 1024LL * 1024) {
                check(q.exec(QStringLiteral("COMMIT")), q);
                check(q.exec(QStringLiteral("BEGIN")), q);
                batchCount = 0; batchBytes = 0;
            }
        });
        checkCancelled(cancelled);
        check(q.exec(QStringLiteral("COMMIT")), q);
        check(q.exec(QStringLiteral("CREATE INDEX by_start ON programmes(key,start,id,stop)")), q);
        check(q.exec(QStringLiteral("CREATE INDEX by_stop ON programmes(key,stop)")), q);
        check(q.exec(QStringLiteral("CREATE TABLE metadata(version INTEGER, profile TEXT, fingerprint TEXT, fetched INTEGER, entries INTEGER)")), q);
        check(q.prepare(QStringLiteral("INSERT INTO metadata VALUES(1,?,?,?,?)")), q);
        q.addBindValue(result->m_metadata.profileId.toString(QUuid::WithoutBraces));
        q.addBindValue(result->m_metadata.fingerprint);
        q.addBindValue(result->m_metadata.fetchedAt.toMSecsSinceEpoch());
        q.addBindValue(result->m_metadata.entries);
        check(q.exec(), q);
        check(q.exec(QStringLiteral("PRAGMA quick_check")), q);
        if (!q.next() || q.value(0).toString() != QStringLiteral("ok"))
            throw std::runtime_error("EPG database verification failed.");
        checkCancelled(cancelled);
    }
    QMutexLocker lock(&registryMutex);
    for (auto it = registry.begin(); it != registry.end();) {
        if (it.value().expired()) it = registry.erase(it); else ++it;
    }
    registry.insert(path, result);
    return result;
}

std::shared_ptr<EpgStore> EpgStore::open(const QString &path)
{
    QMutexLocker lock(&registryMutex);
    if (auto existing = registry.value(path).lock()) return existing;
    Connection c(path);
    QSqlQuery q(c.db);
    check(q.exec(QStringLiteral("SELECT version,profile,fingerprint,fetched,entries FROM metadata")), q);
    if (!q.next() || q.value(0).toInt() != 1) throw std::runtime_error("Invalid EPG database metadata.");
    Metadata m { QUuid(q.value(1).toString()), q.value(2).toString(),
        QDateTime::fromMSecsSinceEpoch(q.value(3).toLongLong(), QTimeZone::UTC), q.value(4).toLongLong() };
    if (m.profileId.isNull() || m.entries < 0 || !m.fetchedAt.isValid()) throw std::runtime_error("Invalid EPG database identity.");
    auto result = std::shared_ptr<EpgStore>(new EpgStore(path, m, false));
    for (auto it = registry.begin(); it != registry.end();) {
        if (it.value().expired()) it = registry.erase(it); else ++it;
    }
    registry.insert(path, result);
    return result;
}

QHash<QString, QList<EpgEntry>> EpgStore::ranges(const QStringList &channels,
    const QDateTime &from, const QDateTime &to, int limit, bool summaries) const
{
    QHash<QString, QList<EpgEntry>> result;
    if (from >= to || limit == 0) return result;
    std::unique_ptr<Connection> connection;
    for (const auto &channel : channels) {
        const auto key = channel.trimmed().toLower();
        const auto cacheKey = QStringLiteral("%1:%2:%3:%4:%5").arg(from.toMSecsSinceEpoch())
            .arg(to.toMSecsSinceEpoch()).arg(limit).arg(summaries).arg(key);
        {
            QMutexLocker lock(&m_mutex);
            if (const auto *cached = m_cache.object(cacheKey)) { ++m_cacheHits; result.insert(key, *cached); continue; }
        }
        QElapsedTimer timer; timer.start();
        if (!connection) connection = std::make_unique<Connection>(m_path);
        QSqlQuery q(connection->db);
        q.setForwardOnly(true);
        const auto projection = summaries ? QStringLiteral("channel,title,subtitle,'',episode,start,stop") : QString::fromLatin1(columns);
        check(q.prepare(QStringLiteral("SELECT %1 FROM programmes WHERE key=? AND start<? AND stop>? ORDER BY start,id LIMIT ?").arg(projection)), q);
        q.addBindValue(key); q.addBindValue(to.toMSecsSinceEpoch()); q.addBindValue(from.toMSecsSinceEpoch()); q.addBindValue(limit);
        check(q.exec(), q);
        QList<EpgEntry> entries;
        qint64 bytes = 128 + 2 * cacheKey.size();
        while (q.next()) { auto e = readEntry(q); bytes += cost(e); entries.push_back(std::move(e)); }
        if (q.lastError().isValid()) throw std::runtime_error(q.lastError().text().toStdString());
        {
            QMutexLocker lock(&m_mutex);
            ++m_queryCount; m_queryMilliseconds += timer.elapsed();
            if (bytes <= m_cache.maxCost()) m_cache.insert(cacheKey, new QList<EpgEntry>(entries), static_cast<int>(bytes));
        }
        result.insert(key, entries);
    }
    return result;
}
QList<EpgEntry> EpgStore::range(const QString &channel, const QDateTime &from, const QDateTime &to, int limit, bool summaries) const
{
    return ranges({channel}, from, to, limit, summaries).value(channel.trimmed().toLower());
}
QList<EpgEntry> EpgStore::allEntries() const
{
    Connection c(m_path);
    QSqlQuery q(c.db); q.setForwardOnly(true);
    check(q.exec(QStringLiteral("SELECT %1 FROM programmes ORDER BY start,key,id").arg(QString::fromLatin1(columns))), q);
    QList<EpgEntry> entries;
    while (q.next()) entries.push_back(readEntry(q));
    return entries;
}
QDateTime EpgStore::maxStop(const QString &channel) const
{
    Connection c(m_path); QSqlQuery q(c.db);
    check(q.prepare(QStringLiteral("SELECT MAX(stop) FROM programmes WHERE key=?")), q);
    q.addBindValue(channel.trimmed().toLower()); check(q.exec(), q);
    return q.next() && !q.value(0).isNull() ? QDateTime::fromMSecsSinceEpoch(q.value(0).toLongLong(), QTimeZone::UTC) : QDateTime {};
}
QString EpgStore::diagnostics() const
{
    QMutexLocker lock(&m_mutex);
    return QStringLiteral("entries=%1 cacheBytes=%2 cacheHits=%3 diskQueries=%4 queryMs=%5")
        .arg(m_metadata.entries).arg(m_cache.totalCost()).arg(m_cacheHits).arg(m_queryCount).arg(m_queryMilliseconds);
}
int EpgStore::cachedBytes() const { QMutexLocker lock(&m_mutex); return static_cast<int>(m_cache.totalCost()); }
} // namespace OKILTV::Core
