#include "sqlitevodstore.h"
#include "storagecodec.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QSet>
#include <QVariant>
#include <QTimeZone>
#include <limits>

namespace OKILTV::Vod {
namespace {
QString uuid(const QUuid &id) { return id.toString(QUuid::WithoutBraces); }
QString processInstance() { static const auto instance = uuid(QUuid::createUuid()); return instance; }
QByteArray encodeArtwork(const QList<ArtworkRef> &artwork) {
    QJsonArray values;
    for (const auto &art : artwork) values.append(QJsonObject{{QStringLiteral("id"), art.id}, {QStringLiteral("role"), art.role}});
    return QJsonDocument(values).toJson(QJsonDocument::Compact);
}
QList<ArtworkRef> decodeArtwork(const QByteArray &bytes) {
    QList<ArtworkRef> result;
    for (const auto &value : QJsonDocument::fromJson(bytes).array()) {
        const auto object = value.toObject();
        result.append({object.value(QStringLiteral("id")).toString(), object.value(QStringLiteral("role")).toString(), {}});
    }
    return result;
}
QString sortKey(const QString &title)
{
    // Locale-independent base-letter ordering, also shared by search queries.
    // NFKD handles accents across languages; these Latin letters do not decompose.
    const auto decomposed = title.normalized(QString::NormalizationForm_KD).toCaseFolded()
        .normalized(QString::NormalizationForm_KD);
    QString result;
    result.reserve(decomposed.size());
    for (const auto scalar : decomposed.toUcs4()) {
        const auto codepoint = static_cast<char32_t>(scalar);
        const auto category = QChar::category(codepoint);
        if (category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining
            || category == QChar::Mark_Enclosing) continue;
        switch (codepoint) {
        case U'ł': result += u'l'; break;
        case U'ø': result += u'o'; break;
        case U'đ': case U'ð': result += u'd'; break;
        case U'ħ': result += u'h'; break;
        case U'ı': result += u'i'; break;
        case U'æ': result += QStringLiteral("ae"); break;
        case U'œ': result += QStringLiteral("oe"); break;
        case U'þ': result += QStringLiteral("th"); break;
        case U'ß': result += QStringLiteral("ss"); break;
        default: result += QString::fromUcs4(&codepoint, 1); break;
        }
    }
    return result;
}
QVariant optionalString(const std::optional<QString> &value) { return value ? QVariant(*value) : QVariant(QMetaType::fromType<QString>()); }
template<class T> QVariant optionalNumber(const std::optional<T> &value)
{ return value ? QVariant::fromValue(*value) : QVariant(QMetaType::fromType<qint64>()); }
struct Connection {
    QString name = uuid(QUuid::createUuid());
    QSqlDatabase db;
    RequestContext context;
    bool inTransaction = false;
    Connection(const QString &path, RequestContext request) : context(std::move(request))
    {
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=%1").arg(std::clamp(context.deadline.remainingTime(), qint64(1), qint64(1000))));
        // Do not throw until construction completes, so the named connection is
        // removed even on an open failure.
    }
    ~Connection() { if (inTransaction) db.rollback(); db.close(); db = {}; QSqlDatabase::removeDatabase(name); }
    [[noreturn]] void fail(const QSqlError &error, QStringView action) const
    {
        bool validCode = false;
        const auto code = error.nativeErrorCode().toInt(&validCode);
        // Driver text and SQL bindings may contain protected data. Log only
        // the numeric SQLite code, action and opaque operation identity.
        qWarning().noquote() << QStringLiteral("VOD SQL failure: operation=%1 action=%2 code=%3")
            .arg(uuid(context.operationId), action.toString(), validCode ? QString::number(code) : QStringLiteral("unknown"));
        const bool busy = validCode && ((code & 0xff) == 5 || (code & 0xff) == 6);
        throw Error{busy ? ErrorCode::StorageBusy : ErrorCode::StorageUnavailable, context.operationId, busy};
    }
    QSqlQuery sql(const QString &statement, const QVariantList &values = {})
    {
        QSqlQuery query(db);
        if (!query.prepare(statement)) fail(query.lastError(), u"prepare");
        for (const auto &value : values) query.addBindValue(value);
        if (!query.exec()) fail(query.lastError(), u"execute");
        return query;
    }
    void open()
    {
        if (const auto error = context.interruption()) throw *error;
        if (!db.open()) fail(db.lastError(), u"open");
        sql(QStringLiteral("PRAGMA foreign_keys=ON"));
    }
    void begin(bool write = true) { sql(write ? QStringLiteral("BEGIN IMMEDIATE") : QStringLiteral("BEGIN")); inTransaction = true; }
    void commit() { if (const auto error = context.interruption()) throw *error; sql(QStringLiteral("COMMIT")); inTransaction = false; }
    void current()
    {
        if (const auto error = context.interruption()) throw *error;
        auto state = sql(QStringLiteral("SELECT namespace,revision,removed,configuration_revision,mutation_pending FROM vod_source_state WHERE profile=?"), {uuid(context.source.profileId)});
        if (!state.next() || state.value(0).toString() != uuid(context.source.catalogNamespace)
            || state.value(1).toULongLong() != context.source.credentialRevision || state.value(2).toInt() != 0 || state.value(4).toInt() != 0)
            throw Error{ErrorCode::Cancelled, context.operationId};
    }
    void checkRef(const ContentRef &ref)
    {
        current();
        if (!ref.valid() || ref.profileId != context.source.profileId || ref.catalogNamespace != context.source.catalogNamespace)
            throw Error{ErrorCode::ContentUnavailable, context.operationId};
    }
    void checkScope(const CatalogScope &scope)
    {
        current();
        if (scope.profileId != context.source.profileId || scope.catalogNamespace != context.source.catalogNamespace
            || (scope.kind != CatalogKind::Movies && scope.kind != CatalogKind::Series) || (scope.categoryId && scope.categoryId->isEmpty()))
            throw Error{ErrorCode::ContentUnavailable, context.operationId};
    }
};
template<class T, class F> Result<T> guarded(const RequestContext &context, F work)
{
    try { return work(); }
    catch (const Error &error) { return error; }
    catch (...) { return Error{ErrorCode::StorageUnavailable, context.operationId}; }
}
int contentKind(CatalogKind kind) { return int(kind == CatalogKind::Movies ? ContentKind::Movie : ContentKind::Series); }
void checkImport(Connection &connection, const ImportToken &token, bool complete = false)
{
    connection.checkScope(token.scope);
    auto query = connection.sql(QStringLiteral("SELECT profile,namespace,kind,category,revision,valid,complete FROM vod_sync_runs WHERE id=?"), {uuid(token.id)});
    if (!query.next() || query.value(0).toString() != uuid(token.scope.profileId) || query.value(1).toString() != uuid(token.scope.catalogNamespace)
        || query.value(2).toInt() != contentKind(token.scope.kind) || query.value(3).toString() != token.scope.categoryId.value_or(QString())
        || query.value(4).toULongLong() != token.source.credentialRevision || query.value(5).toInt() != 1)
        throw Error{ErrorCode::Cancelled, token.request.operationId};
    if (complete && query.value(6).toInt() != 1) throw Error{ErrorCode::InvalidResponse, token.request.operationId};
}
void advanceGeneration(Connection &connection, const CatalogScope &scope)
{
    connection.sql(QStringLiteral("INSERT INTO vod_generations(profile,kind,generation,refreshed) VALUES(?,?,1,?) "
        "ON CONFLICT(profile,kind) DO UPDATE SET generation=generation+1,refreshed=excluded.refreshed"),
        {uuid(scope.profileId), contentKind(scope.kind), QDateTime::currentMSecsSinceEpoch()});
}
void eraseScope(Connection &connection, const CatalogScope &scope)
{
    if (const auto category = scope.categoryId) {
        connection.sql(QStringLiteral("DELETE FROM vod_item_categories WHERE category=? AND identity IN "
            "(SELECT identity FROM vod_items WHERE profile=? AND namespace=? AND kind=?)"),
            {*category, uuid(scope.profileId), uuid(scope.catalogNamespace), contentKind(scope.kind)});
        // Keep items still referenced by another category. History has no FK to
        // these cache rows and is unaffected by this deletion.
        connection.sql(QStringLiteral("DELETE FROM vod_items WHERE profile=? AND namespace=? AND kind=? "
            "AND identity NOT IN (SELECT identity FROM vod_item_categories)"),
            {uuid(scope.profileId), uuid(scope.catalogNamespace), contentKind(scope.kind)});
    } else connection.sql(QStringLiteral("DELETE FROM vod_items WHERE profile=? AND namespace=? AND kind=?"),
        {uuid(scope.profileId), uuid(scope.catalogNamespace), contentKind(scope.kind)});
}
}
SqliteVodStore::SqliteVodStore(QString databasePath, SourceLoader loader) : m_path(std::move(databasePath)), m_loader(std::move(loader)) {}
QString SqliteVodStore::backupPath() const { QMutexLocker lock(&m_stateMutex); return m_backupPath; }
Outcome SqliteVodStore::prepare(const RequestContext &context)
{
    QMutexLocker lock(&m_migrationMutex);
    if (m_prepared) {
        if (const auto error = context.interruption()) return *error;
        return Success{};
    }
    return guarded<Success>(context, [&]() -> Success {
        Connection connection(m_path, context); connection.open();
        auto exists = connection.sql(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='vod_schema_migrations'"));
        int version = 0;
        if (exists.next()) {
            auto query = connection.sql(QStringLiteral("SELECT COALESCE(MAX(version),0) FROM vod_schema_migrations"));
            query.next(); version = query.value(0).toInt();
        }
        exists.finish();
        if (version > schemaVersion) throw Error{ErrorCode::UnsupportedCapability, context.operationId};
        if (version == schemaVersion) {
            connection.begin();
            connection.sql(QStringLiteral("DELETE FROM vod_sync_runs WHERE owner<>?"), {processInstance()});
            connection.commit(); m_prepared = true; return {};
        }
        auto tables = connection.sql(QStringLiteral("SELECT count(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"));
        tables.next(); const bool populated = tables.value(0).toInt() > 0; tables.finish();
        if (populated) {
            const auto backup = m_path + QStringLiteral(".before-vod-%1.sqlite").arg(uuid(QUuid::createUuid()));
            connection.sql(QStringLiteral("VACUUM INTO ?"), {backup});
            QMutexLocker stateLock(&m_stateMutex); m_backupPath = backup;
        }
        connection.begin();
        // A second instance may have migrated between the initial inspection
        // and acquiring the writer lock. Recheck under that lock.
        auto marker = connection.sql(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='vod_schema_migrations'"));
        const bool hasMarker = marker.next(); marker.finish();
        if (hasMarker) {
            auto latest = connection.sql(QStringLiteral("SELECT COALESCE(MAX(version),0) FROM vod_schema_migrations"));
            latest.next(); version = latest.value(0).toInt(); latest.finish();
            if (version > schemaVersion) throw Error{ErrorCode::UnsupportedCapability, context.operationId};
        }
        const QStringList statements{
            QStringLiteral("CREATE TABLE IF NOT EXISTS vod_schema_migrations(version INTEGER PRIMARY KEY)"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS vod_source_state(profile TEXT PRIMARY KEY,namespace TEXT NOT NULL,revision INTEGER NOT NULL,removed INTEGER NOT NULL DEFAULT 0)"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS vod_generations(profile TEXT NOT NULL,kind INTEGER NOT NULL,generation INTEGER NOT NULL,refreshed INTEGER NOT NULL,PRIMARY KEY(profile,kind))"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS vod_items(identity BLOB PRIMARY KEY,profile TEXT NOT NULL,namespace TEXT NOT NULL,kind INTEGER NOT NULL,provider_id TEXT NOT NULL,parent TEXT NOT NULL,title TEXT NOT NULL,sort_key TEXT NOT NULL,year INTEGER,availability INTEGER NOT NULL,UNIQUE(profile,namespace,kind,provider_id,parent))"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS vod_items_page ON vod_items(profile,namespace,kind,sort_key,identity)"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS vod_item_categories(identity BLOB NOT NULL REFERENCES vod_items(identity) ON DELETE CASCADE,category TEXT NOT NULL,PRIMARY KEY(identity,category))"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS vod_categories_lookup ON vod_item_categories(category,identity)"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS vod_category_snapshots(profile TEXT NOT NULL,namespace TEXT NOT NULL,kind INTEGER NOT NULL,refreshed INTEGER NOT NULL,request TEXT NOT NULL,PRIMARY KEY(profile,namespace,kind))"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS vod_categories(profile TEXT NOT NULL,namespace TEXT NOT NULL,kind INTEGER NOT NULL,id TEXT NOT NULL,name TEXT NOT NULL,parent TEXT,ordering INTEGER NOT NULL,PRIMARY KEY(profile,namespace,kind,id),FOREIGN KEY(profile,namespace,kind) REFERENCES vod_category_snapshots(profile,namespace,kind) ON DELETE CASCADE)"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS vod_details(identity BLOB PRIMARY KEY,profile TEXT NOT NULL,payload BLOB NOT NULL,fetched INTEGER NOT NULL)"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS vod_sync_runs(id TEXT PRIMARY KEY,profile TEXT NOT NULL,namespace TEXT NOT NULL,kind INTEGER NOT NULL,category TEXT NOT NULL,revision INTEGER NOT NULL,valid INTEGER NOT NULL,complete INTEGER NOT NULL DEFAULT 0,owner TEXT NOT NULL DEFAULT '')"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS vod_seasons(series BLOB NOT NULL,profile TEXT NOT NULL,season TEXT NOT NULL,number INTEGER,ordering INTEGER NOT NULL,PRIMARY KEY(series,season))"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS vod_episode_links(episode BLOB PRIMARY KEY REFERENCES vod_items(identity) ON DELETE CASCADE,series BLOB NOT NULL,profile TEXT NOT NULL,season TEXT,number INTEGER,ordering INTEGER NOT NULL)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS vod_episode_order ON vod_episode_links(series,season,ordering)"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS vod_staging(run TEXT NOT NULL REFERENCES vod_sync_runs(id) ON DELETE CASCADE,identity BLOB NOT NULL,provider_id TEXT NOT NULL,parent TEXT NOT NULL,title TEXT NOT NULL,sort_key TEXT NOT NULL,year INTEGER,availability INTEGER NOT NULL,categories BLOB NOT NULL,PRIMARY KEY(run,identity))"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS vod_progress(identity BLOB PRIMARY KEY,profile TEXT NOT NULL,namespace TEXT NOT NULL,kind INTEGER NOT NULL,provider_id TEXT NOT NULL,parent TEXT NOT NULL,session TEXT NOT NULL,sequence INTEGER NOT NULL DEFAULT 0,position INTEGER,duration INTEGER,status INTEGER,content_revision TEXT,updated INTEGER,UNIQUE(profile,namespace,kind,provider_id,parent))")
        };
        for (const auto &statement : statements) connection.sql(statement);
        connection.sql(QStringLiteral("CREATE TABLE IF NOT EXISTS vod_movie_lists(identity BLOB PRIMARY KEY,profile TEXT NOT NULL,namespace TEXT NOT NULL,kind INTEGER NOT NULL,provider_id TEXT NOT NULL,parent TEXT NOT NULL,to_watch INTEGER NOT NULL DEFAULT 0,favourite INTEGER NOT NULL DEFAULT 0,UNIQUE(profile,namespace,kind,provider_id,parent))"));
        connection.sql(QStringLiteral("CREATE TABLE IF NOT EXISTS vod_series_history(series BLOB PRIMARY KEY,profile TEXT NOT NULL,namespace TEXT NOT NULL,provider_id TEXT NOT NULL,episode_id TEXT NOT NULL,updated INTEGER NOT NULL)"));
        connection.sql(QStringLiteral("CREATE INDEX IF NOT EXISTS vod_series_history_source ON vod_series_history(profile,namespace)"));
        connection.sql(QStringLiteral("CREATE TABLE IF NOT EXISTS vod_series_state(series BLOB PRIMARY KEY,profile TEXT NOT NULL,namespace TEXT NOT NULL,continue_hidden INTEGER NOT NULL DEFAULT 0,blocked_session TEXT NOT NULL DEFAULT '')"));
        connection.sql(QStringLiteral("CREATE TABLE IF NOT EXISTS vod_subtitles(identity BLOB PRIMARY KEY,profile TEXT NOT NULL,namespace TEXT NOT NULL,payload BLOB NOT NULL)"));
        if (version < 10) connection.sql(QStringLiteral("DROP VIEW IF EXISTS vod_series_continue"));
        connection.sql(QStringLiteral("CREATE VIEW IF NOT EXISTS vod_series_continue AS "
            "WITH available AS (SELECT l.*,s.number AS season_number, "
            "ROW_NUMBER() OVER (PARTITION BY l.series ORDER BY COALESCE(s.number,2147483647),COALESCE(s.ordering,2147483647),COALESCE(l.number,2147483647),l.ordering) AS queue_order "
            "FROM vod_episode_links l JOIN vod_items i ON i.identity=l.episode LEFT JOIN vod_seasons s ON s.series=l.series AND s.season=l.season "
            "WHERE i.availability<>2), last AS (SELECT h.series,h.updated,a.episode,a.queue_order,a.season_number,p.status,p.position,p.duration "
            "FROM vod_series_history h JOIN vod_items i ON i.profile=h.profile AND i.namespace=h.namespace AND i.kind=2 AND i.provider_id=h.episode_id AND i.parent=h.provider_id "
            "JOIN available a ON a.episode=i.identity LEFT JOIN vod_progress p ON p.identity=i.identity LEFT JOIN vod_series_state state ON state.series=h.series WHERE COALESCE(state.continue_hidden,0)=0) "
            "SELECT series,updated,CASE WHEN COALESCE(status,0)=0 AND (duration IS NULL OR duration<=0 OR COALESCE(position,0)<=duration*0.95) THEN episode "
            "WHEN COALESCE(season_number,-1)<>0 THEN (SELECT a.episode FROM available a WHERE a.series=last.series AND COALESCE(a.season_number,-1)<>0 AND a.queue_order>last.queue_order ORDER BY a.queue_order LIMIT 1) END AS episode FROM last"));
        connection.sql(QStringLiteral("CREATE INDEX IF NOT EXISTS vod_movie_lists_source ON vod_movie_lists(profile,namespace,kind)"));
        auto progressColumns = connection.sql(QStringLiteral("PRAGMA table_info(vod_progress)"));
        bool hasTrackPreferences = false;
        while (progressColumns.next())
            hasTrackPreferences = hasTrackPreferences || progressColumns.value(1).toString() == QStringLiteral("track_preferences");
        progressColumns.finish();
        if (!hasTrackPreferences)
            connection.sql(QStringLiteral("ALTER TABLE vod_progress ADD COLUMN track_preferences BLOB NOT NULL DEFAULT '{}'"));
        auto columns = connection.sql(QStringLiteral("PRAGMA table_info(vod_sync_runs)"));
        bool hasOwner = false;
        while (columns.next()) hasOwner = hasOwner || columns.value(1).toString() == QStringLiteral("owner");
        columns.finish();
        if (!hasOwner) connection.sql(QStringLiteral("ALTER TABLE vod_sync_runs ADD COLUMN owner TEXT NOT NULL DEFAULT ''"));
        auto sourceColumns = connection.sql(QStringLiteral("PRAGMA table_info(vod_source_state)"));
        bool hasConfigurationRevision = false;
        bool hasMutationPending = false;
        while (sourceColumns.next()) {
            hasConfigurationRevision = hasConfigurationRevision || sourceColumns.value(1).toString() == QStringLiteral("configuration_revision");
            hasMutationPending = hasMutationPending || sourceColumns.value(1).toString() == QStringLiteral("mutation_pending");
        }
        sourceColumns.finish();
        if (!hasConfigurationRevision) {
            connection.sql(QStringLiteral("ALTER TABLE vod_source_state ADD COLUMN configuration_revision INTEGER NOT NULL DEFAULT 1"));
            connection.sql(QStringLiteral("UPDATE vod_source_state SET configuration_revision=revision"));
        }
        if (!hasMutationPending)
            connection.sql(QStringLiteral("ALTER TABLE vod_source_state ADD COLUMN mutation_pending INTEGER NOT NULL DEFAULT 0"));
        for (const auto &table : {QStringLiteral("vod_items"), QStringLiteral("vod_staging")}) {
            auto fields = connection.sql(QStringLiteral("PRAGMA table_info(%1)").arg(table));
            bool hasArtwork = false;
            while (fields.next()) hasArtwork = hasArtwork || fields.value(1).toString() == QStringLiteral("artwork");
            fields.finish();
            if (!hasArtwork) connection.sql(QStringLiteral("ALTER TABLE %1 ADD COLUMN artwork BLOB NOT NULL DEFAULT '[]'").arg(table));
        }
        connection.sql(QStringLiteral("DELETE FROM vod_sync_runs WHERE owner<>?"), {processInstance()});
        if (version < 9) {
            auto history=connection.sql(QStringLiteral("SELECT profile,namespace,parent,provider_id,COALESCE(updated,0) FROM (SELECT *,ROW_NUMBER() OVER (PARTITION BY profile,namespace,parent ORDER BY COALESCE(updated,0) DESC,identity DESC) AS recent FROM vod_progress WHERE kind=2 AND parent<>'' AND position IS NOT NULL) WHERE recent=1"));
            while (history.next()) {
                const ContentRef series{QUuid(history.value(0).toString()),QUuid(history.value(1).toString()),ContentKind::Series,history.value(2).toString(),{}};
                connection.sql(QStringLiteral("INSERT OR IGNORE INTO vod_series_history(series,profile,namespace,provider_id,episode_id,updated) VALUES(?,?,?,?,?,?)"),
                    {series.key(),history.value(0),history.value(1),history.value(2),history.value(3),history.value(4)});
            }
            history.finish();
        }
        if (version < 7) {
            // Bound memory and release read queries before updating their rows.
            // The enclosing transaction rolls back every key and generation on failure.
            for (const auto &table : {QStringLiteral("vod_items"), QStringLiteral("vod_staging")}) {
                qint64 lastRow = 0;
                for (;;) {
                    if (const auto error = context.interruption()) throw *error;
                    auto rows = connection.sql(QStringLiteral("SELECT rowid,title FROM %1 WHERE rowid>? ORDER BY rowid LIMIT 256").arg(table), {lastRow});
                    QList<QPair<qint64, QString>> batch;
                    while (rows.next()) batch.append({rows.value(0).toLongLong(), sortKey(rows.value(1).toString())});
                    rows.finish();
                    if (batch.isEmpty()) break;
                    for (const auto &[row, key] : batch)
                        connection.sql(QStringLiteral("UPDATE %1 SET sort_key=? WHERE rowid=?").arg(table), {key, row});
                    lastRow = batch.last().first;
                }
            }
            connection.sql(QStringLiteral("UPDATE vod_generations SET generation=generation+1"));
        }
        connection.sql(QStringLiteral("INSERT OR IGNORE INTO vod_schema_migrations(version) VALUES(?)"), {schemaVersion});
        connection.commit();
        m_prepared = true;
        return {};
    });
}
Result<SourceContext> SqliteVodStore::snapshot(const QUuid &id)
{
    quint64 epoch = 0;
    { QMutexLocker lock(&m_stateMutex); epoch = m_epochs.value(id); }
    RequestContext context;
    const auto migrated = prepare(context);
    if (const auto *error = std::get_if<Error>(&migrated)) return *error;
    return guarded<SourceContext>(context, [&]() {
        if (!m_loader) throw Error{ErrorCode::SecretUnavailable, context.operationId};
        auto loaded = m_loader(id);
        if (const auto *error = std::get_if<Error>(&loaded)) throw *error;
        auto source = std::get<SourceContext>(loaded);
        if (source.revision.profileId != id || id.isNull() || source.revision.credentialRevision == 0
            || source.revision.credentialRevision > quint64(std::numeric_limits<qint64>::max()))
            throw Error{ErrorCode::InvalidResponse, context.operationId};
        // Disabled sources remain readable for current-session progress/recovery.
        // Network and new playback admission belong to VodController.
        Connection connection(m_path, context); connection.open(); connection.begin(false);
        const auto configuredRevision = source.revision.credentialRevision;
        const auto readState = [&]() {
            auto query = connection.sql(QStringLiteral("SELECT namespace,revision,removed,configuration_revision,mutation_pending FROM vod_source_state WHERE profile=?"), {uuid(id)});
            if (!query.next()) {
                source.revision.catalogNamespace = QUuid::createUuid();
                source.revision.credentialRevision = configuredRevision;
                return false;
            }
            if (query.value(2).toInt() != 0 || query.value(4).toInt() != 0
                || query.value(3).toULongLong() > configuredRevision)
                throw Error{ErrorCode::Cancelled, context.operationId};
            source.revision.catalogNamespace = QUuid(query.value(0).toString());
            const auto storedRevision = query.value(1).toULongLong();
            if (query.value(3).toULongLong() == configuredRevision) {
                source.revision.credentialRevision = storedRevision;
                return true;
            }
            if (storedRevision >= quint64(std::numeric_limits<qint64>::max()))
                throw Error{ErrorCode::StorageUnavailable, context.operationId};
            source.revision.credentialRevision = std::max(configuredRevision, storedRevision + 1);
            return false;
        };
        const bool unchanged = readState();
        // Release shared SQL locks before taking the state mutex: a writer
        // holding that mutex may be waiting for readers to finish its commit.
        connection.commit();
        if (unchanged) {
            QMutexLocker lock(&m_stateMutex);
            if (m_epochs.value(id) != epoch) throw Error{ErrorCode::Cancelled, context.operationId};
            m_current[id] = source.revision;
            return source;
        }
        // Re-read after acquiring a write reservation: another snapshot may
        // have created or reconciled this source while we waited. Serialize
        // publication with invalidate(), including this write transaction.
        connection.begin();
        QMutexLocker lock(&m_stateMutex);
        if (m_epochs.value(id) != epoch) throw Error{ErrorCode::Cancelled, context.operationId};
        if (!readState()) {
            connection.sql(QStringLiteral("INSERT INTO vod_source_state(profile,namespace,revision,configuration_revision) VALUES(?,?,?,?) "
                "ON CONFLICT(profile) DO UPDATE SET revision=excluded.revision,configuration_revision=excluded.configuration_revision"),
                {uuid(id), uuid(source.revision.catalogNamespace), QVariant::fromValue(source.revision.credentialRevision), QVariant::fromValue(configuredRevision)});
        }
        connection.commit();
        m_current[id] = source.revision;
        return source;
    });
}
bool SqliteVodStore::isCurrent(const SourceRevision &source)
{ QMutexLocker lock(&m_stateMutex); return m_current.contains(source.profileId) && m_current.value(source.profileId) == source; }
Result<QList<QUuid>> SqliteVodStore::reconcileRemovedSources(const QList<QUuid> &existingProfiles)
{
    RequestContext context;
    const auto migrated = prepare(context);
    if (const auto *error = std::get_if<Error>(&migrated)) return *error;
    return guarded<QList<QUuid>>(context, [&]() {
        QList<QUuid> removed;
        {
            Connection connection(m_path, context); connection.open();
            auto rows = connection.sql(QStringLiteral("SELECT profile,removed FROM vod_source_state"));
            while (rows.next()) {
                const QUuid id(rows.value(0).toString());
                if (rows.value(1).toInt() != 0 || !existingProfiles.contains(id)) removed.append(id);
            }
        }
        for (const auto &id : removed) {
            for (const auto &step : {std::function<Outcome()>([&]() { return prepareRemoval(id); }),
                     std::function<Outcome()>([&]() { return removeSourceState(id); }),
                     std::function<Outcome()>([&]() { return finishRemoval(id); })}) {
                if (const auto result = step(); std::holds_alternative<Error>(result)) throw std::get<Error>(result);
            }
        }
        for (const auto &id : existingProfiles) {
            if (removed.contains(id)) continue;
            const auto completed = finishCredentialChange(id);
            if (const auto *error = std::get_if<Error>(&completed)) throw *error;
        }
        return removed;
    });
}
void SqliteVodStore::invalidate(const QUuid &id) { QMutexLocker lock(&m_stateMutex); ++m_epochs[id]; m_current.remove(id); }
Outcome SqliteVodStore::advanceCredentialRevision(const QUuid &id, quint64 revision)
{
    invalidate(id);
    RequestContext context;
    const auto migrated = prepare(context);
    if (const auto *error = std::get_if<Error>(&migrated)) return *error;
    const auto result = guarded<Success>(context, [&]() -> Success {
        if (id.isNull() || revision == 0 || revision > quint64(std::numeric_limits<qint64>::max()))
            throw Error{ErrorCode::InvalidResponse, context.operationId};
        Connection connection(m_path, context); connection.open(); connection.begin();
        auto current = connection.sql(QStringLiteral("SELECT revision,removed,mutation_pending FROM vod_source_state WHERE profile=?"), {uuid(id)});
        if (current.next()) {
            if (current.value(1).toInt() != 0 || current.value(2).toInt() != 0)
                throw Error{ErrorCode::Cancelled, context.operationId};
            const auto storedRevision = current.value(0).toULongLong();
            if (storedRevision >= quint64(std::numeric_limits<qint64>::max()))
                throw Error{ErrorCode::StorageUnavailable, context.operationId};
            revision = std::max(revision, storedRevision + 1);
        }
        current.finish();
        connection.sql(QStringLiteral("INSERT INTO vod_source_state(profile,namespace,revision,mutation_pending) VALUES(?,?,?,1) "
            "ON CONFLICT(profile) DO UPDATE SET revision=excluded.revision,mutation_pending=1"),
            {uuid(id), uuid(QUuid::createUuid()), QVariant::fromValue(revision)});
        connection.sql(QStringLiteral("UPDATE vod_sync_runs SET valid=0 WHERE profile=?"), {uuid(id)});
        connection.commit();
        return {};
    });
    invalidate(id);
    return result;
}
Outcome SqliteVodStore::finishCredentialChange(const QUuid &id)
{
    invalidate(id);
    RequestContext context;
    const auto migrated = prepare(context);
    if (const auto *error = std::get_if<Error>(&migrated)) return *error;
    const auto result = guarded<Success>(context, [&]() -> Success {
        Connection connection(m_path, context); connection.open(); connection.begin();
        auto pending = connection.sql(QStringLiteral("SELECT mutation_pending,removed FROM vod_source_state WHERE profile=?"), {uuid(id)});
        if (!pending.next() || pending.value(0).toInt() == 0 || pending.value(1).toInt() != 0) return {};
        pending.finish();
        if (!m_loader) throw Error{ErrorCode::SecretUnavailable, context.operationId};
        const auto loaded = m_loader(id);
        if (const auto *error = std::get_if<Error>(&loaded)) throw *error;
        const auto &source = std::get<SourceContext>(loaded);
        const auto revision = source.revision.credentialRevision;
        if (source.revision.profileId != id || revision == 0 || revision > quint64(std::numeric_limits<qint64>::max()))
            throw Error{ErrorCode::InvalidResponse, context.operationId};
        // Keep the advanced request revision even when the configuration write
        // failed. Mapping the actual saved revision cannot revive an old writer.
        connection.sql(QStringLiteral("UPDATE vod_source_state SET configuration_revision=?,mutation_pending=0 WHERE profile=?"),
            {QVariant::fromValue(revision), uuid(id)});
        connection.commit();
        return {};
    });
    invalidate(id);
    return result;
}
Outcome SqliteVodStore::prepareRemoval(const QUuid &id)
{
    invalidate(id);
    RequestContext context;
    const auto migrated = prepare(context);
    if (const auto *error = std::get_if<Error>(&migrated)) return *error;
    return guarded<Success>(context, [&]() -> Success {
        Connection connection(m_path, context); connection.open(); connection.begin();
        connection.sql(QStringLiteral("INSERT INTO vod_source_state(profile,namespace,revision,removed) VALUES(?,?,1,1) "
            "ON CONFLICT(profile) DO UPDATE SET removed=1,revision=revision+1"), {uuid(id), uuid(QUuid::createUuid())});
        connection.sql(QStringLiteral("UPDATE vod_sync_runs SET valid=0 WHERE profile=?"), {uuid(id)});
        connection.commit(); return {};
    });
}
Outcome SqliteVodStore::finishRemoval(const QUuid &id)
{
    return guarded<Success>({}, [&]() -> Success {
        Connection connection(m_path, {}); connection.open(); connection.begin();
        // Keep a minimal tombstone: a late snapshot must never recreate this ID.
        connection.sql(QStringLiteral("UPDATE vod_source_state SET removed=2 WHERE profile=? AND removed=1"), {uuid(id)});
        connection.commit(); invalidate(id); return {};
    });
}
Result<QList<QUuid>> SqliteVodStore::pendingRemovals()
{
    return guarded<QList<QUuid>>({}, [&]() {
        Connection connection(m_path, {}); connection.open();
        auto query = connection.sql(QStringLiteral("SELECT profile FROM vod_source_state WHERE removed=1"));
        QList<QUuid> result;
        while (query.next()) result.append(QUuid(query.value(0).toString()));
        return result;
    });
}
Result<ImportToken> SqliteVodStore::beginRefresh(const CatalogScope &scope, const RequestContext &context)
{
    return guarded<ImportToken>(context, [&]() {
        Connection connection(m_path, context); connection.open(); connection.begin(); connection.checkScope(scope);
        const auto category = scope.categoryId.value_or(QStringLiteral(""));
        connection.sql(QStringLiteral("UPDATE vod_sync_runs SET valid=0 WHERE profile=? AND kind=? AND (?='' OR category='' OR category=?)"),
            {uuid(scope.profileId), contentKind(scope.kind), category, category});
        connection.sql(QStringLiteral("DELETE FROM vod_sync_runs WHERE profile=? AND valid=0"), {uuid(scope.profileId)});
        ImportToken token{QUuid::createUuid(), scope, context.source, context};
        connection.sql(QStringLiteral("INSERT INTO vod_sync_runs(id,profile,namespace,kind,category,revision,valid,owner) VALUES(?,?,?,?,?,?,1,?)"),
            {uuid(token.id), uuid(scope.profileId), uuid(scope.catalogNamespace), contentKind(scope.kind), category,
             QVariant::fromValue(context.source.credentialRevision), processInstance()});
        connection.commit(); return token;
    });
}
Outcome SqliteVodStore::stageBatch(const ImportToken &token, const CatalogBatch &batch)
{
    return guarded<Success>(token.request, [&]() -> Success {
        Connection connection(m_path, token.request); connection.open(); connection.begin(); checkImport(connection, token);
        if (batch.scope != token.scope || (batch.complete && batch.next) || (!batch.complete && !batch.next))
            throw Error{ErrorCode::InvalidResponse, token.request.operationId};
        for (const auto &item : batch.items) std::visit([&](const auto &summary) {
            connection.checkRef(summary.ref);
            if (int(summary.ref.kind) != contentKind(batch.scope.kind)
                || (batch.scope.categoryId && !summary.categoryIds.contains(*batch.scope.categoryId)))
                throw Error{ErrorCode::InvalidResponse, token.request.operationId};
            connection.sql(QStringLiteral("INSERT INTO vod_staging(run,identity,provider_id,parent,title,sort_key,year,availability,categories,artwork) VALUES(?,?,?,?,?,?,?,?,?,?)"),
                {uuid(token.id), summary.ref.key(), summary.ref.providerItemId, summary.ref.parentNamespace.value_or(QStringLiteral("")),
                 summary.title, sortKey(summary.title), optionalNumber(summary.year), int(summary.availability),
                 QJsonDocument(QJsonArray::fromStringList(summary.categoryIds)).toJson(QJsonDocument::Compact), encodeArtwork(summary.artwork)});
        }, item);
        connection.sql(QStringLiteral("UPDATE vod_sync_runs SET complete=? WHERE id=?"), {batch.complete ? 1 : 0, uuid(token.id)});
        connection.current(); connection.commit(); return {};
    });
}
Result<quint64> SqliteVodStore::publishIfCurrent(const ImportToken &token, bool complete)
{
    return guarded<quint64>(token.request, [&]() -> quint64 {
        Connection connection(m_path, token.request); connection.open(); connection.begin(); checkImport(connection, token, true);
        if (!complete) throw Error{ErrorCode::InvalidResponse, token.request.operationId};
        eraseScope(connection, token.scope);
        auto rows = connection.sql(QStringLiteral("SELECT identity,provider_id,parent,title,sort_key,year,availability,categories,artwork FROM vod_staging WHERE run=?"), {uuid(token.id)});
        while (rows.next()) {
            if (const auto error = token.request.interruption()) throw *error;
            connection.sql(QStringLiteral("INSERT INTO vod_items(identity,profile,namespace,kind,provider_id,parent,title,sort_key,year,availability,artwork) VALUES(?,?,?,?,?,?,?,?,?,?,?) "
                "ON CONFLICT(identity) DO UPDATE SET title=excluded.title,sort_key=excluded.sort_key,year=excluded.year,availability=excluded.availability,artwork=excluded.artwork"),
                {rows.value(0), uuid(token.scope.profileId), uuid(token.scope.catalogNamespace), contentKind(token.scope.kind),
                 rows.value(1), rows.value(2), rows.value(3), rows.value(4), rows.value(5), rows.value(6), rows.value(8)});
            // Category-scoped publication only replaces membership in its scope.
            auto categories = QJsonDocument::fromJson(rows.value(7).toByteArray()).array();
            if (token.scope.categoryId) categories = QJsonArray{*token.scope.categoryId};
            for (const auto &category : categories) connection.sql(QStringLiteral("INSERT OR IGNORE INTO vod_item_categories(identity,category) VALUES(?,?)"), {rows.value(0), category.toString()});
        }
        rows.finish();
        advanceGeneration(connection, token.scope);
        std::unique_lock<std::recursive_mutex> policyLock;
        if (token.request.policyMutex) policyLock = std::unique_lock<std::recursive_mutex>(*token.request.policyMutex);
        connection.current();
        auto generation = connection.sql(QStringLiteral("SELECT generation FROM vod_generations WHERE profile=? AND kind=?"), {uuid(token.scope.profileId), contentKind(token.scope.kind)});
        generation.next(); const auto number = generation.value(0).toULongLong(); generation.finish();
        connection.sql(QStringLiteral("DELETE FROM vod_sync_runs WHERE id=?"), {uuid(token.id)});
        connection.commit(); return number;
    });
}
Outcome SqliteVodStore::abandonRefresh(const ImportToken &token) noexcept
{
    return guarded<Success>({}, [&]() -> Success {
        Connection connection(m_path, {}); connection.open(); connection.begin();
        connection.sql(QStringLiteral("DELETE FROM vod_sync_runs WHERE id=?"), {uuid(token.id)});
        connection.commit(); return {};
    });
}
Result<CatalogPage> SqliteVodStore::query(const CatalogQuery &input, const RequestContext &context)
{
    return guarded<CatalogPage>(context, [&]() {
        Connection connection(m_path, context); connection.open(); connection.begin(false); connection.checkScope(input.scope);
        if (input.pageSize < 1 || input.pageSize > 1000) throw Error{ErrorCode::InvalidResponse, context.operationId};
        CatalogPage page; page.scope = input.scope;
        auto generation = connection.sql(QStringLiteral("SELECT generation,refreshed FROM vod_generations WHERE profile=? AND kind=?"), {uuid(input.scope.profileId), contentKind(input.scope.kind)});
        if (generation.next()) { page.generation = generation.value(0).toULongLong(); page.refreshedAtUtc = QDateTime::fromMSecsSinceEpoch(generation.value(1).toLongLong(), QTimeZone::UTC); }
        generation.finish();
        if (input.page && input.page->generation != page.generation) throw Error{ErrorCode::Cancelled, context.operationId};
        const bool seriesContinue = input.continueWatchingOnly && input.scope.kind == CatalogKind::Series;
        const auto orderKey = seriesContinue ? QStringLiteral("(SELECT updated FROM vod_series_continue WHERE series=vod_items.identity)") : input.continueWatchingOnly
            ? QStringLiteral("(SELECT COALESCE(updated,0) FROM vod_progress WHERE vod_progress.identity=vod_items.identity)")
            : QStringLiteral("sort_key");
        QString statement = QStringLiteral("SELECT identity,provider_id,parent,title,year,availability,%1,artwork,COALESCE((SELECT to_watch FROM vod_movie_lists WHERE vod_movie_lists.identity=vod_items.identity),0),COALESCE((SELECT favourite FROM vod_movie_lists WHERE vod_movie_lists.identity=vod_items.identity),0) FROM vod_items WHERE profile=? AND namespace=? AND kind=?").arg(orderKey);
        QVariantList args{uuid(input.scope.profileId), uuid(input.scope.catalogNamespace), contentKind(input.scope.kind)};
        if (input.identity) { statement += QStringLiteral(" AND identity=?"); args.append(*input.identity); }
        if (const auto category = input.scope.categoryId) {
            statement += QStringLiteral(" AND identity IN (SELECT identity FROM vod_item_categories WHERE category=?)"); args.append(*category);
        }
        if (input.allowedCategories) {
            if (input.allowedCategories->isEmpty()) statement += QStringLiteral(" AND 0");
            else {
                QStringList placeholders;
                for (const auto &category : *input.allowedCategories) { placeholders.append(QStringLiteral("?")); args.append(category); }
                statement += QStringLiteral(" AND identity IN (SELECT identity FROM vod_item_categories WHERE category IN (%1))").arg(placeholders.join(u','));
            }
        }
        if (seriesContinue) statement += QStringLiteral(" AND identity IN (SELECT series FROM vod_series_continue WHERE episode IS NOT NULL)");
        else if (input.continueWatchingOnly)
            statement += QStringLiteral(" AND identity IN (SELECT identity FROM vod_progress WHERE status=0 AND position>0 AND (duration IS NULL OR duration<=0 OR position<=duration*0.95))");
        if (input.movieList != MovieList::None) {
            if ((input.movieList != MovieList::ToWatch && input.movieList != MovieList::Favourites))
                throw Error{ErrorCode::InvalidResponse, context.operationId};
            statement += input.movieList == MovieList::ToWatch
                ? QStringLiteral(" AND identity IN (SELECT identity FROM vod_movie_lists WHERE to_watch=1)")
                : QStringLiteral(" AND identity IN (SELECT identity FROM vod_movie_lists WHERE favourite=1)");
        }
        const auto prefix = sortKey(input.titlePrefix);
        if (!prefix.isEmpty()) {
            statement += QStringLiteral(" AND sort_key>=? AND substr(sort_key,1,length(?))=?");
            args.append(prefix); args.append(prefix); args.append(prefix);
        }
        const auto contains = sortKey(input.titleContains);
        if (!contains.isEmpty()) { statement += QStringLiteral(" AND instr(sort_key,?)>0"); args.append(contains); }
        const bool descending = input.continueWatchingOnly || input.sort == CatalogSort::TitleDescending;
        if (const auto cursor = input.page) {
            statement += (descending ? QStringLiteral(" AND (%1,identity)<(?,?)") : QStringLiteral(" AND (%1,identity)>(?,?)")).arg(orderKey);
            args.append(input.continueWatchingOnly ? QVariant(cursor->lastSortKey.toLongLong()) : QVariant(cursor->lastSortKey));
            args.append(cursor->lastIdentity);
        }
        statement += (descending ? QStringLiteral(" ORDER BY %1 DESC,identity DESC LIMIT ?") : QStringLiteral(" ORDER BY %1,identity LIMIT ?")).arg(orderKey);
        args.append(input.pageSize + 1);
        auto rows = connection.sql(statement, args);
        LocalPageToken last;
        while (rows.next()) {
            if (page.items.size() == input.pageSize) { page.next = last; break; }
            MovieSummary summary;
            summary.ref = {input.scope.profileId, input.scope.catalogNamespace, ContentKind(contentKind(input.scope.kind)), rows.value(1).toString(), {}};
            if (!rows.value(2).toString().isEmpty()) summary.ref.parentNamespace = rows.value(2).toString();
            summary.title = rows.value(3).toString();
            if (!rows.value(4).isNull()) summary.year = rows.value(4).toInt();
            summary.availability = Availability(rows.value(5).toInt());
            summary.artwork = decodeArtwork(rows.value(7).toByteArray());
            page.movieLists.insert(summary.ref.key(), {rows.value(8).toBool(), rows.value(9).toBool()});
            if (input.scope.kind==CatalogKind::Series) {
                auto target=connection.sql(QStringLiteral("SELECT COALESCE(s.number,0),COALESCE(l.number,0),i.title FROM vod_series_continue v JOIN vod_items i ON i.identity=v.episode JOIN vod_episode_links l ON l.episode=i.identity LEFT JOIN vod_seasons s ON s.series=l.series AND s.season=l.season WHERE v.series=?"), {summary.ref.key()});
                if (target.next()) page.continuationLabels.insert(summary.ref.key(),QStringLiteral("S%1E%2 · %3").arg(target.value(0).toInt(),2,10,QChar('0')).arg(target.value(1).toInt(),2,10,QChar('0')).arg(target.value(2).toString()));
            }
            auto categories = connection.sql(QStringLiteral("SELECT category FROM vod_item_categories WHERE identity=? ORDER BY category"), {rows.value(0)});
            while (categories.next()) summary.categoryIds.append(categories.value(0).toString());
            if (input.scope.kind == CatalogKind::Movies) page.items.append(summary);
            else page.items.append(SeriesSummary{summary.ref, summary.title, summary.year, summary.artwork, summary.categoryIds, summary.availability});
            last = {page.generation, rows.value(6).toString(), rows.value(0).toByteArray()};
        }
        rows.finish(); connection.commit(); return page;
    });
}
Result<std::optional<CategorySnapshot>> SqliteVodStore::readCategories(const CatalogScope &scope, const RequestContext &context)
{
    return guarded<std::optional<CategorySnapshot>>(context, [&]() -> std::optional<CategorySnapshot> {
        Connection connection(m_path, context); connection.open(); connection.begin(false); connection.checkScope(scope);
        if (scope.categoryId) throw Error{ErrorCode::InvalidResponse, context.operationId};
        const QVariantList key{uuid(scope.profileId), uuid(scope.catalogNamespace), contentKind(scope.kind)};
        auto stamp = connection.sql(QStringLiteral("SELECT refreshed FROM vod_category_snapshots WHERE profile=? AND namespace=? AND kind=? AND refreshed>0"), key);
        if (!stamp.next()) return {};
        CategorySnapshot snapshot{scope, {}, QDateTime::fromMSecsSinceEpoch(stamp.value(0).toLongLong(), QTimeZone::UTC)};
        stamp.finish();
        auto rows = connection.sql(QStringLiteral("SELECT id,name,parent FROM vod_categories WHERE profile=? AND namespace=? AND kind=? ORDER BY ordering"), key);
        while (rows.next()) {
            if (const auto error = context.interruption()) throw *error;
            VodCategory category{scope, rows.value(0).toString(), rows.value(1).toString(), {}};
            if (!rows.value(2).isNull()) category.parentId = rows.value(2).toString();
            snapshot.categories.append(std::move(category));
        }
        rows.finish(); connection.commit(); return snapshot;
    });
}
Outcome SqliteVodStore::beginCategoryRefresh(const CatalogScope &scope, const RequestContext &context)
{
    return guarded<Success>(context, [&]() -> Success {
        Connection connection(m_path, context); connection.open(); connection.begin(); connection.checkScope(scope);
        if (scope.categoryId || context.operationId.isNull()) throw Error{ErrorCode::InvalidResponse, context.operationId};
        connection.sql(QStringLiteral("INSERT INTO vod_category_snapshots(profile,namespace,kind,refreshed,request) VALUES(?,?,?,0,?) "
            "ON CONFLICT(profile,namespace,kind) DO UPDATE SET request=excluded.request"),
            {uuid(scope.profileId), uuid(scope.catalogNamespace), contentKind(scope.kind), uuid(context.operationId)});
        connection.commit(); return {};
    });
}
Outcome SqliteVodStore::storeCategories(const CategorySnapshot &snapshot, const RequestContext &context)
{
    return guarded<Success>(context, [&]() -> Success {
        Connection connection(m_path, context); connection.open(); connection.begin(); connection.checkScope(snapshot.scope);
        if (snapshot.scope.categoryId || !snapshot.refreshedAtUtc.isValid() || snapshot.refreshedAtUtc.toMSecsSinceEpoch() <= 0) throw Error{ErrorCode::InvalidResponse, context.operationId};
        const QVariantList key{uuid(snapshot.scope.profileId), uuid(snapshot.scope.catalogNamespace), contentKind(snapshot.scope.kind)};
        auto current = connection.sql(QStringLiteral("SELECT request FROM vod_category_snapshots WHERE profile=? AND namespace=? AND kind=?"), key);
        if (!current.next() || current.value(0).toString() != uuid(context.operationId)) throw Error{ErrorCode::Cancelled, context.operationId};
        current.finish();
        connection.sql(QStringLiteral("DELETE FROM vod_categories WHERE profile=? AND namespace=? AND kind=?"), key);
        QVariantList values{snapshot.refreshedAtUtc.toMSecsSinceEpoch(), QStringLiteral("")}; values.append(key);
        connection.sql(QStringLiteral("UPDATE vod_category_snapshots SET refreshed=?,request=? WHERE profile=? AND namespace=? AND kind=?"), values);
        QSet<QString> ids;
        int order = 0;
        for (const auto &category : snapshot.categories) {
            if (const auto error = context.interruption()) throw *error;
            if (category.scope != snapshot.scope || category.id.isEmpty() || ids.contains(category.id))
                throw Error{ErrorCode::InvalidResponse, context.operationId};
            ids.insert(category.id);
            values = key; values.append(category.id); values.append(category.name);
            values.append(optionalString(category.parentId)); values.append(order++);
            connection.sql(QStringLiteral("INSERT INTO vod_categories(profile,namespace,kind,id,name,parent,ordering) VALUES(?,?,?,?,?,?,?)"), values);
        }
        // Take policy after the database write lock, as in movie publication.
        // Holding it while waiting for SQLite would invert the lock order when
        // different sources synchronize concurrently.
        std::unique_lock<std::recursive_mutex> policyLock;
        if (context.policyMutex) policyLock = std::unique_lock<std::recursive_mutex>(*context.policyMutex);
        connection.current(); connection.commit(); return {};
    });
}
Result<std::optional<VodDetails>> SqliteVodStore::readDetails(const ContentRef &ref, const RequestContext &context)
{
    return guarded<std::optional<VodDetails>>(context, [&]() -> std::optional<VodDetails> {
        Connection connection(m_path, context); connection.open(); connection.begin(false); connection.checkRef(ref);
        auto query = connection.sql(QStringLiteral("SELECT payload FROM vod_details WHERE identity=? AND fetched>=?"), {ref.key(), QDateTime::currentMSecsSinceEpoch() - qint64(168) * 3600 * 1000});
        if (!query.next()) return {};
        const auto details = Storage::decodeDetails(query.value(0).toByteArray());
        if (!details || details->ref != ref) throw Error{ErrorCode::StorageUnavailable, context.operationId};
        return details;
    });
}
Result<std::optional<VodMediaProbe>> SqliteVodStore::readSeasonMediaMetadata(const ContentRef &series, const QString &seasonId, const RequestContext &context)
{
    return guarded<std::optional<VodMediaProbe>>(context, [&]() -> std::optional<VodMediaProbe> {
        Connection connection(m_path, context); connection.open(); connection.begin(false); connection.checkRef(series);
        if (series.kind != ContentKind::Series) throw Error{ErrorCode::InvalidResponse, context.operationId};
        auto query = connection.sql(QStringLiteral(
            "SELECT d.payload FROM vod_episode_links e JOIN vod_details d ON d.identity=e.episode "
            "WHERE e.series=? AND COALESCE(e.season,'')=? AND d.fetched>=? "
            "ORDER BY COALESCE(e.number,2147483647),e.ordering,e.rowid"),
            {series.key(), seasonId, QDateTime::currentMSecsSinceEpoch() - qint64(168) * 3600 * 1000});
        std::optional<VodMediaProbe> newest;
        while (query.next()) {
            if (const auto error = context.interruption()) throw *error;
            const auto details = Storage::decodeDetails(query.value(0).toByteArray());
            if (!details || parentSeries(details->ref) != series) throw Error{ErrorCode::StorageUnavailable, context.operationId};
            const auto metadata = details->mediaProbe;
            if (metadata && (!newest || metadata->observedAtUtc > newest->observedAtUtc)) newest = metadata;
        }
        connection.current();
        return newest;
    });
}
Outcome SqliteVodStore::storeDetails(const VodDetails &details, const RequestContext &context)
{
    return guarded<Success>(context, [&]() -> Success {
        Connection connection(m_path, context); connection.open(); connection.begin(); connection.checkRef(details.ref);
        const auto payload = Storage::encodeDetails(details);
        if (!Storage::decodeDetails(payload)) throw Error{ErrorCode::InvalidResponse, context.operationId};
        if (details.ref.kind == ContentKind::Series) {
            const auto &series = details.ref;
            connection.sql(QStringLiteral("DELETE FROM vod_items WHERE profile=? AND namespace=? AND kind=? AND parent=?"),
                {uuid(series.profileId), uuid(series.catalogNamespace), int(ContentKind::Episode), series.providerItemId});
            connection.sql(QStringLiteral("DELETE FROM vod_seasons WHERE series=?"), {series.key()});
            for (const auto &season : details.seasons)
                connection.sql(QStringLiteral("INSERT INTO vod_seasons(series,profile,season,number,ordering) VALUES(?,?,?,?,?)"),
                    {series.key(), uuid(series.profileId), season.id, optionalNumber(season.number), season.order});
            for (const auto &episode : details.episodes) {
                connection.sql(QStringLiteral("INSERT INTO vod_items(identity,profile,namespace,kind,provider_id,parent,title,sort_key,availability) VALUES(?,?,?,?,?,?,?,?,?)"),
                    {episode.ref.key(), uuid(series.profileId), uuid(series.catalogNamespace), int(ContentKind::Episode),
                     episode.ref.providerItemId, series.providerItemId, episode.title, sortKey(episode.title), int(episode.availability)});
                connection.sql(QStringLiteral("INSERT INTO vod_episode_links(episode,series,profile,season,number,ordering) VALUES(?,?,?,?,?,?)"),
                    {episode.ref.key(), series.key(), uuid(series.profileId), optionalString(episode.seasonId), optionalNumber(episode.number), episode.order});
                auto existing=connection.sql(QStringLiteral("SELECT payload FROM vod_details WHERE identity=?"),{episode.ref.key()});
                auto episodeDetails=existing.next() ? Storage::decodeDetails(existing.value(0).toByteArray()).value_or(VodDetails{}) : VodDetails{};
                existing.finish();
                episodeDetails.ref=episode.ref;episodeDetails.title=episode.title;
                episodeDetails.description=episode.description;episodeDetails.artwork=episode.artwork;
                if (!episodeDetails.mediaProbe) episodeDetails.declaredDurationMs=episode.durationMs;
                connection.sql(QStringLiteral("INSERT INTO vod_details(identity,profile,payload,fetched) VALUES(?,?,?,?) ON CONFLICT(identity) DO UPDATE SET payload=excluded.payload,fetched=excluded.fetched"),
                    {episode.ref.key(),uuid(episode.ref.profileId),Storage::encodeDetails(episodeDetails),QDateTime::currentMSecsSinceEpoch()});
            }
        }
        connection.sql(QStringLiteral("INSERT INTO vod_details(identity,profile,payload,fetched) VALUES(?,?,?,?) ON CONFLICT(identity) DO UPDATE SET payload=excluded.payload,fetched=excluded.fetched"),
            {details.ref.key(), uuid(details.ref.profileId), payload, QDateTime::currentMSecsSinceEpoch()});
        connection.current(); connection.commit(); return {};
    });
}
Outcome SqliteVodStore::evictCache(const CatalogScope &scope, const RequestContext &context)
{
    return guarded<Success>(context, [&]() -> Success {
        Connection connection(m_path, context); connection.open(); connection.begin(); connection.checkScope(scope);
        eraseScope(connection, scope);
        if (!scope.categoryId)
            connection.sql(QStringLiteral("DELETE FROM vod_category_snapshots WHERE profile=? AND namespace=? AND kind=?"),
                {uuid(scope.profileId), uuid(scope.catalogNamespace), contentKind(scope.kind)});
        if (scope.kind == CatalogKind::Series) {
            connection.sql(QStringLiteral("DELETE FROM vod_items WHERE profile=? AND namespace=? AND kind=?"),
                {uuid(scope.profileId), uuid(scope.catalogNamespace), int(ContentKind::Episode)});
            connection.sql(QStringLiteral("DELETE FROM vod_seasons WHERE profile=?"), {uuid(scope.profileId)});
        }
        connection.sql(QStringLiteral("DELETE FROM vod_details WHERE profile=?"), {uuid(scope.profileId)});
        advanceGeneration(connection, scope);
        connection.commit(); return {};
    });
}
Outcome SqliteVodStore::removeSourceState(const QUuid &id)
{
    return guarded<Success>({}, [&]() -> Success {
        Connection connection(m_path, {}); connection.open(); connection.begin();
        auto state = connection.sql(QStringLiteral("SELECT removed FROM vod_source_state WHERE profile=?"), {uuid(id)});
        if (!state.next() || state.value(0).toInt() == 0) throw Error{ErrorCode::StorageUnavailable, {}};
        state.finish();
        for (const auto &table : {QStringLiteral("vod_category_snapshots"), QStringLiteral("vod_items"), QStringLiteral("vod_seasons"), QStringLiteral("vod_details"), QStringLiteral("vod_generations"), QStringLiteral("vod_sync_runs"), QStringLiteral("vod_progress"), QStringLiteral("vod_movie_lists"), QStringLiteral("vod_series_history"), QStringLiteral("vod_series_state"), QStringLiteral("vod_subtitles")})
            connection.sql(QStringLiteral("DELETE FROM %1 WHERE profile=?").arg(table), {uuid(id)});
        connection.commit(); return {};
    });
}
Outcome SqliteVodStore::beginSession(const ContentRef &ref, const QUuid &session, const RequestContext &context)
{
    return guarded<Success>(context, [&]() -> Success {
        Connection connection(m_path, context); connection.open(); connection.begin(); connection.checkRef(ref);
        if (!ref.playable() || session.isNull()) throw Error{ErrorCode::InvalidResponse, context.operationId};
        connection.sql(QStringLiteral("INSERT INTO vod_progress(identity,profile,namespace,kind,provider_id,parent,session,sequence) VALUES(?,?,?,?,?,?,?,0) "
            "ON CONFLICT(identity) DO UPDATE SET session=excluded.session,sequence=0"),
            {ref.key(), uuid(ref.profileId), uuid(ref.catalogNamespace), int(ref.kind), ref.providerItemId, ref.parentNamespace.value_or(QStringLiteral("")), uuid(session)});
        connection.commit(); return {};
    });
}
Outcome SqliteVodStore::checkpoint(const ContentRef &ref, const VodProgress &progress, const RequestContext &context, bool completed)
{
    return guarded<Success>(context, [&]() -> Success {
        Connection connection(m_path, context); connection.open(); connection.begin(); connection.checkRef(ref);
        if (!ref.playable() || progress.positionMs < 0 || progress.sequence == 0 || progress.sequence > quint64(std::numeric_limits<qint64>::max())
            || (progress.durationMs && *progress.durationMs <= 0)) throw Error{ErrorCode::InvalidResponse, context.operationId};
        auto update = connection.sql(QStringLiteral("UPDATE vod_progress SET position=?,duration=?,status=?,content_revision=?,updated=?,sequence=?,track_preferences=? "
            "WHERE identity=? AND session=? AND sequence<?"),
            {progress.positionMs, optionalNumber(progress.durationMs), int(progress.status), optionalString(progress.contentRevision),
             progress.updatedAtUtc.toMSecsSinceEpoch(), QVariant::fromValue(progress.sequence), QJsonDocument(progress.trackPreferences).toJson(QJsonDocument::Compact), ref.key(), uuid(progress.sessionToken), QVariant::fromValue(progress.sequence)});
        if (update.numRowsAffected() != 1) throw Error{ErrorCode::Cancelled, context.operationId};
        if (ref.kind == ContentKind::Episode && ref.parentNamespace) {
            const auto series = parentSeries(ref);
            if (progress.playbackCheckpoint) {
                connection.sql(QStringLiteral("UPDATE vod_series_state SET continue_hidden=0 WHERE series=? AND continue_hidden=1 AND blocked_session<>?"), {series.key(), uuid(progress.sessionToken)});
                connection.sql(QStringLiteral("INSERT INTO vod_series_history(series,profile,namespace,provider_id,episode_id,updated) VALUES(?,?,?,?,?,?) ON CONFLICT(series) DO UPDATE SET episode_id=excluded.episode_id,updated=excluded.updated"),
                    {series.key(), uuid(ref.profileId), uuid(ref.catalogNamespace), series.providerItemId, ref.providerItemId, progress.updatedAtUtc.toMSecsSinceEpoch()});
            }
            if (completed && progress.status == WatchStatus::Watched) {
                auto remaining = connection.sql(QStringLiteral("SELECT COUNT(*),SUM(CASE WHEN p.status=1 OR (p.duration>0 AND p.position>p.duration*0.95) THEN 0 ELSE 1 END) FROM vod_episode_links l JOIN vod_items i ON i.identity=l.episode LEFT JOIN vod_seasons s ON s.series=l.series AND s.season=l.season LEFT JOIN vod_progress p ON p.identity=l.episode WHERE l.series=? AND COALESCE(s.number,CASE WHEN l.season='0' THEN 0 ELSE -1 END)<>0"), {series.key()});
                if (remaining.next() && remaining.value(0).toInt() > 0 && remaining.value(1).toInt() == 0)
                    connection.sql(QStringLiteral("UPDATE vod_movie_lists SET to_watch=0 WHERE identity=?"), {series.key()});
            }
        }
        if (completed && ref.kind == ContentKind::Movie && progress.status == WatchStatus::Watched)
            connection.sql(QStringLiteral("UPDATE vod_movie_lists SET to_watch=0 WHERE identity=?"), {ref.key()});
        connection.current(); connection.commit(); return {};
    });
}
Result<MovieListState> SqliteVodStore::readMovieLists(const ContentRef &ref, const RequestContext &context)
{
    return guarded<MovieListState>(context, [&]() -> MovieListState {
        Connection connection(m_path, context); connection.open(); connection.begin(false); connection.checkRef(ref);
        if (ref.kind != ContentKind::Movie && ref.kind != ContentKind::Series) throw Error{ErrorCode::ContentUnavailable, context.operationId};
        auto row = connection.sql(QStringLiteral("SELECT to_watch,favourite FROM vod_movie_lists WHERE identity=?"), {ref.key()});
        const MovieListState result = row.next() ? MovieListState{row.value(0).toBool(), row.value(1).toBool()} : MovieListState{};
        row.finish(); connection.commit(); return result;
    });
}
Result<MovieListState> SqliteVodStore::setMovieList(const ContentRef &ref, MovieList list, bool enabled, const RequestContext &context)
{
    return guarded<MovieListState>(context, [&]() -> MovieListState {
        Connection connection(m_path, context); connection.open(); connection.begin(); connection.checkRef(ref);
        if ((ref.kind != ContentKind::Movie && ref.kind != ContentKind::Series) || (list != MovieList::ToWatch && list != MovieList::Favourites))
            throw Error{ErrorCode::InvalidResponse, context.operationId};
        const auto column = list == MovieList::ToWatch ? QStringLiteral("to_watch") : QStringLiteral("favourite");
        connection.sql(QStringLiteral("INSERT INTO vod_movie_lists(identity,profile,namespace,kind,provider_id,parent,%1) VALUES(?,?,?,?,?,?,?) ON CONFLICT(identity) DO UPDATE SET %1=excluded.%1").arg(column),
            {ref.key(), uuid(ref.profileId), uuid(ref.catalogNamespace), int(ref.kind), ref.providerItemId, ref.parentNamespace.value_or(QStringLiteral("")), enabled});
        auto row = connection.sql(QStringLiteral("SELECT to_watch,favourite FROM vod_movie_lists WHERE identity=?"), {ref.key()});
        row.next(); const MovieListState result{row.value(0).toBool(), row.value(1).toBool()}; row.finish();
        connection.current(); connection.commit(); return result;
    });
}
Result<std::optional<VodProgress>> SqliteVodStore::read(const ContentRef &ref, const RequestContext &context)
{
    return guarded<std::optional<VodProgress>>(context, [&]() -> std::optional<VodProgress> {
        Connection connection(m_path, context); connection.open(); connection.begin(false); connection.checkRef(ref);
        auto query = connection.sql(QStringLiteral("SELECT position,duration,status,content_revision,session,sequence,updated,track_preferences FROM vod_progress WHERE identity=?"), {ref.key()});
        if (!query.next() || query.value(0).isNull()) return {};
        VodProgress result;
        result.positionMs = query.value(0).toLongLong();
        if (!query.value(1).isNull()) result.durationMs = query.value(1).toLongLong();
        result.status = WatchStatus(query.value(2).toInt());
        if (!query.value(3).isNull()) result.contentRevision = query.value(3).toString();
        result.sessionToken = QUuid(query.value(4).toString()); result.sequence = query.value(5).toULongLong();
        result.trackPreferences = QJsonDocument::fromJson(query.value(7).toByteArray()).object();
        result.updatedAtUtc = QDateTime::fromMSecsSinceEpoch(query.value(6).toLongLong(), QTimeZone::UTC);
        return result;
    });
}
}

namespace OKILTV::Vod {
Result<SeriesProgress> SqliteVodStore::readSeriesProgress(const ContentRef &series, const RequestContext &context) {
    return guarded<SeriesProgress>(context, [&]() {
        Connection connection(m_path, context); connection.open(); connection.begin(false); connection.checkRef(series);
        if (series.kind != ContentKind::Series) throw Error{ErrorCode::ContentUnavailable, context.operationId};
        SeriesProgress result;
        auto hidden = connection.sql(QStringLiteral("SELECT continue_hidden FROM vod_series_state WHERE series=?"), {series.key()});
        if (hidden.next()) result.continuationHidden = hidden.value(0).toBool();
        hidden.finish();
        auto history = connection.sql(QStringLiteral("SELECT episode_id FROM vod_series_history WHERE series=?"), {series.key()});
        if (history.next()) result.lastEpisode = ContentRef{series.profileId, series.catalogNamespace, ContentKind::Episode, history.value(0).toString(), series.providerItemId};
        history.finish();
        auto target=connection.sql(QStringLiteral("SELECT i.provider_id FROM vod_series_continue v JOIN vod_items i ON i.identity=v.episode WHERE v.series=?"),{series.key()});
        if (target.next()) result.continuationEpisode=ContentRef{series.profileId,series.catalogNamespace,ContentKind::Episode,target.value(0).toString(),series.providerItemId};
        target.finish();
        auto rows = connection.sql(QStringLiteral("SELECT provider_id,position,duration,status,updated,track_preferences FROM vod_progress WHERE profile=? AND namespace=? AND kind=2 AND parent=? AND position IS NOT NULL"), {uuid(series.profileId),uuid(series.catalogNamespace),series.providerItemId});
        while (rows.next()) {
            ContentRef ref{series.profileId,series.catalogNamespace,ContentKind::Episode,rows.value(0).toString(),series.providerItemId};
            VodProgress progress; progress.positionMs=rows.value(1).toLongLong();
            if (!rows.value(2).isNull()) progress.durationMs=rows.value(2).toLongLong();
            progress.status=WatchStatus(rows.value(3).toInt()); progress.updatedAtUtc=QDateTime::fromMSecsSinceEpoch(rows.value(4).toLongLong(),QTimeZone::UTC);
            progress.trackPreferences=QJsonDocument::fromJson(rows.value(5).toByteArray()).object();
            result.episodes.insert(ref.key(),progress);
        }
        rows.finish(); connection.commit(); return result;
    });
}
}

namespace OKILTV::Vod {
Outcome SqliteVodStore::setSeriesWatched(const SeriesWatchedChange &change, const RequestContext &context)
{
    return guarded<Success>(context, [&]() -> Success {
        Connection connection(m_path, context); connection.open(); connection.begin(); connection.checkRef(change.series);
        if (change.series.kind != ContentKind::Series || change.episodes.isEmpty()) throw Error{ErrorCode::InvalidResponse, context.operationId};
        QSet<QByteArray> seen;
        for (const auto &ref : change.episodes) {
            if (!ref.valid() || ref.kind != ContentKind::Episode || parentSeries(ref) != change.series || seen.contains(ref.key()))
                throw Error{ErrorCode::InvalidResponse, context.operationId};
            seen.insert(ref.key());
            const bool active = ref == change.activeEpisode;
            VodProgress progress;
            auto saved = connection.sql(QStringLiteral("SELECT position,duration,track_preferences,content_revision FROM vod_progress WHERE identity=?"), {ref.key()});
            if (saved.next()) {
                progress.positionMs = saved.value(0).toLongLong();
                if (!saved.value(1).isNull()) progress.durationMs = saved.value(1).toLongLong();
                progress.trackPreferences = QJsonDocument::fromJson(saved.value(2).toByteArray()).object();
                if (!saved.value(3).isNull()) progress.contentRevision = saved.value(3).toString();
            }
            saved.finish();
            if (active) {
                if (change.activeProgress) progress = *change.activeProgress;
                progress.sessionToken = change.blockedSession; progress.sequence = change.activeSequence;
            } else { progress.sessionToken = QUuid::createUuid(); progress.sequence = 1; }
            if (progress.positionMs < 0 || (progress.durationMs && *progress.durationMs <= 0) || progress.sessionToken.isNull() || progress.sequence == 0 || progress.sequence > quint64(std::numeric_limits<qint64>::max()))
                throw Error{ErrorCode::InvalidResponse, context.operationId};
            if (!active)
                connection.sql(QStringLiteral("INSERT INTO vod_progress(identity,profile,namespace,kind,provider_id,parent,session,sequence) VALUES(?,?,?,?,?,?,?,0) ON CONFLICT(identity) DO UPDATE SET session=excluded.session,sequence=0"),
                    {ref.key(),uuid(ref.profileId),uuid(ref.catalogNamespace),int(ref.kind),ref.providerItemId,ref.parentNamespace.value_or(QString{}),uuid(progress.sessionToken)});
            auto updated = connection.sql(QStringLiteral("UPDATE vod_progress SET position=?,duration=?,status=?,track_preferences=?,content_revision=?,updated=?,sequence=? WHERE identity=? AND session=? AND sequence<?"),
                {change.watched ? progress.positionMs : 0,optionalNumber(progress.durationMs),int(change.watched ? WatchStatus::Watched : WatchStatus::InProgress),
                 QJsonDocument(progress.trackPreferences).toJson(QJsonDocument::Compact),optionalString(progress.contentRevision),QDateTime::currentMSecsSinceEpoch(),
                 QVariant::fromValue(progress.sequence),ref.key(),uuid(progress.sessionToken),QVariant::fromValue(progress.sequence)});
            if (updated.numRowsAffected() != 1) throw Error{ErrorCode::Cancelled, context.operationId};
        }
        if (change.watched) {
            connection.sql(QStringLiteral("INSERT INTO vod_series_state(series,profile,namespace,continue_hidden,blocked_session) VALUES(?,?,?,1,?) ON CONFLICT(series) DO UPDATE SET continue_hidden=1,blocked_session=excluded.blocked_session"),
                {change.series.key(),uuid(change.series.profileId),uuid(change.series.catalogNamespace),uuid(change.blockedSession)});
            connection.sql(QStringLiteral("UPDATE vod_movie_lists SET to_watch=0 WHERE identity=?"), {change.series.key()});
        }
        connection.current(); connection.commit(); return {};
    });
}
Result<QJsonObject> SqliteVodStore::readSubtitles(const ContentRef &ref, const RequestContext &context)
{
    return guarded<QJsonObject>(context, [&]() {
        Connection connection(m_path, context); connection.open(); connection.checkRef(ref);
        auto query = connection.sql(QStringLiteral("SELECT payload FROM vod_subtitles WHERE identity=?"), {ref.key()});
        return query.next() ? QJsonDocument::fromJson(query.value(0).toByteArray()).object() : QJsonObject{};
    });
}
Outcome SqliteVodStore::writeSubtitles(const ContentRef &ref, const QJsonObject &payload, const RequestContext &context)
{
    return guarded<Success>(context, [&]() -> Success {
        Connection connection(m_path, context); connection.open(); connection.begin(); connection.checkRef(ref);
        connection.sql(QStringLiteral("INSERT INTO vod_subtitles(identity,profile,namespace,payload) VALUES(?,?,?,?) "
            "ON CONFLICT(identity) DO UPDATE SET payload=excluded.payload"),
            {ref.key(), uuid(ref.profileId), uuid(ref.catalogNamespace), QJsonDocument(payload).toJson(QJsonDocument::Compact)});
        connection.commit(); return Success{};
    });
}

Result<QJsonArray> SqliteVodStore::subtitleInventory(const RequestContext &context)
{
    return guarded<QJsonArray>(context, [&]() {
        Connection connection(m_path, context); connection.open();
        auto query = connection.sql(QStringLiteral("SELECT identity,profile,payload FROM vod_subtitles"));
        QJsonArray result;
        while (query.next()) result.append(QJsonObject{{QStringLiteral("key"), QString::fromLatin1(query.value(0).toByteArray().toHex())},
            {QStringLiteral("profile"), query.value(1).toString()}, {QStringLiteral("state"), QJsonDocument::fromJson(query.value(2).toByteArray()).object()}});
        return result;
    });
}

}
