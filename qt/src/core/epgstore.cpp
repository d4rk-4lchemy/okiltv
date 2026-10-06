#include "epgstore.h"
#include "epgsearch_p.h"
#include <QFile>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QTimeZone>
#include <QVariant>
#include <QCoreApplication>
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
        // Keep ordinary EPG available even when a packaging error omits FTS5.
        result->m_searchReady = q.exec(QStringLiteral("CREATE VIRTUAL TABLE programme_search USING fts5(title,subtitle, tokenize='unicode61 remove_diacritics 0', prefix='2 3')"));
        if (!result->m_searchReady && !q.lastError().text().contains(QStringLiteral("no such module"), Qt::CaseInsensitive))
            check(false, q);
        if (result->m_searchReady) {
            check(q.exec(QStringLiteral("CREATE TABLE search_documents(programme INTEGER PRIMARY KEY, identity TEXT NOT NULL UNIQUE, title TEXT NOT NULL, subtitle TEXT NOT NULL)")), q);
        }
        check(q.exec(QStringLiteral("CREATE TABLE search_metadata(schema_version INTEGER, normalization_version INTEGER, available INTEGER, maximum_duration INTEGER)")), q);
        check(q.exec(QStringLiteral("INSERT INTO search_metadata VALUES(2,1,%1,0)").arg(result->m_searchReady ? 1 : 0)), q);
        if (deduplicate) check(q.exec(QStringLiteral("CREATE UNIQUE INDEX identity ON programmes(key,start)")), q);
        check(q.exec(QStringLiteral("BEGIN")), q);
        QSqlQuery insert(c.db);
        check(insert.prepare(QStringLiteral("INSERT OR IGNORE INTO programmes(key,channel,title,subtitle,description,episode,start,stop) VALUES(?,?,?,?,?,?,?,?)")), insert);
        QSqlQuery document(c.db);
        if (result->m_searchReady) {
            check(document.prepare(QStringLiteral("INSERT OR IGNORE INTO search_documents VALUES(?,?,?,?)")), document);
        }
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
            if (result->m_searchReady && insert.numRowsAffected() > 0
                && e.start.isValid() && e.stop.isValid() && e.stop > e.start) {
                const auto id = insert.lastInsertId();
                const auto title = normalizeEpgSearchText(e.title);
                const auto subtitle = normalizeEpgSearchText(e.subTitle);
                document.bindValue(0, id); document.bindValue(1, EpgSearchDetail::identity(e));
                document.bindValue(2, title); document.bindValue(3, subtitle);
                check(document.exec(), document);
                result->m_maximumSearchDuration = std::max(result->m_maximumSearchDuration, e.start.msecsTo(e.stop));
            }
            batchBytes += cost(e);
            if (++batchCount >= 1000 || batchBytes >= 1024LL * 1024) {
                check(q.exec(QStringLiteral("COMMIT")), q);
                check(q.exec(QStringLiteral("BEGIN")), q);
                batchCount = 0; batchBytes = 0;
            }
        });
        checkCancelled(cancelled);
        check(q.exec(QStringLiteral("COMMIT")), q);
        if (result->m_searchReady) {
            // Chronological document rowids let FTS read directly in the UI's
            // time order, with bounded pages and no full-result SQL sort.
            check(q.exec(QStringLiteral("CREATE TABLE search_order(id INTEGER PRIMARY KEY, programme INTEGER NOT NULL UNIQUE, start INTEGER NOT NULL, stop INTEGER NOT NULL)")), q);
            check(q.exec(QStringLiteral("INSERT INTO search_order(programme,start,stop) SELECT d.programme,p.start,p.stop FROM search_documents d JOIN programmes p ON p.id=d.programme ORDER BY p.start,p.stop,d.identity")), q);
            checkCancelled(cancelled);
            check(q.exec(QStringLiteral("CREATE INDEX search_order_time ON search_order(start,id)")), q);
            check(q.exec(QStringLiteral("CREATE INDEX search_exact_title ON search_documents(title,programme)")), q);
            QSqlQuery ordered(c.db), searchInsert(c.db);
            ordered.setForwardOnly(true);
            check(ordered.exec(QStringLiteral("SELECT o.id,d.title,d.subtitle FROM search_order o JOIN search_documents d ON d.programme=o.programme ORDER BY o.id")), ordered);
            check(searchInsert.prepare(QStringLiteral("INSERT INTO programme_search(rowid,title,subtitle) VALUES(?,?,?)")), searchInsert);
            check(q.exec(QStringLiteral("BEGIN")), q);
            int searchBatch = 0;
            while (ordered.next()) {
                checkCancelled(cancelled);
                searchInsert.bindValue(0, ordered.value(0));
                searchInsert.bindValue(1, ordered.value(1));
                searchInsert.bindValue(2, ordered.value(2));
                check(searchInsert.exec(), searchInsert);
                if (++searchBatch == 1000) {
                    check(q.exec(QStringLiteral("COMMIT")), q);
                    checkCancelled(cancelled);
                    check(q.exec(QStringLiteral("BEGIN")), q);
                    searchBatch = 0;
                }
            }
            if (ordered.lastError().isValid()) check(false, ordered);
            ordered.finish();
            check(q.exec(QStringLiteral("COMMIT")), q);
            checkCancelled(cancelled);
            check(q.exec(QStringLiteral("INSERT INTO programme_search(programme_search) VALUES('optimize')")), q);
            checkCancelled(cancelled);
            check(q.exec(QStringLiteral("INSERT INTO programme_search(programme_search) VALUES('integrity-check')")), q);
            check(q.exec(QStringLiteral("UPDATE search_metadata SET maximum_duration=%1").arg(result->m_maximumSearchDuration)), q);
        }
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
    if (q.exec(QStringLiteral("SELECT schema_version,normalization_version,available,maximum_duration FROM search_metadata")) && q.next()) {
        result->m_searchReady = q.value(0).toInt() == 2 && q.value(1).toInt() == 1 && q.value(2).toInt() == 1 && supportsFts5();
        result->m_maximumSearchDuration = q.value(3).toLongLong();
    }
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

