#include "epgservice.h"

#include <QBuffer>
#include "debuglogger.h"
#include <QRegularExpression>
#include <QTimeZone>
#include <QXmlStreamReader>

#include <algorithm>
#include <stdexcept>

#if defined(OKILTV_USE_QT_ZLIB)
#include <QtZlib/zlib.h>
#else
#include <zlib.h>
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

// Pull-based decompression: QXmlStreamReader never owns a complete XML payload.
class XmlInput final : public QIODevice
{
public:
    XmlInput(QIODevice *source, EpgStore::Cancelled cancelled, qint64 maximum)
        : m_source(source), m_cancelled(std::move(cancelled)), m_maximum(maximum)
    {
        m_compressed = looksCompressedPayload(source->peek(2));
        if (m_compressed && inflateInit2(&m_stream, MAX_WBITS + 32) != Z_OK)
            throw std::runtime_error("Failed to initialize XMLTV decompressor.");
        open(QIODevice::ReadOnly);
    }
    ~XmlInput() override { if (m_compressed) inflateEnd(&m_stream); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return QIODevice::bytesAvailable() + (m_finished ? 0 : 1); }
    bool atEnd() const override { return m_finished && QIODevice::bytesAvailable() == 0; }
protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        if (m_cancelled && m_cancelled()) throw std::runtime_error("EPG import cancelled.");
        if (m_finished || maximum <= 0) return 0;
        maximum = std::min<qint64>(maximum, sizeof(m_input));
        qint64 produced = 0;
        if (!m_compressed) {
            produced = m_source->read(data, maximum);
            if (produced < 0) throw std::runtime_error("Cannot read XMLTV input.");
            m_finished = produced == 0;
        } else {
            while (produced == 0 && !m_finished) {
                if (!m_stream.avail_in) {
                    const auto n = m_source->read(m_input, sizeof(m_input));
                    if (n <= 0) throw std::runtime_error("Truncated compressed XMLTV payload.");
                    m_stream.next_in = reinterpret_cast<Bytef *>(m_input);
                    m_stream.avail_in = static_cast<uInt>(n);
                }
                m_stream.next_out = reinterpret_cast<Bytef *>(data);
                m_stream.avail_out = static_cast<uInt>(maximum);
                const auto code = inflate(&m_stream, Z_NO_FLUSH);
                produced = maximum - m_stream.avail_out;
                if (code == Z_STREAM_END) {
                    // RFC 1952 permits concatenated gzip members.
                    if (m_stream.avail_in || !m_source->atEnd()) {
                        if (inflateReset2(&m_stream, MAX_WBITS + 32) != Z_OK)
                            throw std::runtime_error("Cannot reset XMLTV decompressor.");
                    } else m_finished = true;
                } else if (code != Z_OK) throw std::runtime_error("Failed to decompress XMLTV payload.");
            }
        }
        m_bytes += produced;
        if (m_bytes > m_maximum) throw std::runtime_error("XMLTV exceeds the 2 GiB import limit.");
        return produced;
    }
    qint64 writeData(const char *, qint64) override { return -1; }
private:
    QIODevice *m_source;
    EpgStore::Cancelled m_cancelled;
    qint64 m_maximum;
    qint64 m_bytes = 0;
    bool m_compressed = false;
    bool m_finished = false;
    z_stream m_stream {};
    char m_input[64 * 1024];
};

} // namespace

QList<EpgEntry> EpgService::parseEntries(const QByteArray &payload)
{
    QBuffer buffer;
    buffer.setData(payload);
    buffer.open(QIODevice::ReadOnly);
    return parseEntries(&buffer);
}

QList<EpgEntry> EpgService::parseEntries(QIODevice *device)
{
    QList<EpgEntry> entries;
    streamEntries(device, [&](const EpgEntry &entry) { entries.push_back(entry); });
    return entries;
}

