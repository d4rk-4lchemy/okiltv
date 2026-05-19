#include "epgservice.h"

#include <QBuffer>
#include <QRegularExpression>
#include <QTimeZone>
#include <QXmlStreamReader>

#include <algorithm>
#include <stdexcept>

#if __has_include(<zlib.h>)
#include <zlib.h>
#elif __has_include(<QtZlib/zlib.h>)
#include <QtZlib/zlib.h>
#else
#error "zlib headers are required for XMLTV gzip/zlib decompression support."
#endif

namespace OKILTV::Core {

namespace {

QString normalizeOffset(QString value)
{
    value = value.trimmed();
    static const QRegularExpression offsetExpression(QStringLiteral(R"(([+-])(\d{2})(\d{2})$)"));
    const auto match = offsetExpression.match(value);
    if (!match.hasMatch()) {
        return value;
    }

    return QStringLiteral("%1 %2%3:%4")
        .arg(value.left(value.size() - 5))
        .arg(match.captured(1))
        .arg(match.captured(2))
        .arg(match.captured(3));
}

bool looksLikeZlibHeader(const QByteArray &payload)
{
    if (payload.size() < 2) {
        return false;
    }

    const auto cmf = static_cast<quint8>(payload.at(0));
    const auto flg = static_cast<quint8>(payload.at(1));
    if ((cmf & 0x0F) != 8) {
        return false;
    }

    return (((cmf << 8) + flg) % 31) == 0;
}

bool looksCompressedPayload(const QByteArray &payload)
{
    if (payload.size() < 2) {
        return false;
    }

    const auto b0 = static_cast<quint8>(payload.at(0));
    const auto b1 = static_cast<quint8>(payload.at(1));
    const auto isGzip = b0 == 0x1F && b1 == 0x8B;
    return isGzip || looksLikeZlibHeader(payload);
}

QByteArray inflatePayload(const QByteArray &payload)
{
    if (payload.isEmpty()) {
        return {};
    }

    constexpr int kChunkSize = 16 * 1024;
    constexpr qsizetype kMaxOutputBytes = 128 * 1024 * 1024;

    z_stream stream {};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(payload.constData()));
    stream.avail_in = static_cast<uInt>(payload.size());

    if (inflateInit2(&stream, MAX_WBITS + 32) != Z_OK) {
        throw std::runtime_error("Failed to initialize XMLTV decompressor.");
    }

    QByteArray inflated;
    inflated.reserve(std::min<qsizetype>(payload.size() * 2, 512 * 1024));
    char outputBuffer[kChunkSize];
    int inflateCode = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef *>(outputBuffer);
        stream.avail_out = kChunkSize;

        inflateCode = inflate(&stream, Z_NO_FLUSH);
        if (inflateCode != Z_OK && inflateCode != Z_STREAM_END) {
            inflateEnd(&stream);
            throw std::runtime_error("Failed to decompress XMLTV payload.");
        }

        const auto produced = kChunkSize - static_cast<int>(stream.avail_out);
        if (produced > 0) {
            inflated.append(outputBuffer, produced);
            if (inflated.size() > kMaxOutputBytes) {
                inflateEnd(&stream);
                throw std::runtime_error("XMLTV payload is too large after decompression.");
            }
        }
    } while (inflateCode != Z_STREAM_END);

    inflateEnd(&stream);
    return inflated;
}

} // namespace

QList<EpgEntry> EpgService::parseEntries(const QByteArray &payload)
{
    if (payload.trimmed().isEmpty()) {
        throw std::runtime_error("XMLTV payload is empty.");
    }

    auto normalizedPayload = payload;
    if (looksCompressedPayload(payload)) {
        normalizedPayload = inflatePayload(payload);
    }
    if (normalizedPayload.trimmed().isEmpty()) {
        throw std::runtime_error("XMLTV payload is empty.");
    }

    QBuffer buffer;
    buffer.setData(normalizedPayload);
    buffer.open(QIODevice::ReadOnly);
    return parseEntries(&buffer);
}

