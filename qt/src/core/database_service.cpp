#include "database_service.h"

#include "appdatapaths.h"
#include "m3uservice.h"
#include <QMutex>
#include <memory>
#include "secretprotection.h"
#include "debuglogger.h"

#include <QCryptographicHash>
#include <QFile>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QTimeZone>
#include <QVariant>

#include <algorithm>
#include <stdexcept>

namespace OKILTV::Core {

namespace {

struct ChannelPublication {
    QMutex mutex;
    quint64 token { 0 };
};

std::shared_ptr<ChannelPublication> publicationFor(const QUuid &id)
{
    static QMutex mutex;
    static QHash<QUuid, std::shared_ptr<ChannelPublication>> publications;
    QMutexLocker lock(&mutex);
    auto &publication = publications[id];
    if (!publication) publication = std::make_shared<ChannelPublication>();
    return publication;
}

class ScopedConnection
{
public:
    explicit ScopedConnection(const QString &databaseFilePath, int busyTimeoutMs = 5000)
        : m_name(QStringLiteral("iptvplayer_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
    {
        m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        m_database.setDatabaseName(databaseFilePath);
        m_database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=%1").arg(busyTimeoutMs));
        if (!m_database.open()) {
            const auto error = QStringLiteral("Failed to open SQLite database %1: %2")
                .arg(databaseFilePath, m_database.lastError().text());
            close();
            throw std::runtime_error(error.toStdString());
        }
        if (!QFile::setPermissions(databaseFilePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            close();
            throw std::runtime_error("Cannot restrict database file permissions.");
        }
    }

    ~ScopedConnection()
    {
        close();
    }

    QSqlDatabase &database()
    {
        return m_database;
    }

private:
    void close()
    {
        m_database.close();
        m_database = {};
        QSqlDatabase::removeDatabase(m_name);
    }

    QString m_name;
    QSqlDatabase m_database;
};

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool tableHasColumn(QSqlDatabase &database, const QString &tableName, const QString &columnName)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(tableName))) {
        throw std::runtime_error(
            QStringLiteral("Inspect schema failed: %1").arg(query.lastError().text()).toStdString());
    }

    while (query.next()) {
        if (query.value(1).toString().compare(columnName, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }

    return false;
}

void execOrThrow(QSqlQuery &query, const QString &context)
{
    if (!query.exec()) {
        throw std::runtime_error(
            QStringLiteral("%1 failed: %2")
                .arg(context, query.lastError().text())
                .toStdString());
    }
}

void ensureSchemaOnConnection(QSqlDatabase &database)
{
    const QStringList statements = {
        QStringLiteral(R"sql(
            CREATE TABLE IF NOT EXISTS catchup_progress (
                resume_key TEXT PRIMARY KEY,
                profile_id TEXT NOT NULL,
                program_start_ms INTEGER NOT NULL,
                program_stop_ms INTEGER NOT NULL,
                position_ms INTEGER NOT NULL,
                expires_at_ms INTEGER NOT NULL
            )
        )sql"),
        QStringLiteral(R"sql(
            CREATE TABLE IF NOT EXISTS channels (
                id          INTEGER NOT NULL,
                profile_id  TEXT    NOT NULL,
                name        TEXT    NOT NULL,
                stream_url  TEXT    NOT NULL,
                category_id TEXT    NOT NULL DEFAULT '',
                tvg_id      TEXT    NOT NULL DEFAULT '',
                tvg_name    TEXT    NOT NULL DEFAULT '',
                icon_url    TEXT,
                cached_icon TEXT,
                source      TEXT    NOT NULL,
                sort_order  INTEGER NOT NULL DEFAULT 0,
                catchup_supported INTEGER NOT NULL DEFAULT 0,
                catchup_window_hours INTEGER NOT NULL DEFAULT 0,
                catchup_mode TEXT NOT NULL DEFAULT '',
                catchup_source_template TEXT NOT NULL DEFAULT '',
                PRIMARY KEY (id, profile_id)
            )
        )sql"),
        QStringLiteral(R"sql(
            CREATE TABLE IF NOT EXISTS epg_entries (
                channel_id  TEXT    NOT NULL,
                profile_id  TEXT    NOT NULL,
                title       TEXT    NOT NULL,
                sub_title   TEXT,
                description TEXT,
                start_unix  INTEGER NOT NULL,
                stop_unix   INTEGER NOT NULL
            )
        )sql"),
        QStringLiteral(R"sql(
            CREATE INDEX IF NOT EXISTS idx_epg_lookup
                ON epg_entries(profile_id, channel_id, start_unix)
        )sql"),
        QStringLiteral(R"sql(
            CREATE TABLE IF NOT EXISTS icon_cache (
                url_hash    TEXT PRIMARY KEY,
                local_path  TEXT NOT NULL,
                fetched_at  INTEGER NOT NULL
            )
        )sql"),
        QStringLiteral(R"sql(
            CREATE TABLE IF NOT EXISTS m3u_channel_sequences (
                profile_id TEXT PRIMARY KEY,
                next_id INTEGER NOT NULL
            )
        )sql"),
        QStringLiteral(R"sql(
            CREATE TABLE IF NOT EXISTS channel_watch_stats (
                profile_id    TEXT    NOT NULL,
                channel_id    INTEGER NOT NULL,
                watch_seconds INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY (profile_id, channel_id)
            )
        )sql")
    };

    for (const auto &statement : statements) {
        QSqlQuery query(database);
        if (!query.exec(statement)) {
            throw std::runtime_error(
                QStringLiteral("Ensure schema failed: %1").arg(query.lastError().text()).toStdString());
        }
    }

    if (!tableHasColumn(database, QStringLiteral("epg_entries"), QStringLiteral("sub_title"))) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("ALTER TABLE epg_entries ADD COLUMN sub_title TEXT"))) {
            throw std::runtime_error(
                QStringLiteral("Schema migration failed: %1").arg(query.lastError().text()).toStdString());
        }
    }

