#include "m3uservice.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QUrl>

#include <algorithm>
#include <optional>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace OKILTV::Core {

namespace {

qsizetype extinfMetadataSeparator(const QString &extinf)
{
    QChar quote;
    auto inQuote = false;
    for (qsizetype i = 0; i < extinf.size(); ++i) {
        const auto ch = extinf.at(i);
        if (inQuote) {
            if (ch == u'\\' && i + 1 < extinf.size()) {
                ++i;
                continue;
            }
            if (ch == quote) {
                inQuote = false;
            }
            continue;
        }
        if (ch == u'"' || ch == u'\'') {
            inQuote = true;
            quote = ch;
            continue;
        }
        if (ch == u',') {
            return i;
        }
    }
    return -1;
}

QString fallbackNameFromUrl(const QString &url)
{
    const QUrl parsed(url);
    auto fromUrl = QFileInfo(parsed.path()).completeBaseName();
    if (!fromUrl.isEmpty()) {
        return fromUrl;
    }

    return QFileInfo(url).completeBaseName();
}

QHash<QString, QString> parseExtinfAttributes(const QString &extinf)
{
    const auto metadataStart = extinf.indexOf(u':');
    if (metadataStart < 0 || metadataStart + 1 >= extinf.size()) {
        return {};
    }

    const auto separator = extinfMetadataSeparator(extinf);
    const auto metadataEnd = separator >= 0 ? separator : extinf.size();
    const auto metadata = extinf.mid(metadataStart + 1, metadataEnd - metadataStart - 1);

    QHash<QString, QString> attributes;
    qsizetype index = 0;
    while (index < metadata.size()) {
        while (index < metadata.size() && metadata.at(index).isSpace()) {
            ++index;
        }
        const auto keyStart = index;
        while (index < metadata.size()) {
            const auto ch = metadata.at(index);
            if (ch.isLetterOrNumber() || ch == u'-' || ch == u'_' || ch == u':') {
                ++index;
                continue;
            }
            break;
        }
        if (index == keyStart) {
            ++index;
            continue;
        }

        const auto key = metadata.mid(keyStart, index - keyStart).trimmed().toLower();
        while (index < metadata.size() && metadata.at(index).isSpace()) {
            ++index;
        }
        if (index >= metadata.size() || metadata.at(index) != u'=') {
            continue;
        }
        ++index;
        while (index < metadata.size() && metadata.at(index).isSpace()) {
            ++index;
        }

        QString value;
        if (index < metadata.size() && (metadata.at(index) == u'"' || metadata.at(index) == u'\'')) {
            const auto quote = metadata.at(index);
            ++index;
            while (index < metadata.size()) {
                const auto ch = metadata.at(index);
                if (ch == u'\\' && index + 1 < metadata.size()) {
                    value.append(metadata.at(index + 1));
                    index += 2;
                    continue;
                }
                if (ch == quote) {
                    ++index;
                    break;
                }
                value.append(ch);
                ++index;
            }
        } else {
            const auto valueStart = index;
            while (index < metadata.size() && !metadata.at(index).isSpace()) {
                ++index;
            }
            value = metadata.mid(valueStart, index - valueStart);
        }

        attributes.insert(key, value.trimmed());
    }

    return attributes;
}

QString attributeValue(const QHash<QString, QString> &attributes, const QString &attributeName)
{
    return attributes.value(attributeName.trimmed().toLower()).trimmed();
}

std::optional<int> archiveWindowHoursFromDaysAttribute(const QString &value)
{
    bool ok = false;
    const auto days = value.trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(days) || days <= 0.0
        || days > static_cast<double>(std::numeric_limits<int>::max()) / 24.0) {
        return std::nullopt;
    }

    return std::max(1, qRound(days * 24.0));
}

} // namespace

M3UService::M3UService(std::shared_ptr<NetworkAccess> network)
    : m_network(std::move(network))
{
}