void EpgService::streamEntries(QIODevice *device, const EpgStore::Sink &sink,
    const EpgStore::Cancelled &cancelled, qint64 maximumBytes)
{
    if (!device || !device->isReadable()) throw std::runtime_error("XMLTV input is not readable.");
    XmlInput input(device, cancelled, maximumBytes);
    QXmlStreamReader reader(&input);
    reader.setEntityExpansionLimit(1024);
    bool sawRoot = false;
    while (!reader.atEnd()) {
        if (cancelled && cancelled()) throw std::runtime_error("EPG import cancelled.");
        if (reader.readNext() != QXmlStreamReader::StartElement) continue;
        if (!sawRoot) {
            sawRoot = true;
            if (reader.name() != QStringLiteral("tv"))
                throw std::runtime_error("Unexpected XMLTV root element. Expected <tv>.");
        }
        if (reader.name() == QStringLiteral("programme")) {
            if (const auto parsed = parseProgramme(reader)) sink(*parsed);
        }
    }
    if (reader.hasError()) throw std::runtime_error(
        QStringLiteral("XMLTV parse failed at line %1, column %2: %3")
            .arg(reader.lineNumber()).arg(reader.columnNumber()).arg(reader.errorString()).toStdString());
    if (!sawRoot) throw std::runtime_error("XMLTV payload does not contain a root element.");
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
        snapshot.prefixMaxStopsByChannelId[key].push_back(snapshot.maxStopByChannelId.value(key));
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

std::shared_ptr<const EpgService::Snapshot> EpgService::snapshot() const
{
    QReadLocker lock(&m_lock);
    return m_snapshot;
}

QThreadPool *EpgService::readPool()
{
    static QThreadPool pool;
    static const bool configured = [] { pool.setMaxThreadCount(2); return true; }();
    Q_UNUSED(configured);
    return &pool;
}

QThreadPool *EpgService::importPool()
{
    static QThreadPool pool;
    static const bool configured = [] { pool.setMaxThreadCount(1); return true; }();
    Q_UNUSED(configured);
    return &pool;
}

QHash<QString, QList<EpgEntry>> EpgService::programsForChannels(const QStringList &channels,
    const QDateTime &from, const QDateTime &to, int limit, bool summaries) const
{
    const auto pinned = snapshot();
    if (pinned->store) {
        try { return pinned->store->ranges(channels, from, to, limit, summaries); }
        catch (const std::exception &error) {
            DebugLogger::instance().log(QStringLiteral("epg.read"), QString::fromUtf8(error.what()));
            return {};
        }
    }
    EpgService reader;
    reader.applySnapshot(pinned);
    QHash<QString, QList<EpgEntry>> result;
    for (const auto &channel : channels) result.insert(channel.trimmed().toLower(), reader.programsInRange(channel, from, to, limit));
    return result;
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

QList<EpgEntry> EpgService::programsInRange(const QString &tvgId, const QDateTime &from, const QDateTime &to, const int limit) const
{
    if (from >= to || limit == 0) {
        return {};
    }
    std::shared_ptr<const Snapshot> snapshot;
    {
        QReadLocker locker(&m_lock);
        snapshot = m_snapshot;
    }

    if (!snapshot) {
        return {};
    }

    if (snapshot->store) {
        try { return snapshot->store->range(tvgId, from, to, limit); }
        catch (const std::exception &error) {
            DebugLogger::instance().log(QStringLiteral("epg.read"), QString::fromUtf8(error.what()));
            return {};
        }
    }

    const auto it = snapshot->index.constFind(normalizeKey(tvgId));
    if (it == snapshot->index.cend()) {
        return {};
    }

    const auto &entries = it.value();
    auto first = entries.cbegin();
    const auto stops = snapshot->prefixMaxStopsByChannelId.constFind(it.key());
    if (stops != snapshot->prefixMaxStopsByChannelId.cend() && stops->size() == entries.size()) {
        const auto candidate = std::upper_bound(stops->cbegin(), stops->cend(), from);
        first += std::distance(stops->cbegin(), candidate);
    }
    QList<EpgEntry> result;
    for (auto candidate = first; candidate != entries.cend(); ++candidate) {
        const auto &entry = *candidate;
        if (entry.start >= to) {
            break;
        }
        if (entry.stop > from && entry.start < to) {
            result.push_back(entry);
            if (limit > 0 && result.size() >= limit) {
                break;
            }
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

    return snapshot->store ? snapshot->store->allEntries() : snapshot->allEntries;
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

    if (snapshot->store) return snapshot->store->maxStop(tvgId);
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