bool EpgStore::supportsFts5()
{
    try {
        Connection c(QStringLiteral(":memory:"), true);
        QSqlQuery q(c.db);
        return q.exec(QStringLiteral("CREATE VIRTUAL TABLE capability_test USING fts5(title)"));
    } catch (const std::exception &) { return false; }
}

bool EpgStore::searchReady() const
{
    QMutexLocker lock(&m_mutex);
    return m_searchReady || (m_searchReplacement && m_searchReplacement->searchReady());
}

void EpgStore::useSearchIndex(const std::shared_ptr<EpgStore> &replacement)
{
    if (!replacement || replacement.get() == this || !replacement->searchReady()
        || replacement->metadata().profileId != m_metadata.profileId
        || replacement->metadata().fingerprint != m_metadata.fingerprint
        || replacement->metadata().fetchedAt != m_metadata.fetchedAt
        || replacement->metadata().entries != m_metadata.entries)
        throw std::runtime_error("Invalid replacement EPG search generation.");
    QMutexLocker lock(&m_mutex);
    m_searchReplacement = replacement;
    m_searchPreparationError.clear();
}

void EpgStore::setSearchPreparationError(const QString &code)
{
    QMutexLocker lock(&m_mutex);
    m_searchPreparationError = code;
}

std::shared_ptr<EpgStore> EpgStore::withSearchIndex(const QString &path, const Cancelled &cancelled) const
{
    auto metadata = m_metadata;
    metadata.entries = 0;
    return create(path, metadata, [&](const Sink &sink) {
        Connection c(m_path);
        QSqlQuery q(c.db); q.setForwardOnly(true);
        check(q.exec(QStringLiteral("SELECT %1 FROM programmes ORDER BY id").arg(QString::fromLatin1(columns))), q);
        while (q.next()) { checkCancelled(cancelled); sink(readEntry(q)); }
        if (q.lastError().isValid()) check(false, q);
    }, false, cancelled);
}

