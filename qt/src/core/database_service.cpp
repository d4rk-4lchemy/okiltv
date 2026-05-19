#include "database_service.h"

#include "appdatapaths.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimeZone>
#include <QVariant>

#include <algorithm>
#include <stdexcept>

namespace OKILTV::Core {

namespace {

class ScopedConnection
{
public:
    explicit ScopedConnection(const QString &databaseFilePath)
        : m_name(QStringLiteral("iptvplayer_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
    {
        m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        m_database.setDatabaseName(databaseFilePath);
        if (!m_database.open()) {
            throw std::runtime_error(
                QStringLiteral("Failed to open SQLite database %1: %2")
                    .arg(databaseFilePath, m_database.lastError().text())
                    .toStdString());
        }
    }

    ~ScopedConnection()
    {
        const auto name = m_name;
        m_database.close();
        m_database = {};
        QSqlDatabase::removeDatabase(name);
    }

    QSqlDatabase &database()
    {
        return m_database;
    }

private:
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

QSqlDatabase &schemaReadyDatabase(ScopedConnection &connection)
{
    auto &database = connection.database();
    ensureSchemaOnConnection(database);
    return database;
}

Channel channelFromQuery(const QSqlQuery &query, const QUuid &profileId)
{
    Channel channel;
    channel.id = query.value(QStringLiteral("id")).toInt();
    channel.profileId = profileId;
    channel.name = query.value(QStringLiteral("name")).toString();
    channel.streamUrl = query.value(QStringLiteral("stream_url")).toString();
    channel.categoryId = normalizeChannelCategoryId(query.value(QStringLiteral("category_id")).toString());
    channel.categoryName = query.value(QStringLiteral("category_name")).toString();
    channel.tvgId = query.value(QStringLiteral("tvg_id")).toString();
    channel.tvgName = query.value(QStringLiteral("tvg_name")).toString();
    channel.iconUrl = query.value(QStringLiteral("icon_url")).toString();
    channel.cachedIconPath = query.value(QStringLiteral("cached_icon")).toString();
    channel.source = channelSourceFromString(query.value(QStringLiteral("source")).toString());
    channel.sortOrder = query.value(QStringLiteral("sort_order")).toInt();
    channel.catchupSupported = query.value(QStringLiteral("catchup_supported")).toBool();
    channel.catchupWindowHours = std::max(0, query.value(QStringLiteral("catchup_window_hours")).toInt());
    channel.catchupMode = query.value(QStringLiteral("catchup_mode")).toString();
    channel.catchupSourceTemplate = query.value(QStringLiteral("catchup_source_template")).toString();
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

DatabaseService::DatabaseService(QString databaseFilePath)
    : m_databaseFilePath(databaseFilePath.isEmpty() ? AppDataPaths::databaseFile() : std::move(databaseFilePath))
{
    ensureSchema();
}

QString DatabaseService::databaseFilePath() const
{
    return m_databaseFilePath;
}

void DatabaseService::ensureSchema() const
{
    ScopedConnection connection(m_databaseFilePath);
    schemaReadyDatabase(connection);
}

void DatabaseService::upsertChannels(const QList<Channel> &channels) const
{
    ScopedConnection connection(m_databaseFilePath);
    auto &database = schemaReadyDatabase(connection);

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

        for (const auto &channel : channels) {
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
                    : channel.catchupSourceTemplate.trimmed());
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

void DatabaseService::replaceChannelsForProfile(const QUuid &profileId, const QList<Channel> &channels) const
{
    ScopedConnection connection(m_databaseFilePath);
    auto &database = schemaReadyDatabase(connection);

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

        for (const auto &channel : channels) {
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
                    : channel.catchupSourceTemplate.trimmed());
            execOrThrow(query, QStringLiteral("Upsert channel"));
        }

        QSqlQuery pruneQuery(database);
        if (channels.isEmpty()) {
            pruneQuery.prepare(QStringLiteral("DELETE FROM channels WHERE profile_id = :profile_id"));
        } else {
            QStringList ids;
            ids.reserve(channels.size());
            for (const auto &channel : channels) {
                ids.push_back(QString::number(channel.id));
            }
            pruneQuery.prepare(
                QStringLiteral("DELETE FROM channels WHERE profile_id = :profile_id AND id NOT IN (%1)")
                    .arg(ids.join(',')));
        }
        pruneQuery.bindValue(QStringLiteral(":profile_id"), guidToString(profileId));
        execOrThrow(pruneQuery, QStringLiteral("Prune stale channels"));

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

QList<Channel> DatabaseService::loadChannels(const QUuid &profileId) const
{
    ScopedConnection connection(m_databaseFilePath);
    auto &database = schemaReadyDatabase(connection);

    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT * FROM channels WHERE profile_id = :profile_id ORDER BY sort_order"));
    query.bindValue(QStringLiteral(":profile_id"), guidToString(profileId));
    execOrThrow(query, QStringLiteral("Load channels"));

    QList<Channel> channels;
    while (query.next()) {
        channels.push_back(channelFromQuery(query, profileId));
    }

    return channels;
}

void DatabaseService::updateCachedIcon(int channelId, const QUuid &profileId, const QString &localPath) const
{
    ScopedConnection connection(m_databaseFilePath);
    auto &database = schemaReadyDatabase(connection);

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
    auto &database = schemaReadyDatabase(connection);

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

    ScopedConnection connection(m_databaseFilePath);
    auto &database = schemaReadyDatabase(connection);

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
    auto &database = schemaReadyDatabase(connection);

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
    auto &database = schemaReadyDatabase(connection);

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
    auto &database = schemaReadyDatabase(connection);

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
    auto &database = schemaReadyDatabase(connection);

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

} // namespace OKILTV::Core
