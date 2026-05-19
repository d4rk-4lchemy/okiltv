#include "m3uservice.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QUrl>

#include <algorithm>
#include <optional>
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
    if (!ok || days <= 0.0) {
        return std::nullopt;
    }

    return std::max(1, qRound(days * 24.0));
}

} // namespace

M3UService::M3UService(std::shared_ptr<NetworkAccess> network)
    : m_network(std::move(network))
{
}

QList<Channel> M3UService::loadFromUrl(const QUrl &url, const QUuid &profileId) const
{
    return parse(m_network->get(url), profileId);
}

QList<Channel> M3UService::loadFromFile(const QString &path, const QUuid &profileId) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(QStringLiteral("Unable to open %1").arg(path).toStdString());
    }

    return parse(file.readAll(), profileId);
}

QList<Channel> M3UService::parse(const QByteArray &data, const QUuid &profileId) const
{
    QList<Channel> channels;
    QString extinf;
    auto index = 0;

    const auto lines = QString::fromUtf8(data).split(u'\n');
    for (const auto &rawLine : lines) {
        const auto line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        if (line.startsWith(QStringLiteral("#EXTINF:"), Qt::CaseInsensitive)) {
            extinf = line;
            continue;
        }

        if (!line.startsWith(u'#') && !extinf.isEmpty()) {
            channels.push_back(parseEntry(extinf, line, index++, profileId));
            extinf.clear();
        }
    }

    return channels;
}

Channel M3UService::parseEntry(
    const QString &extinf, // NOLINT(bugprone-easily-swappable-parameters)
    const QString &url,
    const int index,
    const QUuid &profileId) const
{
    const auto attributes = parseExtinfAttributes(extinf);
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
    channel.id        = index;        // 0-based; M3U IDs are positional indices
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
    const auto catchupWindowHours = catchupDays.has_value() ? catchupDays : legacyTimeshiftDays;
    const auto resolvedCatchupWindowHours = catchupWindowHours.value_or(0);
    const auto resolvedLegacyWindowHours = legacyTimeshiftDays.value_or(0);
    if ((catchupMode == QStringLiteral("default") || catchupMode == QStringLiteral("append"))
        && !catchupSource.isEmpty()
        && catchupWindowHours.has_value()) {
        channel.catchupSupported = true;
        channel.catchupWindowHours = resolvedCatchupWindowHours;
        channel.catchupMode = catchupMode;
        channel.catchupSourceTemplate = catchupSource;
    } else if (legacyTimeshiftDays.has_value()) {
        channel.catchupSupported = true;
        channel.catchupWindowHours = resolvedLegacyWindowHours;
        channel.catchupMode = QStringLiteral("append");
        channel.catchupSourceTemplate = QStringLiteral("utc={utc}&lutc={lutc}");
    }

    return channel;
}

} // namespace OKILTV::Core