EpgSearchResult EpgStore::search(const EpgSearchRequest &request, const Cancelled &cancelled) const
{
    EpgSearchResult result;
    result.request = request;
    try {
        if (cancelled && cancelled()) { result.status = EpgSearchStatus::Cancelled; return result; }
        std::shared_ptr<EpgStore> replacement;
        QString preparationError;
        { QMutexLocker lock(&m_mutex); replacement = m_searchReplacement; preparationError = m_searchPreparationError; }
        if (replacement) return replacement->search(request, cancelled);
        if (QUuid(request.profileId) != m_metadata.profileId) {
            result.status = EpgSearchStatus::Error; result.errorCode = QStringLiteral("profile-mismatch");
            result.errorText = QCoreApplication::translate("EpgSearch", "The active source changed. Search again."); return result;
        }
        if (!m_metadata.entries) { result.status = EpgSearchStatus::NoEpg; return result; }
        if (!m_searchReady) {
            if (!preparationError.isEmpty()) {
                result.status = EpgSearchStatus::Error; result.errorCode = preparationError;
                result.errorText = QCoreApplication::translate("EpgSearch", "Could not prepare the local EPG search index. Retry to prepare it again.");
                return result;
            }
            result.status = supportsFts5() ? EpgSearchStatus::Preparing : EpgSearchStatus::Unsupported;
            if (result.status == EpgSearchStatus::Unsupported) {
                result.errorCode = QStringLiteral("fts5-unavailable");
                result.errorText = QCoreApplication::translate("EpgSearch", "The SQLite plugin does not support EPG search (FTS5).");
            }
            return result;
        }
        const auto error = epgSearchQueryError(request.query);
        if (!error.isEmpty()) {
            if (error != QStringLiteral("query-too-short")) {
                result.status = EpgSearchStatus::Error; result.errorCode = error;
                result.errorText = EpgSearchDetail::queryErrorText(error);
            }
            return result;
        }
        if (!request.nowUtc.isValid()) {
            result.status = EpgSearchStatus::Error; result.errorCode = QStringLiteral("invalid-clock");
            result.errorText = QCoreApplication::translate("EpgSearch", "The search reference time is invalid. Search again."); return result;
        }
        const auto channels = EpgSearchDetail::channels(request);
        if (channels.isEmpty()) return result;
        QHash<QString, QList<int>> map;
        for (int i = 0; i < channels.size(); ++i) map[channels[i].tvgId.trimmed().toLower()].append(i);
        QStringList terms;
        for (const auto &token : epgSearchTokens(request.query)) {
            const auto quoted = u'"' + token + u'"';
            // Query validation still requires one >=2-character token, but
            // fragments after punctuation (Spider-M) must also match prefixes.
            terms += quoted + u'*';
        }
        const auto expression = terms.join(QStringLiteral(" AND "));
        EpgSearchDetail::Page page(request);
        Connection c(m_path);
        QSqlQuery boundary(c.db);
        const auto lastAtOrBefore = [&](qint64 time) {
            check(boundary.prepare(QStringLiteral("SELECT id FROM search_order WHERE start<=? ORDER BY start DESC,id DESC LIMIT 1")), boundary);
            boundary.addBindValue(time); check(boundary.exec(), boundary);
            return boundary.next() ? boundary.value(0).toLongLong() : qint64(0);
        };
        const auto nowMs = request.nowUtc.toMSecsSinceEpoch();
        const auto lastStarted = lastAtOrBefore(nowMs);
        const auto earliestCurrent = request.nowUtc.addMSecs(-m_maximumSearchDuration);
        const auto beforeCurrent = earliestCurrent.isValid() ? lastAtOrBefore(earliestCurrent.toMSecsSinceEpoch()) : 0;
        check(boundary.exec(QStringLiteral("SELECT COALESCE(MAX(id),0) FROM search_order")), boundary);
        check(boundary.next(), boundary);
        const auto lastDocument = boundary.value(0).toLongLong();
        for (int group = 0; group < 3; ++group) {
            if (!EpgSearchDetail::accepts(group, request.timeFilter)) continue;
            const qint64 lower = group == 0 ? beforeCurrent : group == 1 ? lastStarted : 0;
            const qint64 upper = group == 1 ? lastDocument : lastStarted;
            if (lower >= upper) continue;
            if (cancelled && cancelled()) { result.status = EpgSearchStatus::Cancelled; return result; }
            QSqlQuery q(c.db); q.setForwardOnly(true);
            // Read all title/subtitle matches in chronological rowid order.
            // Match quality only breaks timestamp ties; rank buckets would let
            // a later exact title displace an earlier prefix/subtitle match.
            check(q.prepare(QStringLiteral("SELECT p.channel,p.title,p.subtitle,'',p.episode,p.start,p.stop,d.title,d.identity "
                "FROM programme_search JOIN search_order o ON o.id=programme_search.rowid "
                "JOIN programmes p ON p.id=o.programme JOIN search_documents d ON d.programme=p.id "
                "WHERE programme_search MATCH ? AND programme_search.rowid>? AND programme_search.rowid<=? "
                "ORDER BY programme_search.rowid %1").arg(group == 2 ? QStringLiteral("DESC") : QStringLiteral("ASC"))), q);
            q.addBindValue(expression);
            q.addBindValue(lower); q.addBindValue(upper); check(q.exec(), q);
            while (q.next()) {
                if (cancelled && cancelled()) { result.status = EpgSearchStatus::Cancelled; return result; }
                const auto mapped = map.constFind(q.value(0).toString().trimmed().toLower());
                if (mapped == map.cend()) continue;
                const auto entry = readEntry(q);
                if (EpgSearchDetail::section(entry, request.nowUtc) != group) continue;
                const auto sortTime = group == 2 ? -entry.start.toMSecsSinceEpoch() : entry.start.toMSecsSinceEpoch();
                // Read every candidate at the boundary timestamp before cutting
                // a page, including all mapped channels and match-quality ties.
                if (page.full() && sortTime > page.worstTime()) return page.result();
                for (const auto index : *mapped)
                    page.append(entry, channels[index], index, q.value(8).toString(), q.value(7).toString());
            }
            if (q.lastError().isValid()) check(false, q);
            if (page.full()) return page.result();
        }
        if (cancelled && cancelled()) { result.status = EpgSearchStatus::Cancelled; return result; }
        return page.result();
    } catch (const std::exception &) {
        result.status = cancelled && cancelled() ? EpgSearchStatus::Cancelled : EpgSearchStatus::Error;
        result.errorCode = QStringLiteral("search-read-failed");
        result.errorText = QCoreApplication::translate("EpgSearch", "Could not read the local EPG search index.");
        if (result.status != EpgSearchStatus::Cancelled && !supportsFts5()) {
            result.status = EpgSearchStatus::Unsupported;
            result.errorCode = QStringLiteral("fts5-unavailable");
            result.errorText = QCoreApplication::translate("EpgSearch", "The SQLite plugin does not support EPG search (FTS5).");
        }
        return result;
    }
}
} // namespace OKILTV::Core