    if (!tableHasColumn(database, QStringLiteral("channels"), QStringLiteral("category_name"))) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("ALTER TABLE channels ADD COLUMN category_name TEXT NOT NULL DEFAULT ''"))) {
            throw std::runtime_error(
                QStringLiteral("Schema migration failed: %1").arg(query.lastError().text()).toStdString());
        }
    }
    if (!tableHasColumn(database, QStringLiteral("channels"), QStringLiteral("catchup_supported"))) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("ALTER TABLE channels ADD COLUMN catchup_supported INTEGER NOT NULL DEFAULT 0"))) {
            throw std::runtime_error(
                QStringLiteral("Schema migration failed: %1").arg(query.lastError().text()).toStdString());
        }
    }
    if (!tableHasColumn(database, QStringLiteral("channels"), QStringLiteral("catchup_window_hours"))) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("ALTER TABLE channels ADD COLUMN catchup_window_hours INTEGER NOT NULL DEFAULT 0"))) {
            throw std::runtime_error(
                QStringLiteral("Schema migration failed: %1").arg(query.lastError().text()).toStdString());
        }
    }
    if (!tableHasColumn(database, QStringLiteral("channels"), QStringLiteral("catchup_mode"))) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("ALTER TABLE channels ADD COLUMN catchup_mode TEXT NOT NULL DEFAULT ''"))) {
            throw std::runtime_error(
                QStringLiteral("Schema migration failed: %1").arg(query.lastError().text()).toStdString());
        }
    }
    if (!tableHasColumn(database, QStringLiteral("channels"), QStringLiteral("catchup_source_template"))) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("ALTER TABLE channels ADD COLUMN catchup_source_template TEXT NOT NULL DEFAULT ''"))) {
            throw std::runtime_error(
                QStringLiteral("Schema migration failed: %1").arg(query.lastError().text()).toStdString());
        }
    }
}

// Secret protection can involve OS services. Complete it before holding a
// SQLite write transaction so a source refresh does not block other writers.
QList<Channel> channelsWithProtectedFields(QList<Channel> channels)
{
    for (auto &channel : channels) {
        channel.streamUrl = protectSecret(channel.streamUrl);
        if (!channel.iconUrl.isEmpty()) channel.iconUrl = protectSecret(channel.iconUrl);
        channel.catchupSourceTemplate = channel.catchupSourceTemplate.trimmed();
        if (!channel.catchupSourceTemplate.isEmpty()) {
            channel.catchupSourceTemplate = protectSecret(channel.catchupSourceTemplate);
        }
    }
    return channels;
}

Channel channelFromQuery(const QSqlQuery &query, const QUuid &profileId)
{
    Channel channel;
    channel.id = query.value(QStringLiteral("id")).toInt();
    channel.profileId = profileId;
    channel.name = query.value(QStringLiteral("name")).toString();
    channel.streamUrl = unprotectSecret(query.value(QStringLiteral("stream_url")).toString());
    channel.categoryId = normalizeChannelCategoryId(query.value(QStringLiteral("category_id")).toString());
    channel.categoryName = query.value(QStringLiteral("category_name")).toString();
    channel.tvgId = query.value(QStringLiteral("tvg_id")).toString();
    channel.tvgName = query.value(QStringLiteral("tvg_name")).toString();
    channel.iconUrl = unprotectSecret(query.value(QStringLiteral("icon_url")).toString());
    channel.cachedIconPath = query.value(QStringLiteral("cached_icon")).toString();
    channel.source = channelSourceFromString(query.value(QStringLiteral("source")).toString());
    channel.sortOrder = query.value(QStringLiteral("sort_order")).toInt();
    channel.catchupSupported = query.value(QStringLiteral("catchup_supported")).toBool();
    channel.catchupWindowHours = std::max(0, query.value(QStringLiteral("catchup_window_hours")).toInt());
    channel.catchupMode = query.value(QStringLiteral("catchup_mode")).toString();
    channel.catchupSourceTemplate = unprotectSecret(query.value(QStringLiteral("catchup_source_template")).toString());
    return channel;
}