QList<EpgEntry> EpgService::parseEntries(QIODevice *device)
{
    if (device == nullptr) {
        throw std::runtime_error("XMLTV payload device is null.");
    }

    QList<EpgEntry> entries;
    QXmlStreamReader reader(device);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    reader.setEntityExpansionLimit(1024);
#endif

    bool sawRootElement = false;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token != QXmlStreamReader::StartElement) {
            continue;
        }

        const auto name = reader.name();
        if (!sawRootElement) {
            sawRootElement = true;
            if (name != QStringLiteral("tv")) {
                throw std::runtime_error(
                    QStringLiteral("Unexpected XMLTV root element '%1'. Expected <tv>.")
                        .arg(name.toString())
                        .toStdString());
            }
        }

        if (name != QStringLiteral("programme")) {
            continue;
        }

        const auto parsed = parseProgramme(reader);
        if (parsed.has_value()) {
            entries.push_back(parsed.value());
        }
    }

    if (reader.hasError()) {
        throw std::runtime_error(
            QStringLiteral("XMLTV parse failed at line %1, column %2: %3")
                .arg(reader.lineNumber())
                .arg(reader.columnNumber())
                .arg(reader.errorString())
                .toStdString());
    }
    if (!sawRootElement) {
        throw std::runtime_error("XMLTV payload does not contain a root element.");
    }

    return entries;
}

EpgService::Snapshot EpgService::buildSnapshot(const QList<EpgEntry> &entries)
{
    Snapshot snapshot;
    snapshot.totalEntries = static_cast<int>(entries.size());
    snapshot.allEntries = entries;
    std::sort(snapshot.allEntries.begin(), snapshot.allEntries.end(), [](const EpgEntry &left, const EpgEntry &right) {
        if (left.start == right.start) {
            return left.channelId.toLower() < right.channelId.toLower();
        }
        return left.start < right.start;
    });

    for (const auto &entry : snapshot.allEntries) {
        const auto key = normalizeKey(entry.channelId);
        snapshot.index[key].push_back(entry);
        const auto currentMaxStop = snapshot.maxStopByChannelId.value(key);
        if (!currentMaxStop.isValid() || entry.stop > currentMaxStop) {
            snapshot.maxStopByChannelId.insert(key, entry.stop);
        }
    }

    return snapshot;
}

void EpgService::loadFromBytes(const QByteArray &payload)
{
    applySnapshot(buildSnapshot(parseEntries(payload)));
}

void EpgService::loadFromEntries(const QList<EpgEntry> &entries)
{
    applySnapshot(buildSnapshot(entries));
}

void EpgService::applySnapshot(Snapshot snapshot)
{
    applySnapshot(std::make_shared<Snapshot>(std::move(snapshot)));
}

void EpgService::applySnapshot(std::shared_ptr<const Snapshot> snapshot)
{
    if (!snapshot) {
        snapshot = std::make_shared<Snapshot>();
    }

    QWriteLocker locker(&m_lock);
    m_snapshot = std::move(snapshot);
}

void EpgService::clear()
{
    applySnapshot(Snapshot {});
}

std::optional<EpgEntry> EpgService::currentProgram(const QString &tvgId) const
{
    const auto now = QDateTime::currentDateTimeUtc();
    const auto candidates = programsInRange(tvgId, now.addSecs(-60), now.addSecs(60));
    for (const auto &entry : candidates) {
        if (entry.start <= now && now < entry.stop) {
            return entry;
        }
    }

    return std::nullopt;
}

std::optional<EpgEntry> EpgService::nextProgram(const QString &tvgId) const
{
    const auto now = QDateTime::currentDateTimeUtc();
    const auto current = currentProgram(tvgId);
    const auto from = current.has_value() ? current->stop : now;

    const auto candidates = programsInRange(tvgId, from, from.addSecs(static_cast<qint64>(6) * 3600));
    for (const auto &entry : candidates) {
        if (entry.start >= from) {
            return entry;
        }
    }

    return std::nullopt;
}

QList<EpgEntry> EpgService::programsInRange(const QString &tvgId, const QDateTime &from, const QDateTime &to) const
{
    std::shared_ptr<const Snapshot> snapshot;
    {
        QReadLocker locker(&m_lock);
        snapshot = m_snapshot;
    }

    if (!snapshot) {
        return {};
    }

    const auto it = snapshot->index.constFind(normalizeKey(tvgId));
    if (it == snapshot->index.cend()) {
        return {};
    }

    const auto &entries = it.value();
    QList<EpgEntry> result;
    for (const auto &entry : entries) {
        if (entry.start >= to) {
            break;
        }
        if (entry.stop > from && entry.start < to) {
            result.push_back(entry);
        }
    }

    return result;
}

