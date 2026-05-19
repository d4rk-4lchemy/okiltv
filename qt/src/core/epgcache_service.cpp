#include "epgcache_service.h"

#include "appdatapaths.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QSaveFile>
#include <QTimeZone>

#include <stdexcept>

namespace OKILTV::Core {

namespace {

constexpr quint32 kMagic = 0x45504743;  // ASCII "EPGC"
constexpr quint32 kVersion = 2;

QDataStream &operator<<(QDataStream &stream, const EpgEntry &entry)
{
    stream << entry.channelId;
    stream << entry.title;
    stream << entry.description;
    stream << entry.episodeNum;
    stream << entry.subTitle;
    stream << entry.start.toMSecsSinceEpoch();
    stream << entry.stop.toMSecsSinceEpoch();
    return stream;
}

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
    switch (profile.type) {
    case ProfileType::Xtream:
        return QStringLiteral("xtream|%1|%2|%3")
            .arg(profile.xtreamBaseUrl.trimmed(), profile.xtreamUsername.trimmed(), profile.xtreamPassword);
    case ProfileType::M3UUrl:
        return QStringLiteral("xmltv|%1|m3uurl").arg(profile.xmltvUrl.trimmed());
    case ProfileType::M3UFile:
        return QStringLiteral("xmltv|%1|m3ufile").arg(profile.xmltvUrl.trimmed());
    }

    return QString {};
}

} // namespace

EpgCacheService::LoadResult EpgCacheService::load(const QUuid &profileId) const
{
    QFile file(AppDataPaths::epgCacheFile(profileId));
    if (!file.exists()) {
        return {};
    }

    if (!file.open(QIODevice::ReadOnly)) {
        return { LoadStatus::Invalid, {} };
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);

    quint32 magic = 0;
    quint32 version = 0;
    QString profileIdString;
    QString sourceFingerprint;
    qint64 fetchedAtMs = -1;
    quint32 entryCount = 0;

    stream >> magic;
    stream >> version;
    stream >> profileIdString;
    stream >> sourceFingerprint;
    stream >> fetchedAtMs;
    stream >> entryCount;

    if (stream.status() != QDataStream::Ok || magic != kMagic || version == 0 || version > kVersion) {
        return { LoadStatus::Invalid, {} };
    }

    QList<EpgEntry> entries;
    entries.reserve(static_cast<qsizetype>(entryCount));
    for (quint32 index = 0; index < entryCount; ++index) {
        EpgEntry entry;
        if (version == 1) {
            if (!readVersion1Entry(stream, &entry)) {
                return { LoadStatus::Invalid, {} };
            }
        } else {
            stream >> entry;
            if (stream.status() != QDataStream::Ok) {
                return { LoadStatus::Invalid, {} };
            }
        }
        if (stream.status() != QDataStream::Ok) {
            return { LoadStatus::Invalid, {} };
        }
        entries.push_back(entry);
    }

    CacheData data;
    data.profileId = parseGuid(profileIdString);
    data.sourceFingerprint = sourceFingerprint;
    data.fetchedAt = fetchedAtMs >= 0 ? QDateTime::fromMSecsSinceEpoch(fetchedAtMs, QTimeZone::UTC) : QDateTime {};
    data.snapshot = EpgService::buildSnapshot(entries);

    if (data.profileId.isNull() || data.profileId != profileId || !data.fetchedAt.isValid()) {
        return { LoadStatus::Invalid, {} };
    }

    return { LoadStatus::Loaded, std::move(data) };
}

void EpgCacheService::save(const CacheData &data) const
{
    QSaveFile file(AppDataPaths::epgCacheFile(data.profileId));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error(
            QStringLiteral("Failed to open EPG cache file %1").arg(file.fileName()).toStdString());
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << kMagic;
    stream << kVersion;
    stream << guidToString(data.profileId);
    stream << data.sourceFingerprint;
    stream << data.fetchedAt.toUTC().toMSecsSinceEpoch();
    stream << static_cast<quint32>(data.snapshot.allEntries.size());
    for (const auto &entry : data.snapshot.allEntries) {
        stream << entry;
    }

    if (stream.status() != QDataStream::Ok || !file.commit()) {
        throw std::runtime_error(
            QStringLiteral("Failed to write EPG cache file %1").arg(file.fileName()).toStdString());
    }
}

void EpgCacheService::remove(const QUuid &profileId) const
{
    QFile::remove(AppDataPaths::epgCacheFile(profileId));
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