EpgEntry epgEntryFromQuery(const QSqlQuery &query)
{
    EpgEntry entry;
    entry.channelId = query.value(0).toString();
    entry.title = query.value(1).toString();
    entry.subTitle = query.value(2).toString();
    entry.description = query.value(3).toString();
    entry.start = QDateTime::fromSecsSinceEpoch(query.value(4).toLongLong(), QTimeZone::UTC);
    entry.stop = QDateTime::fromSecsSinceEpoch(query.value(5).toLongLong(), QTimeZone::UTC);
    return entry;
}

} // namespace

DatabaseService::DatabaseService(QString databaseFilePath, const RebuildStarted &rebuildStarted)
    : m_databaseFilePath(databaseFilePath.isEmpty() ? AppDataPaths::databaseFile() : std::move(databaseFilePath))
{
    ensureSchema(rebuildStarted);
}

QString DatabaseService::databaseFilePath() const
{
    return m_databaseFilePath;
}

void DatabaseService::ensureSchema(const RebuildStarted &rebuildStarted) const
{
    bool rebuildReported = false;
    const auto reportRebuild = [&]() {
        if (!rebuildReported) {
            rebuildReported = true;
            if (rebuildStarted) rebuildStarted();
        }
    };
    QElapsedTimer elapsed;
    elapsed.start();
    const auto logStage = [&elapsed](const QString &stage) {
        DebugLogger::instance().log(QStringLiteral("database-startup"),
            QStringLiteral("%1; elapsedMs=%2").arg(stage).arg(elapsed.elapsed()));
    };
    logStage(QStringLiteral("Opening SQLite connection"));
    ScopedConnection connection(m_databaseFilePath);
    logStage(QStringLiteral("Ensuring schema"));
    auto &database = connection.database();
    ensureSchemaOnConnection(database);
    QSqlQuery secure(database);
    if (!secure.exec(QStringLiteral("PRAGMA secure_delete=ON"))) throw std::runtime_error("Cannot enable database cleanup.");
    secure.finish();
    if (!database.transaction()) throw std::runtime_error("Cannot begin credential migration.");
    bool changed = false;
    try {
        QSqlQuery marker(database);
        if (!marker.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS security_migrations (name TEXT PRIMARY KEY)"))) {
            throw std::runtime_error("Cannot track credential migration cleanup.");
        }
        QSqlQuery rows(database);
        logStage(QStringLiteral("Scanning channel protection state"));
        if (!rows.exec(QStringLiteral("SELECT id, profile_id, stream_url, icon_url, catchup_source_template FROM channels"))) {
            throw std::runtime_error("Cannot read channels for credential migration.");
        }
        struct Row { int id; QString profile; QString stream; QString icon; QString catchup; };
        QList<Row> pending;
        while (rows.next()) {
            const auto stream = rows.value(2).toString();
            const auto icon = rows.value(3).toString();
            const auto catchup = rows.value(4).toString();
            if ((!stream.isEmpty() && !isProtectedSecret(stream))
                || (!icon.isEmpty() && !isProtectedSecret(icon))
                || (!catchup.isEmpty() && !isProtectedSecret(catchup))) {
                pending.push_back({ rows.value(0).toInt(), rows.value(1).toString(), stream, icon, catchup });
            }
        }
        rows.finish();
        if (!pending.isEmpty()) reportRebuild();
        logStage(QStringLiteral("Legacy channels requiring protection: %1").arg(pending.size()));
        QElapsedTimer progressTimer;
        progressTimer.start();
        qsizetype completed = 0;
        const auto migrate = [](const QString &value) {
            if (value.isEmpty() || isProtectedSecret(value)) return value;
            const auto encrypted = protectSecret(value);
            if (unprotectSecret(encrypted) != value) throw std::runtime_error("Channel protection verification failed.");
            return encrypted;
        };
        for (const auto &row : pending) {
            QSqlQuery update(database);
            update.prepare(QStringLiteral("UPDATE channels SET stream_url=?, icon_url=?, catchup_source_template=? WHERE id=? AND profile_id=?"));
            update.addBindValue(migrate(row.stream));
            update.addBindValue(migrate(row.icon));
            update.addBindValue(migrate(row.catchup));
            update.addBindValue(row.id);
            update.addBindValue(row.profile);
            execOrThrow(update, QStringLiteral("Protect cached channel"));
            ++completed;
            if (progressTimer.elapsed() >= 1000 || completed == pending.size()) {
                logStage(QStringLiteral("Protected channels: %1/%2").arg(completed).arg(pending.size()));
                progressTimer.restart();
            }
        }
        if (!pending.isEmpty() && !marker.exec(QStringLiteral("INSERT OR IGNORE INTO security_migrations VALUES ('channel-cleanup-v1')"))) {
            throw std::runtime_error("Cannot track credential migration cleanup.");
        }
        if (!marker.exec(QStringLiteral("SELECT name FROM security_migrations WHERE name='channel-cleanup-v1'"))) {
            throw std::runtime_error("Cannot read credential migration state.");
        }
        changed = marker.next();
        marker.finish();
        if (!database.commit()) throw std::runtime_error("Cannot commit credential migration.");
    } catch (...) {
        database.rollback();
        throw;
    }
    if (changed) {
        reportRebuild(); // Also show progress when resuming interrupted cleanup.
        logStage(QStringLiteral("Checkpointing migrated database"));
        QSqlQuery cleanup(database);
        if (!cleanup.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"))
            || (cleanup.next() && cleanup.value(0).toInt() != 0)) {
            throw std::runtime_error("Protected channels were saved, but database checkpoint must be retried.");
        }
        cleanup.finish();
        logStage(QStringLiteral("Vacuuming migrated database"));
        if (!cleanup.exec(QStringLiteral("VACUUM"))
            || !cleanup.exec(QStringLiteral("DELETE FROM security_migrations WHERE name='channel-cleanup-v1'"))) {
            throw std::runtime_error("Protected channels were saved, but database cleanup must be retried.");
        }
    }
    logStage(QStringLiteral("Database ready"));
}