void M3UService::retainChannelIds(QList<Channel> &channels, const QList<Channel> &previous, qint64 &nextId)
{
    QHash<QString, QList<int>> idsByUrl;
    nextId = std::max<qint64>(0, nextId);
    for (const auto &channel : previous) {
        idsByUrl[channel.streamUrl].push_back(channel.id);
        nextId = std::max(nextId, static_cast<qint64>(channel.id) + 1);
    }
    for (auto &channel : channels) {
        auto it = idsByUrl.find(channel.streamUrl);
        if (it != idsByUrl.end() && !it->isEmpty()) {
            channel.id = it->takeFirst();
        } else {
            if (nextId > std::numeric_limits<int>::max())
                throw std::runtime_error("The playlist has exhausted its channel identifiers.");
            channel.id = static_cast<int>(nextId++);
        }
    }
}

QList<Channel> M3UService::loadFromUrl(const QUrl &url, const QUuid &profileId, QStringList *epgUrls) const
{
    return parse(m_network->get(url), profileId, url, epgUrls);
}

QList<Channel> M3UService::loadFromFile(const QString &path, const QUuid &profileId, QStringList *epgUrls) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(QStringLiteral("Unable to open %1").arg(path).toStdString());
    }

    return parse(file.readAll(), profileId, QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()), epgUrls);
}

QList<Channel> M3UService::parse(const QByteArray &data, const QUuid &profileId, const QUrl &baseUrl, QStringList *epgUrls) const
{
    QList<Channel> channels;
    QString extinf;
    QHash<QString, QString> defaults;
    QString groupTitle;
    if (epgUrls)
        epgUrls->clear();
    auto index = 0;
    bool sawHeader = false;

    auto text = QString::fromUtf8(data);
    if (text.startsWith(QChar::ByteOrderMark)) {
        text.remove(0, 1);
    }
    const auto lines = text.split(u'\n');
    // A transport manifest is one playable channel, never a list of TS segments.
    const bool hls = std::any_of(lines.cbegin(), lines.cend(), [](const QString &line) {
        const auto tag = line.trimmed();
        return tag.startsWith(QStringLiteral("#EXT-X-TARGETDURATION:"))
            || tag.startsWith(QStringLiteral("#EXT-X-STREAM-INF:"));
    });
    if (hls) {
        if (baseUrl.isEmpty())
            throw std::runtime_error("An HLS source requires its playlist URL or file path.");
        return {parseEntry(QStringLiteral("#EXTINF:-1,"), baseUrl.toString(), 0, profileId, {})};
    }
    const auto resolveReference = [&baseUrl](const QString &value) {
        const QUrl reference(value);
        return value.isEmpty() || baseUrl.isEmpty() || !reference.isRelative()
            ? value : baseUrl.resolved(reference).toString();
    };
    for (const auto &rawLine : lines) {
        const auto line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        if (line.compare(QStringLiteral("#EXTM3U"), Qt::CaseInsensitive) == 0
            || (line.startsWith(QStringLiteral("#EXTM3U"), Qt::CaseInsensitive)
                && line.size() > 7 && line.at(7).isSpace())) {
            sawHeader = true;
            defaults = parseExtinfAttributes(QStringLiteral("#EXTM3U:") + line.mid(7));
            if (epgUrls) {
                for (const auto &key : {QStringLiteral("url-tvg"), QStringLiteral("x-tvg-url")}) {
                    const auto urls = defaults.value(key).split(u',', Qt::SkipEmptyParts);
                    for (const auto &value : urls) {
                        const auto resolved = resolveReference(value.trimmed());
                        if (!resolved.isEmpty() && !epgUrls->contains(resolved))
                            epgUrls->push_back(resolved);
                    }
                }
            }
            continue;
        }

        if (line.startsWith(QStringLiteral("#EXTINF:"), Qt::CaseInsensitive)) {
            if (!extinf.isEmpty()) {
                throw std::runtime_error("M3U playlist contains an entry without a stream URL.");
            }
            extinf = line;
            continue;
        }

        if (line.startsWith(QStringLiteral("#EXTGRP:"), Qt::CaseInsensitive)) {
            groupTitle = line.mid(8).trimmed();
            continue;
        }
        if (!line.startsWith(u'#')) {
            if (extinf.isEmpty()) {
                throw std::runtime_error("Invalid M3U playlist: expected channel metadata before a stream URL.");
            }
            auto entryDefaults = defaults;
            if (!groupTitle.isEmpty())
                entryDefaults.insert(QStringLiteral("group-title"), groupTitle);
            auto channel = parseEntry(extinf, resolveReference(line), index++, profileId, entryDefaults);
            channel.iconUrl = resolveReference(channel.iconUrl);
            if (channel.catchupMode == QStringLiteral("default"))
                channel.catchupSourceTemplate = resolveReference(channel.catchupSourceTemplate);
            channels.push_back(channel);
            groupTitle.clear();
            extinf.clear();
        }
    }

    if (!extinf.isEmpty()) {
        throw std::runtime_error("M3U playlist is incomplete: the last entry has no stream URL.");
    }
    if (!sawHeader && channels.isEmpty()) {
        throw std::runtime_error("M3U playlist is empty or invalid.");
    }

    return channels;
}

