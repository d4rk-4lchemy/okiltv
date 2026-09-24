#include "epgcache_service.h"

#include "appdatapaths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSet>
#include <QDataStream>
#include <QFile>
#include <QSaveFile>
#include <QTimeZone>

#include <stdexcept>

namespace OKILTV::Core {

namespace {

constexpr quint32 kMagic = 0x45504743;  // ASCII "EPGC"
constexpr quint32 kVersion = 2;

QDataStream &operator>>(QDataStream &stream, EpgEntry &entry)
{
    qint64 startMs = -1;
    qint64 stopMs = -1;
    stream >> entry.channelId;
    stream >> entry.title;
    stream >> entry.description;
    stream >> entry.episodeNum;
    stream >> entry.subTitle;
    stream >> startMs;
    stream >> stopMs;
    entry.start = startMs >= 0 ? QDateTime::fromMSecsSinceEpoch(startMs, QTimeZone::UTC) : QDateTime {};
    entry.stop = stopMs >= 0 ? QDateTime::fromMSecsSinceEpoch(stopMs, QTimeZone::UTC) : QDateTime {};
    return stream;
}

bool readVersion1Entry(QDataStream &stream, EpgEntry *entry)
{
    if (!entry) {
        return false;
    }

    qint64 startMs = -1;
    qint64 stopMs = -1;
    stream >> entry->channelId;
    stream >> entry->title;
    stream >> entry->description;
    stream >> entry->episodeNum;
    stream >> startMs;
    stream >> stopMs;
    if (stream.status() != QDataStream::Ok) {
        return false;
    }

    entry->start = startMs >= 0 ? QDateTime::fromMSecsSinceEpoch(startMs, QTimeZone::UTC) : QDateTime {};
    entry->stop = stopMs >= 0 ? QDateTime::fromMSecsSinceEpoch(stopMs, QTimeZone::UTC) : QDateTime {};
    entry->subTitle.clear();
    return true;
}

QString sourceDescriptor(const ServerProfile &profile)
{
    const auto xmltvSource = profile.xmltvUrl.trimmed().isEmpty()
        ? profile.discoveredXmltvUrls.join(u'\n') : profile.xmltvUrl.trimmed();
    switch (profile.type) {
    case ProfileType::Xtream:
        if (!profile.xmltvUrl.trimmed().isEmpty())
            return QStringLiteral("xmltv|%1|xtream").arg(profile.xmltvUrl.trimmed());
        return QStringLiteral("xtream|%1|%2|%3")
            .arg(profile.xtreamBaseUrl.trimmed(), profile.xtreamUsername.trimmed(), profile.xtreamPassword);
    case ProfileType::M3UUrl:
        return QStringLiteral("xmltv|%1|m3uurl").arg(xmltvSource);
    case ProfileType::M3UFile:
        return QStringLiteral("xmltv|%1|m3ufile").arg(xmltvSource);
    }

    return QString {};
}

} // namespace

namespace {
QMutex publicationMutex;
QHash<QString, std::weak_ptr<std::atomic_bool>> importTokens;
QString generationPath(const QUuid &id)
{
    return QDir(AppDataPaths::epgCacheDirectory()).filePath(
        id.toString(QUuid::WithoutBraces).toLower() + u'-' + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".sqlite"));
}
QString publishedPath(const QUuid &id)
{
    QFile file(EpgCacheService::manifestFile(id));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) return {};
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    const auto name = object.value(QStringLiteral("file")).toString();
    // A manifest is never allowed to point outside this profile's cache.
    if (!name.startsWith(id.toString(QUuid::WithoutBraces).toLower() + u'-')
        || !name.endsWith(QStringLiteral(".sqlite")) || QFileInfo(name).fileName() != name) return {};
    return QDir(AppDataPaths::epgCacheDirectory()).filePath(name);
}
}