quint64 DatabaseService::beginChannelImport(const QUuid &profileId)
{
    const auto state = publicationFor(profileId);
    QMutexLocker lock(&state->mutex);
    return ++state->token;
}

bool DatabaseService::channelImportCurrent(const QUuid &profileId, quint64 token)
{
    const auto state = publicationFor(profileId);
    QMutexLocker lock(&state->mutex);
    return state->token == token;
}

bool DatabaseService::publishChannels(const QUuid &profileId, quint64 token,
    QList<Channel> &channels, bool retainM3uIds) const
{
    const auto state = publicationFor(profileId);
    QMutexLocker lock(&state->mutex);
    if (state->token != token) return false;
    std::optional<qint64> nextId;
    if (retainM3uIds) {
        nextId = nextM3uChannelId(profileId);
        M3UService::retainChannelIds(channels, loadChannels(profileId), *nextId);
    }
    replaceChannelsForProfile(profileId, channels, nextId);
    return true;
}

void DatabaseService::publishChannelIcon(const Channel &channel, quint64 token, const QString &path) const
{
    const auto state = publicationFor(channel.profileId);
    QMutexLocker lock(&state->mutex);
    if (state->token != token) return;
    ScopedConnection connection(m_databaseFilePath);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("SELECT icon_url FROM channels WHERE profile_id=? AND id=?"));
    query.addBindValue(guidToString(channel.profileId));
    query.addBindValue(channel.id);
    execOrThrow(query, QStringLiteral("Read channel icon identity"));
    if (!query.next() || unprotectSecret(query.value(0).toString()) != channel.iconUrl) return;
    query.finish();
    query.prepare(QStringLiteral("UPDATE channels SET cached_icon=? WHERE profile_id=? AND id=?"));
    query.addBindValue(path);
    query.addBindValue(guidToString(channel.profileId));
    query.addBindValue(channel.id);
    execOrThrow(query, QStringLiteral("Publish channel icon"));
}

void DatabaseService::removeProfileData(const QUuid &profileId) const
{
    const auto state = publicationFor(profileId);
    QMutexLocker lock(&state->mutex);
    ++state->token;
    ScopedConnection connection(m_databaseFilePath);
    auto &database = connection.database();
    QSqlQuery secure(database);
    if (!secure.exec(QStringLiteral("PRAGMA secure_delete=ON"))) throw std::runtime_error("Cannot enable database cleanup.");
    secure.finish();
    if (!database.transaction()) throw std::runtime_error("Cannot begin source removal.");
    try {
        for (const auto *table : { "channels", "epg_entries", "channel_watch_stats", "catchup_progress", "m3u_channel_sequences" }) {
            QSqlQuery query(database);
            query.prepare(QStringLiteral("DELETE FROM %1 WHERE profile_id=?").arg(QString::fromLatin1(table)));
            query.addBindValue(guidToString(profileId));
            execOrThrow(query, QStringLiteral("Remove source data"));
        }
        if (!database.commit()) throw std::runtime_error("Cannot commit source removal.");
    } catch (...) {
        database.rollback();
        throw;
    }
}