Channel M3UService::parseEntry(
    const QString &extinf, // NOLINT(bugprone-easily-swappable-parameters)
    const QString &url,
    const int index,
    const QUuid &profileId,
    const QHash<QString, QString> &defaults) const
{
    auto attributes = defaults;
    const auto local = parseExtinfAttributes(extinf);
    for (auto it = local.cbegin(); it != local.cend(); ++it)
        attributes.insert(it.key(), it.value());
    auto tvgId = attributeValue(attributes, QStringLiteral("tvg-id"));
    auto tvgName = attributeValue(attributes, QStringLiteral("tvg-name"));
    auto logo = attributeValue(attributes, QStringLiteral("tvg-logo")).trimmed();
    const auto rawGroupTitle = attributeValue(attributes, QStringLiteral("group-title")).trimmed();
    auto group = normalizeChannelCategoryId(rawGroupTitle);
    const auto separator = extinfMetadataSeparator(extinf);
    auto displayName = separator >= 0 ? extinf.mid(separator + 1).trimmed() : QString {};

    if (displayName.isEmpty()) {
        displayName = !tvgName.trimmed().isEmpty() ? tvgName.trimmed() : fallbackNameFromUrl(url);
    }

    Channel channel;
    channel.id = index; // Initial import ID; refresh reconciles IDs by URL before persistence.
    channel.name = displayName;
    channel.streamUrl = url;
    channel.categoryId = group;
    channel.categoryName = rawGroupTitle.isEmpty() ? displayNameForCategoryId(group) : rawGroupTitle;
    channel.tvgId = tvgId.trimmed();
    channel.tvgName = tvgName.trimmed().isEmpty() ? displayName : tvgName.trimmed();
    channel.iconUrl = logo;
    channel.source = ChannelSource::M3U;
    channel.sortOrder = index + 1;    // 1-based display order
    channel.profileId = profileId;

    const auto catchupMode = attributeValue(attributes, QStringLiteral("catchup")).toLower();
    const auto catchupDays = archiveWindowHoursFromDaysAttribute(attributeValue(attributes, QStringLiteral("catchup-days")));
    const auto legacyTimeshiftDays = archiveWindowHoursFromDaysAttribute(attributeValue(attributes, QStringLiteral("timeshift")));
    const auto catchupSource = attributeValue(attributes, QStringLiteral("catchup-source"));
    const auto catchupWindowHours = attributes.contains(QStringLiteral("catchup-days")) ? catchupDays : legacyTimeshiftDays;
    const auto resolvedCatchupWindowHours = catchupWindowHours.value_or(0);
    const auto resolvedLegacyWindowHours = legacyTimeshiftDays.value_or(0);
    if ((catchupMode == QStringLiteral("default") || catchupMode == QStringLiteral("append"))
        && !catchupSource.isEmpty()
        && catchupWindowHours.has_value()) {
        channel.catchupSupported = true;
        channel.catchupWindowHours = resolvedCatchupWindowHours;
        channel.catchupMode = catchupMode;
        channel.catchupSourceTemplate = catchupSource;
    } else if ((catchupMode == QStringLiteral("shift") && catchupWindowHours.has_value())
               || (catchupMode.isEmpty() && !attributes.contains(QStringLiteral("catchup-days")) && legacyTimeshiftDays.has_value())) {
        channel.catchupSupported = true;
        channel.catchupWindowHours = catchupMode.isEmpty() ? resolvedLegacyWindowHours : resolvedCatchupWindowHours;
        channel.catchupMode = QStringLiteral("append");
        channel.catchupSourceTemplate = QStringLiteral("utc={utc}&lutc={lutc}");
    }

    return channel;
}

} // namespace OKILTV::Core