QString EpgCacheService::manifestFile(const QUuid &profileId)
{
    return AppDataPaths::epgCacheFile(profileId) + QStringLiteral(".json");
}
EpgCacheService::Cancellation EpgCacheService::beginImport(const QUuid &profileId)
{
    QMutexLocker lock(&publicationMutex);
    const auto key = manifestFile(profileId);
    if (const auto previous = importTokens.value(key).lock()) previous->store(true);
    auto token = std::make_shared<std::atomic_bool>(false);
    importTokens.insert(key, token);
    return token;
}
void EpgCacheService::cancel(const Cancellation &token)
{
    QMutexLocker lock(&publicationMutex);
    if (token) token->store(true);
}
void EpgCacheService::invalidateSource(const QUuid &profileId)
{
    QMutexLocker lock(&publicationMutex);
    if (const auto token = importTokens.take(manifestFile(profileId)).lock()) token->store(true);
}
EpgCacheService::CacheData EpgCacheService::build(const QUuid &profileId, const QString &fingerprint,
    const EpgStore::Producer &producer, bool deduplicate, const Cancellation &token) const
{
    CacheData data;
    data.profileId = profileId; data.sourceFingerprint = fingerprint;
    data.fetchedAt = QDateTime::currentDateTimeUtc();
    data.snapshot.store = EpgStore::create(generationPath(profileId),
        {profileId, fingerprint, data.fetchedAt, 0}, producer, deduplicate,
        [token] { return token && token->load(); });
    data.snapshot.totalEntries = static_cast<int>(data.snapshot.store->metadata().entries);
    return data;
}

EpgCacheService::LoadResult EpgCacheService::load(const QUuid &profileId, const Cancellation &token) const
{
    try {
        // The production importer is serialized. Remove transfer files left by a
        // crashed process once per data root, before starting a new transfer.
        static QSet<QString> cleanedRoots;
        {
            QMutexLocker lock(&publicationMutex);
            const auto root = AppDataPaths::epgCacheDirectory();
            if (!cleanedRoots.contains(root)) {
                const QDir directory(root);
                for (const auto &name : directory.entryList({QStringLiteral("download-*.source")}, QDir::Files))
                    QFile::remove(directory.filePath(name));
                cleanedRoots.insert(root);
            }
        }
        // Serialize manifest open with publication/retirement, then pin its generation.
        {
            QMutexLocker lock(&publicationMutex);
            const auto path = publishedPath(profileId);
            if (!path.isEmpty()) {
                auto store = EpgStore::open(path);
                const auto &meta = store->metadata();
                if (meta.profileId != profileId) return {LoadStatus::Invalid, {}};
                CacheData data {profileId, meta.fingerprint, meta.fetchedAt, {}};
                data.snapshot.store = std::move(store);
                data.snapshot.totalEntries = static_cast<int>(meta.entries);
                // Orphaned generations include interrupted imports; pinned readers
                // finish before their retired files are actually removed.
                QDir directory(AppDataPaths::epgCacheDirectory());
                const auto pattern = profileId.toString(QUuid::WithoutBraces).toLower() + QStringLiteral("-*.sqlite");
                for (const auto &name : directory.entryList({pattern}, QDir::Files)) {
                    const auto orphan = directory.filePath(name);
                    if (orphan != path) EpgStore::retireFile(orphan);
                }
                return {LoadStatus::Loaded, std::move(data)};
            }
        }
        QFile file(AppDataPaths::epgCacheFile(profileId));
        if (!file.exists()) return {};
        if (!file.open(QIODevice::ReadOnly)) return {LoadStatus::Invalid, {}};
        QDataStream stream(&file); stream.setVersion(QDataStream::Qt_6_0);
        quint32 magic = 0, version = 0, count = 0;
        QString id, fingerprint; qint64 fetched = -1;
        stream >> magic >> version >> id >> fingerprint >> fetched >> count;
        if (stream.status() != QDataStream::Ok || magic != kMagic || version == 0 || version > kVersion
            || parseGuid(id) != profileId || fetched < 0 || count > static_cast<quint64>(file.size() / 16))
            return {LoadStatus::Invalid, {}};
        CacheData data {profileId, fingerprint, QDateTime::fromMSecsSinceEpoch(fetched, QTimeZone::UTC), {}};
        data.snapshot.store = EpgStore::create(generationPath(profileId), {profileId, fingerprint, data.fetchedAt, 0},
            [&](const EpgStore::Sink &sink) {
                for (quint32 i = 0; i < count; ++i) {
                    EpgEntry entry;
                    if (version == 1) readVersion1Entry(stream, &entry); else stream >> entry;
                    if (stream.status() != QDataStream::Ok) throw std::runtime_error("Invalid legacy EPG cache.");
                    sink(entry);
                }
            }, false, [token] { return token && token->load(); });
        data.snapshot.totalEntries = static_cast<int>(data.snapshot.store->metadata().entries);
        file.close();
        save(data, token);
        return {LoadStatus::Loaded, std::move(data)};
    } catch (const std::exception &) {
        return {LoadStatus::Invalid, {}};
    }
}