void DatabaseService::upsertChannels(const QList<Channel> &channels) const
{
    const auto protectedChannels = channelsWithProtectedFields(channels);
    ScopedConnection connection(m_databaseFilePath);
    auto &database = connection.database();

    if (!database.transaction()) {
        throw std::runtime_error(database.lastError().text().toStdString());
    }

    try {
        QSqlQuery query(database);
        query.prepare(QStringLiteral(R"sql(
            INSERT INTO channels
                (id, profile_id, name, stream_url, category_id, category_name, tvg_id, tvg_name, icon_url, source, sort_order,
                 catchup_supported, catchup_window_hours, catchup_mode, catchup_source_template)
            VALUES
                (:id, :profile_id, :name, :stream_url, :category_id, :category_name, :tvg_id, :tvg_name, :icon_url, :source, :sort_order,
                 :catchup_supported, :catchup_window_hours, :catchup_mode, :catchup_source_template)
            ON CONFLICT(id, profile_id) DO UPDATE SET
                name = excluded.name,
                stream_url = excluded.stream_url,
                category_id = excluded.category_id,
                category_name = excluded.category_name,
                tvg_id = excluded.tvg_id,
                tvg_name = excluded.tvg_name,
                icon_url = excluded.icon_url,
                source = excluded.source,
                sort_order = excluded.sort_order,
                catchup_supported = excluded.catchup_supported,
                catchup_window_hours = excluded.catchup_window_hours,
                catchup_mode = excluded.catchup_mode,
                catchup_source_template = excluded.catchup_source_template
        )sql"));

        for (const auto &channel : protectedChannels) {
            query.bindValue(QStringLiteral(":id"), channel.id);
            query.bindValue(QStringLiteral(":profile_id"), guidToString(channel.profileId));
            query.bindValue(QStringLiteral(":name"), channel.name);
            query.bindValue(QStringLiteral(":stream_url"), channel.streamUrl);
            query.bindValue(QStringLiteral(":category_id"), normalizeChannelCategoryId(channel.categoryId));
            query.bindValue(
                QStringLiteral(":category_name"),
                channel.categoryName.isNull() ? QStringLiteral("") : channel.categoryName);
            query.bindValue(QStringLiteral(":tvg_id"), channel.tvgId.isNull() ? QStringLiteral("") : channel.tvgId);
            query.bindValue(QStringLiteral(":tvg_name"), channel.tvgName.isNull() ? QStringLiteral("") : channel.tvgName);
            query.bindValue(QStringLiteral(":icon_url"), channel.iconUrl.isEmpty() ? QVariant {} : QVariant(channel.iconUrl));
            query.bindValue(QStringLiteral(":source"), channelSourceToString(channel.source));
            query.bindValue(QStringLiteral(":sort_order"), channel.sortOrder);
            query.bindValue(QStringLiteral(":catchup_supported"), channel.catchupSupported);
            query.bindValue(QStringLiteral(":catchup_window_hours"), std::max(0, channel.catchupWindowHours));
            query.bindValue(
                QStringLiteral(":catchup_mode"),
                channel.catchupMode.trimmed().isEmpty() ? QStringLiteral("") : channel.catchupMode.trimmed());
            query.bindValue(
                QStringLiteral(":catchup_source_template"),
                channel.catchupSourceTemplate.trimmed().isEmpty()
                    ? QStringLiteral("")
                    : channel.catchupSourceTemplate);
            execOrThrow(query, QStringLiteral("Upsert channel"));
        }

        if (!database.commit()) {
            throw std::runtime_error(
                QStringLiteral("Transaction commit failed: %1")
                    .arg(database.lastError().text()).toStdString());
        }
    } catch (...) {
        database.rollback();
        throw;
    }
}

void DatabaseService::replaceChannelsForProfile(const QUuid &profileId, const QList<Channel> &channels,
                                                std::optional<qint64> nextM3uChannelId) const
{
    const auto protectedChannels = channelsWithProtectedFields(channels);
    ScopedConnection connection(m_databaseFilePath);
    auto &database = connection.database();

    if (!database.transaction()) {
        throw std::runtime_error(database.lastError().text().toStdString());
    }

    try {
        QSqlQuery query(database);
        query.prepare(QStringLiteral(R"sql(
            INSERT INTO channels
                (id, profile_id, name, stream_url, category_id, category_name, tvg_id, tvg_name, icon_url, source, sort_order,
                 catchup_supported, catchup_window_hours, catchup_mode, catchup_source_template)
            VALUES
                (:id, :profile_id, :name, :stream_url, :category_id, :category_name, :tvg_id, :tvg_name, :icon_url, :source, :sort_order,
                 :catchup_supported, :catchup_window_hours, :catchup_mode, :catchup_source_template)
            ON CONFLICT(id, profile_id) DO UPDATE SET
                name = excluded.name,
                stream_url = excluded.stream_url,
                category_id = excluded.category_id,
                category_name = excluded.category_name,
                tvg_id = excluded.tvg_id,
                tvg_name = excluded.tvg_name,
                icon_url = excluded.icon_url,
                source = excluded.source,
                sort_order = excluded.sort_order,
                catchup_supported = excluded.catchup_supported,
                catchup_window_hours = excluded.catchup_window_hours,
                catchup_mode = excluded.catchup_mode,
                catchup_source_template = excluded.catchup_source_template
        )sql"));

        for (const auto &channel : protectedChannels) {
            if (channel.profileId != profileId) {
                throw std::runtime_error(
                    QStringLiteral("Channel profile mismatch while replacing channels for %1.")
                        .arg(guidToString(profileId))
                        .toStdString());
            }

            query.bindValue(QStringLiteral(":id"), channel.id);
            query.bindValue(QStringLiteral(":profile_id"), guidToString(channel.profileId));
            query.bindValue(QStringLiteral(":name"), channel.name);
            query.bindValue(QStringLiteral(":stream_url"), channel.streamUrl);
            query.bindValue(QStringLiteral(":category_id"), normalizeChannelCategoryId(channel.categoryId));
            query.bindValue(
                QStringLiteral(":category_name"),
                channel.categoryName.isNull() ? QStringLiteral("") : channel.categoryName);
            query.bindValue(QStringLiteral(":tvg_id"), channel.tvgId.isNull() ? QStringLiteral("") : channel.tvgId);
            query.bindValue(QStringLiteral(":tvg_name"), channel.tvgName.isNull() ? QStringLiteral("") : channel.tvgName);
            query.bindValue(QStringLiteral(":icon_url"), channel.iconUrl.isEmpty() ? QVariant {} : QVariant(channel.iconUrl));
            query.bindValue(QStringLiteral(":source"), channelSourceToString(channel.source));
            query.bindValue(QStringLiteral(":sort_order"), channel.sortOrder);
            query.bindValue(QStringLiteral(":catchup_supported"), channel.catchupSupported);
            query.bindValue(QStringLiteral(":catchup_window_hours"), std::max(0, channel.catchupWindowHours));
            query.bindValue(
                QStringLiteral(":catchup_mode"),
                channel.catchupMode.trimmed().isEmpty() ? QStringLiteral("") : channel.catchupMode.trimmed());
            query.bindValue(
                QStringLiteral(":catchup_source_template"),
                channel.catchupSourceTemplate.trimmed().isEmpty()
                    ? QStringLiteral("")
                    : channel.catchupSourceTemplate);
            execOrThrow(query, QStringLiteral("Upsert channel"));
        }

        QSqlQuery pruneQuery(database);
        if (channels.isEmpty()) {
            pruneQuery.prepare(QStringLiteral("DELETE FROM channels WHERE profile_id = :profile_id"));
        } else {
            QStringList ids;
            ids.reserve(channels.size());
            for (const auto &channel : protectedChannels) {
                ids.push_back(QString::number(channel.id));
            }
            pruneQuery.prepare(
                QStringLiteral("DELETE FROM channels WHERE profile_id = :profile_id AND id NOT IN (%1)")
                    .arg(ids.join(',')));
        }
        pruneQuery.bindValue(QStringLiteral(":profile_id"), guidToString(profileId));
        execOrThrow(pruneQuery, QStringLiteral("Prune stale channels"));

        if (nextM3uChannelId) {
            QSqlQuery sequence(database);
            sequence.prepare(QStringLiteral("INSERT INTO m3u_channel_sequences(profile_id, next_id) VALUES(?, ?) "
                "ON CONFLICT(profile_id) DO UPDATE SET next_id=MAX(next_id, excluded.next_id)"));
            sequence.addBindValue(guidToString(profileId));
            sequence.addBindValue(*nextM3uChannelId);
            execOrThrow(sequence, QStringLiteral("Retain playlist channel sequence"));
        }

        if (!database.commit()) {
            throw std::runtime_error(
                QStringLiteral("Transaction commit failed: %1")
                    .arg(database.lastError().text()).toStdString());
        }
    } catch (...) {
        database.rollback();
        throw;
    }
}

qint64 DatabaseService::nextM3uChannelId(const QUuid &profileId) const
{
    ScopedConnection connection(m_databaseFilePath);
    auto &database = connection.database();
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT next_id FROM m3u_channel_sequences WHERE profile_id=?"));
    query.addBindValue(guidToString(profileId));
    execOrThrow(query, QStringLiteral("Load playlist channel sequence"));
    return query.next() ? std::max<qint64>(0, query.value(0).toLongLong()) : 0;
}

QStringList DatabaseService::loadChannelGroupIds(const QUuid &profileId) const
{
    ScopedConnection connection(m_databaseFilePath);
    auto &database = connection.database();
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT category_id FROM channels WHERE profile_id = :profile_id ORDER BY sort_order"));
    query.bindValue(QStringLiteral(":profile_id"), guidToString(profileId));
    execOrThrow(query, QStringLiteral("Load channel group IDs"));

    QStringList groups;
    QSet<QString> seen;
    while (query.next()) {
        const auto group = normalizeChannelCategoryId(query.value(0).toString());
        if (!seen.contains(group)) {
            seen.insert(group);
            groups.push_back(group);
        }
    }
    return groups;
}

QList<Channel> DatabaseService::loadChannels(const QUuid &profileId) const
{
    ScopedConnection connection(m_databaseFilePath);
    auto &database = connection.database();

    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT * FROM channels WHERE profile_id = :profile_id ORDER BY sort_order"));
    query.bindValue(QStringLiteral(":profile_id"), guidToString(profileId));
    execOrThrow(query, QStringLiteral("Load channels"));

    QList<Channel> channels;
    while (query.next()) {
        try {
            channels.push_back(channelFromQuery(query, profileId));
        } catch (const std::exception &) {
            DebugLogger::instance().log(QStringLiteral("storage"),
                QStringLiteral("Cannot unlock cached channel URLs; preserved the database. Re-enter source credentials or unlock the original secret store."));
            return {};
        }
    }

    return channels;
}

void DatabaseService::updateCachedIcon(int channelId, const QUuid &profileId, const QString &localPath) const
{
    ScopedConnection connection(m_databaseFilePath);
    auto &database = connection.database();

    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "UPDATE channels SET cached_icon = :path WHERE id = :id AND profile_id = :profile_id"));
    query.bindValue(QStringLiteral(":path"), localPath);
    query.bindValue(QStringLiteral(":id"), channelId);
    query.bindValue(QStringLiteral(":profile_id"), guidToString(profileId));
    execOrThrow(query, QStringLiteral("Update cached icon"));
}