QList<EpgEntry> EpgService::allEntries() const
{
    std::shared_ptr<const Snapshot> snapshot;
    {
        QReadLocker locker(&m_lock);
        snapshot = m_snapshot;
    }

    if (!snapshot) {
        return {};
    }

    return snapshot->allEntries;
}

QDateTime EpgService::channelMaxStop(const QString &tvgId) const
{
    std::shared_ptr<const Snapshot> snapshot;
    {
        QReadLocker locker(&m_lock);
        snapshot = m_snapshot;
    }

    if (!snapshot) {
        return {};
    }

    return snapshot->maxStopByChannelId.value(normalizeKey(tvgId));
}

int EpgService::totalEntries() const
{
    std::shared_ptr<const Snapshot> snapshot;
    QReadLocker locker(&m_lock);
    snapshot = m_snapshot;
    return snapshot ? snapshot->totalEntries : 0;
}

std::optional<EpgEntry> EpgService::parseProgramme(QXmlStreamReader &reader)
{
    const auto attributes = reader.attributes();
    const auto channelId = attributes.value(QStringLiteral("channel")).toString();
    const auto startRaw = attributes.value(QStringLiteral("start")).toString();
    const auto stopRaw = attributes.value(QStringLiteral("stop")).toString();

    QDateTime start;
    QDateTime stop;
    if (channelId.isEmpty() || !tryParseDate(startRaw, &start) || !tryParseDate(stopRaw, &stop)) {
        return std::nullopt;
    }

    QString title;
    QString subTitle;
    QString description;
    QString episodeNum;

    while (!(reader.isEndElement() && reader.name() == QStringLiteral("programme")) && !reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement()) {
            continue;
        }

        if (reader.name() == QStringLiteral("title") && title.isEmpty()) {
            title = reader.readElementText();
            continue;
        }

        if (reader.name() == QStringLiteral("desc") && description.isEmpty()) {
            description = reader.readElementText();
            continue;
        }

        if (reader.name() == QStringLiteral("sub-title") && subTitle.isEmpty()) {
            subTitle = reader.readElementText();
            continue;
        }

        if (reader.name() == QStringLiteral("episode-num") && episodeNum.isEmpty()) {
            episodeNum = reader.readElementText();
            continue;
        }
    }

    EpgEntry entry;
    entry.channelId = channelId;
    entry.title = title.isEmpty() ? QStringLiteral("(no title)") : title;
    entry.description = description;
    entry.episodeNum = episodeNum;
    entry.start = start;
    entry.stop = stop;
    entry.subTitle = subTitle;
    return entry;
}

bool EpgService::tryParseDate(const QString &value, QDateTime *result)
{
    const auto trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    const auto parts = trimmed.split(u' ', Qt::SkipEmptyParts);
    const auto basePart = parts.isEmpty() ? trimmed.left(14) : parts.front();
    auto dateTime = QDateTime::fromString(basePart, QStringLiteral("yyyyMMddHHmmss"));
    if (!dateTime.isValid()) {
        return false;
    }

    if (parts.size() == 2) {
        static const QRegularExpression offsetExpression(QStringLiteral(R"(^([+-])(\d{2})(\d{2})$)"));
        const auto match = offsetExpression.match(parts[1]);
        if (match.hasMatch()) {
            const auto sign = match.captured(1) == QStringLiteral("-") ? -1 : 1;
            const auto hours = match.captured(2).toInt();
            const auto minutes = match.captured(3).toInt();
            const auto seconds = sign * ((hours * 3600) + (minutes * 60));
            dateTime = QDateTime(dateTime.date(), dateTime.time(), QTimeZone(seconds)).toUTC();
            *result = dateTime;
            return true;
        }
    }

    const auto normalized = normalizeOffset(trimmed);
    auto parsed = QDateTime::fromString(normalized, Qt::ISODate);
    if (parsed.isValid()) {
        *result = parsed.toUTC();
        return true;
    }

    *result = QDateTime(dateTime.date(), dateTime.time(), QTimeZone::UTC);
    return true;
}

QString EpgService::normalizeKey(const QString &channelId)
{
    return channelId.trimmed().toLower();
}

} // namespace OKILTV::Core