void EpgCacheService::save(const CacheData &data, const Cancellation &token) const
{
    auto store = data.snapshot.store;
    if (!store) {
        store = EpgStore::create(generationPath(data.profileId),
            {data.profileId, data.sourceFingerprint, data.fetchedAt, 0}, [&](const EpgStore::Sink &sink) {
                for (const auto &entry : data.snapshot.allEntries) sink(entry);
            }, false, [token] { return token && token->load(); });
    }
    QMutexLocker lock(&publicationMutex);
    if (token && token->load()) throw std::runtime_error("EPG import cancelled.");
    const auto previous = publishedPath(data.profileId);
    QSaveFile manifest(manifestFile(data.profileId));
    const auto json = QJsonDocument(QJsonObject {{QStringLiteral("file"), QFileInfo(store->path()).fileName()}}).toJson(QJsonDocument::Compact);
    if (!manifest.open(QIODevice::WriteOnly) || manifest.write(json) != json.size() || !manifest.commit())
        throw std::runtime_error("Cannot publish EPG cache.");
    store->keep();
    if (!previous.isEmpty() && previous != store->path()) EpgStore::retireFile(previous);
    QFile::remove(AppDataPaths::epgCacheFile(data.profileId));
}

void EpgCacheService::remove(const QUuid &profileId) const
{
    QMutexLocker lock(&publicationMutex);
    if (const auto token = importTokens.take(manifestFile(profileId)).lock()) token->store(true);
    QFile::remove(manifestFile(profileId));
    QFile::remove(AppDataPaths::epgCacheFile(profileId));
    QDir directory(AppDataPaths::epgCacheDirectory());
    const auto pattern = profileId.toString(QUuid::WithoutBraces).toLower() + QStringLiteral("-*.sqlite");
    for (const auto &name : directory.entryList({pattern}, QDir::Files)) EpgStore::retireFile(directory.filePath(name));
}

QString EpgCacheService::sourceFingerprint(const ServerProfile &profile)
{
    const auto descriptor = sourceDescriptor(profile).trimmed();
    if (descriptor.isEmpty()) {
        return QString {};
    }

    return QString::fromLatin1(
        QCryptographicHash::hash(descriptor.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool EpgCacheService::matchesProfile(const CacheData &data, const ServerProfile &profile)
{
    return data.profileId == profile.id && data.sourceFingerprint == sourceFingerprint(profile);
}

QDateTime EpgCacheService::nextRefreshAt(const QDateTime &fetchedAt, const int refreshIntervalMinutes)
{
    if (!fetchedAt.isValid() || refreshIntervalMinutes <= 0) {
        return {};
    }

    return fetchedAt.addSecs(static_cast<qint64>(refreshIntervalMinutes) * 60);
}

bool EpgCacheService::isStale(
    const QDateTime &fetchedAt,
    const int refreshIntervalMinutes,
    const QDateTime &now)
{
    const auto dueAt = nextRefreshAt(fetchedAt, refreshIntervalMinutes);
    return !dueAt.isValid() || dueAt <= now;
}

qint64 EpgCacheService::ageSeconds(const QDateTime &fetchedAt, const QDateTime &now)
{
    return fetchedAt.isValid() ? fetchedAt.secsTo(now) : -1;
}

} // namespace OKILTV::Core