QHash<int, qint64> DatabaseService::loadWatchSecondsByProfile(const QUuid &profileId) const
{
    ScopedConnection connection(m_databaseFilePath);
    auto &database = connection.database();

    QSqlQuery query(database);
    query.prepare(QStringLiteral(R"sql(
        SELECT channel_id, watch_seconds
        FROM channel_watch_stats
        WHERE profile_id = :profile_id
    )sql"));
    query.bindValue(QStringLiteral(":profile_id"), guidToString(profileId));
    execOrThrow(query, QStringLiteral("Load watch stats"));

    QHash<int, qint64> watchSecondsByChannelId;
    while (query.next()) {
        const auto channelId = query.value(0).toInt();
        const auto watchSeconds = std::max<qint64>(0, query.value(1).toLongLong());
        watchSecondsByChannelId.insert(channelId, watchSeconds);
    }

    return watchSecondsByChannelId;
}

void DatabaseService::incrementWatchSeconds(const QUuid &profileId, const int channelId, const qint64 deltaSeconds) const
{
    if (channelId < 0 || deltaSeconds <= 0) {
        return;
    }

    // Called by the UI timer; defer contention to the next flush instead of
    // blocking the event loop behind a source refresh.
    ScopedConnection connection(m_databaseFilePath, 0);
    auto &database = connection.database();

    QSqlQuery query(database);
    query.prepare(QStringLiteral(R"sql(
        INSERT INTO channel_watch_stats (profile_id, channel_id, watch_seconds)
        VALUES (:profile_id, :channel_id, :delta_seconds)
        ON CONFLICT(profile_id, channel_id) DO UPDATE SET
            watch_seconds = watch_seconds + excluded.watch_seconds
    )sql"));
    query.bindValue(QStringLiteral(":profile_id"), guidToString(profileId));
    query.bindValue(QStringLiteral(":channel_id"), channelId);
    query.bindValue(QStringLiteral(":delta_seconds"), deltaSeconds);
    execOrThrow(query, QStringLiteral("Increment watch stats"));
}

void DatabaseService::replaceEpg(const QUuid &profileId, const QList<EpgEntry> &entries) const
{
    ScopedConnection connection(m_databaseFilePath);
    auto &database = connection.database();

    if (!database.transaction()) {
        throw std::runtime_error(database.lastError().text().toStdString());
    }

    try {
        {
            QSqlQuery deleteQuery(database);
            deleteQuery.prepare(QStringLiteral("DELETE FROM epg_entries WHERE profile_id = :profile_id"));
            deleteQuery.bindValue(QStringLiteral(":profile_id"), guidToString(profileId));
            execOrThrow(deleteQuery, QStringLiteral("Delete EPG"));
        }

        QSqlQuery insertQuery(database);
        insertQuery.prepare(QStringLiteral(R"sql(
            INSERT INTO epg_entries (channel_id, profile_id, title, sub_title, description, start_unix, stop_unix)
            VALUES (:channel_id, :profile_id, :title, :sub_title, :description, :start_unix, :stop_unix)
        )sql"));

        for (const auto &entry : entries) {
            insertQuery.bindValue(QStringLiteral(":channel_id"), entry.channelId);
            insertQuery.bindValue(QStringLiteral(":profile_id"), guidToString(profileId));
            insertQuery.bindValue(QStringLiteral(":title"), entry.title);
            insertQuery.bindValue(
                QStringLiteral(":sub_title"),
                entry.subTitle.isEmpty() ? QVariant {} : QVariant(entry.subTitle));
            insertQuery.bindValue(
                QStringLiteral(":description"),
                entry.description.isEmpty() ? QVariant {} : QVariant(entry.description));
            insertQuery.bindValue(QStringLiteral(":start_unix"), entry.start.toSecsSinceEpoch());
            insertQuery.bindValue(QStringLiteral(":stop_unix"), entry.stop.toSecsSinceEpoch());
            execOrThrow(insertQuery, QStringLiteral("Insert EPG entry"));
        }

        if (!database.commit()) {
            throw std::runtime_error(
                QStringLiteral("Transaction commit failed: %1")
                    .arg(database.lastError().text()).toStdString());
        }
    } catch (...) {
        database.rollback();
        throw;
    }
}

QList<EpgEntry> DatabaseService::queryEpg(
    const QUuid &profileId,
    const QString &channelId,
    const QDateTime &from,
    const QDateTime &to) const
{
    ScopedConnection connection(m_databaseFilePath);
    auto &database = connection.database();

    QSqlQuery query(database);
    query.prepare(QStringLiteral(R"sql(
        SELECT channel_id, title, sub_title, description, start_unix, stop_unix
        FROM epg_entries
        WHERE profile_id = :profile_id
          AND channel_id = :channel_id
          AND stop_unix  > :from_unix
          AND start_unix < :to_unix
        ORDER BY start_unix
    )sql"));
    query.bindValue(QStringLiteral(":profile_id"), guidToString(profileId));
    query.bindValue(QStringLiteral(":channel_id"), channelId);
    query.bindValue(QStringLiteral(":from_unix"), from.toSecsSinceEpoch());
    query.bindValue(QStringLiteral(":to_unix"), to.toSecsSinceEpoch());
    execOrThrow(query, QStringLiteral("Query EPG"));

    QList<EpgEntry> entries;
    while (query.next()) {
        entries.push_back(epgEntryFromQuery(query));
    }

    return entries;
}

QString DatabaseService::cachedIconByHash(const QString &urlHash) const
{
    ScopedConnection connection(m_databaseFilePath);
    auto &database = connection.database();

    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT local_path FROM icon_cache WHERE url_hash = :url_hash"));
    query.bindValue(QStringLiteral(":url_hash"), urlHash);
    execOrThrow(query, QStringLiteral("Lookup icon cache"));
    return query.next() ? query.value(0).toString() : QString {};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void DatabaseService::upsertIconCache(const QString &urlHash, const QString &localPath, const qint64 fetchedAtUnix) const
{
    ScopedConnection connection(m_databaseFilePath);
    auto &database = connection.database();

    QSqlQuery query(database);
    query.prepare(QStringLiteral(R"sql(
        INSERT INTO icon_cache (url_hash, local_path, fetched_at)
        VALUES (:url_hash, :local_path, :fetched_at)
        ON CONFLICT(url_hash) DO UPDATE SET
            local_path = excluded.local_path,
            fetched_at = excluded.fetched_at
    )sql"));
    query.bindValue(QStringLiteral(":url_hash"), urlHash);
    query.bindValue(QStringLiteral(":local_path"), localPath);
    query.bindValue(QStringLiteral(":fetched_at"), fetchedAtUnix);
    execOrThrow(query, QStringLiteral("Upsert icon cache"));
}

QString CatchupProgress::keyFor(const Channel &channel, const QDateTime &programStart)
{
    const QJsonArray identity {
        guidToString(channel.profileId), channel.id, static_cast<int>(channel.source),
        channel.tvgId.trimmed().toCaseFolded(),
        channel.source == ChannelSource::M3U ? channel.streamUrl : QString {},
        QString::number(programStart.toMSecsSinceEpoch())
    };
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

qint64 CatchupProgress::resumeSeconds(const qint64 programEndMs) const
{
    const auto durationMs = programEndMs - programStartMs;
    if (positionMs < 60000 || durationMs <= 0 || positionMs >= durationMs - 60000) {
        return 0;
    }
    return (positionMs / 60000) * 60;
}

QList<CatchupProgress> DatabaseService::loadCatchupProgress() const
{
    ScopedConnection connection(m_databaseFilePath);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("SELECT resume_key, profile_id, program_start_ms, program_stop_ms, position_ms, expires_at_ms FROM catchup_progress"));
    execOrThrow(query, QStringLiteral("Load catch-up progress"));
    QList<CatchupProgress> result;
    while (query.next()) {
        result.append({ query.value(0).toString(), QUuid(query.value(1).toString()),
            query.value(2).toLongLong(), query.value(3).toLongLong(),
            query.value(4).toLongLong(), query.value(5).toLongLong() });
    }
    return result;
}

void DatabaseService::saveCatchupProgress(const CatchupProgress &progress) const
{
    if (progress.resumeSeconds(progress.programStopMs) == 0) {
        removeCatchupProgress(progress.key);
        return;
    }
    ScopedConnection connection(m_databaseFilePath);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral(R"sql(
        INSERT INTO catchup_progress VALUES (:key, :profile, :start, :stop, :position, :expires)
        ON CONFLICT(resume_key) DO UPDATE SET
            program_stop_ms = excluded.program_stop_ms,
            position_ms = excluded.position_ms,
            expires_at_ms = excluded.expires_at_ms
    )sql"));
    query.bindValue(QStringLiteral(":key"), progress.key);
    query.bindValue(QStringLiteral(":profile"), guidToString(progress.profileId));
    query.bindValue(QStringLiteral(":start"), progress.programStartMs);
    query.bindValue(QStringLiteral(":stop"), progress.programStopMs);
    query.bindValue(QStringLiteral(":position"), progress.positionMs);
    query.bindValue(QStringLiteral(":expires"), progress.expiresAtMs);
    execOrThrow(query, QStringLiteral("Save catch-up progress"));
}

void DatabaseService::removeCatchupProgress(const QString &key) const
{
    ScopedConnection connection(m_databaseFilePath);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("DELETE FROM catchup_progress WHERE resume_key = :key"));
    query.bindValue(QStringLiteral(":key"), key);
    execOrThrow(query, QStringLiteral("Remove catch-up progress"));
}

} // namespace OKILTV::Core
