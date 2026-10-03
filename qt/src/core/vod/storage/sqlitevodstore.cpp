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
    QSqlQuery sql(const QString &statement, const QVariantList &values = {})
    {
        QSqlQuery query(db);
        if (!query.prepare(statement)) throw Error{ErrorCode::StorageUnavailable, context.operationId};
        for (const auto &value : values) query.addBindValue(value);
        if (!query.exec()) throw Error{ErrorCode::StorageUnavailable, context.operationId};
        return query;
    }
    void open()
    {
        if (const auto error = context.interruption()) throw *error;
        if (!db.open()) throw Error{ErrorCode::StorageUnavailable, context.operationId};
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
        if (!source.enabled) { invalidate(id); throw Error{ErrorCode::UnsupportedCapability, context.operationId}; }
        Connection connection(m_path, context); connection.open(); connection.begin();
        // Serialize publication with invalidate(), including the SQL commit. A
        // loader that started before a source mutation must never restore it.
        QMutexLocker lock(&m_stateMutex);
        if (m_epochs.value(id) != epoch) throw Error{ErrorCode::Cancelled, context.operationId};
        const auto configuredRevision = source.revision.credentialRevision;
        auto query = connection.sql(QStringLiteral("SELECT namespace,revision,removed,configuration_revision,mutation_pending FROM vod_source_state WHERE profile=?"), {uuid(id)});
        if (query.next()) {
            if (query.value(2).toInt() != 0 || query.value(4).toInt() != 0
                || query.value(3).toULongLong() > configuredRevision)
                throw Error{ErrorCode::Cancelled, context.operationId};
            source.revision.catalogNamespace = QUuid(query.value(0).toString());
            const auto storedRevision = query.value(1).toULongLong();
            if (query.value(3).toULongLong() == configuredRevision) source.revision.credentialRevision = storedRevision;
            else {
                if (storedRevision >= quint64(std::numeric_limits<qint64>::max()))
                    throw Error{ErrorCode::StorageUnavailable, context.operationId};
                source.revision.credentialRevision = std::max(configuredRevision, storedRevision + 1);
            }
        } else source.revision.catalogNamespace = QUuid::createUuid();
        query.finish();
        connection.sql(QStringLiteral("INSERT INTO vod_source_state(profile,namespace,revision,configuration_revision) VALUES(?,?,?,?) "
            "ON CONFLICT(profile) DO UPDATE SET revision=excluded.revision,configuration_revision=excluded.configuration_revision"),
            {uuid(id), uuid(source.revision.catalogNamespace), QVariant::fromValue(source.revision.credentialRevision), QVariant::fromValue(configuredRevision)});
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
        const auto orderKey = input.continueWatchingOnly
            ? QStringLiteral("(SELECT COALESCE(updated,0) FROM vod_progress WHERE vod_progress.identity=vod_items.identity)")
            : QStringLiteral("sort_key");
        QString statement = QStringLiteral("SELECT identity,provider_id,parent,title,year,availability,%1,artwork,COALESCE((SELECT to_watch FROM vod_movie_lists WHERE vod_movie_lists.identity=vod_items.identity),0),COALESCE((SELECT favourite FROM vod_movie_lists WHERE vod_movie_lists.identity=vod_items.identity),0) FROM vod_items WHERE profile=? AND namespace=? AND kind=?").arg(orderKey);
        QVariantList args{uuid(input.scope.profileId), uuid(input.scope.catalogNamespace), contentKind(input.scope.kind)};
        if (const auto category = input.scope.categoryId) {
            statement += QStringLiteral(" AND identity IN (SELECT identity FROM vod_item_categories WHERE category=?)"); args.append(*category);
        }
        if (input.continueWatchingOnly)
            statement += QStringLiteral(" AND identity IN (SELECT identity FROM vod_progress WHERE status=0 AND position>0 AND (duration IS NULL OR duration<=0 OR position<=duration*0.95))");
        if (input.movieList != MovieList::None) {
            if (input.scope.kind != CatalogKind::Movies || (input.movieList != MovieList::ToWatch && input.movieList != MovieList::Favourites))
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
            auto categories = connection.sql(QStringLiteral("SELECT category FROM vod_item_categories WHERE identity=? ORDER BY category"), {rows.value(0)});
            while (categories.next()) summary.categoryIds.append(categories.value(0).toString());
            if (input.scope.kind == CatalogKind::Movies) page.items.append(summary);
            else page.items.append(SeriesSummary{summary.ref, summary.title, summary.year, {}, summary.categoryIds, summary.availability});
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
        for (const auto &table : {QStringLiteral("vod_category_snapshots"), QStringLiteral("vod_items"), QStringLiteral("vod_seasons"), QStringLiteral("vod_details"), QStringLiteral("vod_generations"), QStringLiteral("vod_sync_runs"), QStringLiteral("vod_progress"), QStringLiteral("vod_movie_lists")})
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
        if (completed && ref.kind == ContentKind::Movie && progress.status == WatchStatus::Watched)
            connection.sql(QStringLiteral("UPDATE vod_movie_lists SET to_watch=0 WHERE identity=?"), {ref.key()});
        connection.current(); connection.commit(); return {};
    });
}
Result<MovieListState> SqliteVodStore::readMovieLists(const ContentRef &ref, const RequestContext &context)
{
    return guarded<MovieListState>(context, [&]() -> MovieListState {
        Connection connection(m_path, context); connection.open(); connection.begin(false); connection.checkRef(ref);
        if (ref.kind != ContentKind::Movie) throw Error{ErrorCode::ContentUnavailable, context.operationId};
        auto row = connection.sql(QStringLiteral("SELECT to_watch,favourite FROM vod_movie_lists WHERE identity=?"), {ref.key()});
        const MovieListState result = row.next() ? MovieListState{row.value(0).toBool(), row.value(1).toBool()} : MovieListState{};
        row.finish(); connection.commit(); return result;
    });
}
Result<MovieListState> SqliteVodStore::setMovieList(const ContentRef &ref, MovieList list, bool enabled, const RequestContext &context)
{
    return guarded<MovieListState>(context, [&]() -> MovieListState {
        Connection connection(m_path, context); connection.open(); connection.begin(); connection.checkRef(ref);
        if (ref.kind != ContentKind::Movie || (list != MovieList::ToWatch && list != MovieList::Favourites))
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
