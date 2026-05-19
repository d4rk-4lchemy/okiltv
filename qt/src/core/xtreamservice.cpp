#include "xtreamservice.h"
#include "redaction.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUrlQuery>

#include <algorithm>
#include <stdexcept>

namespace OKILTV::Core {

namespace {

QString trimmedBaseUrl(const QString &value)
{
    auto base = value.trimmed();
    while (base.endsWith(u'/')) {
        base.chop(1);
    }
    return base;
}

int jsonInt(const QJsonValue &value)
{
    if (value.isDouble()) {
        return value.toInt();
    }

    return value.toString().toInt();
}

QJsonDocument parseJson(const QByteArray &payload, const QString &context)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError) {
        throw std::runtime_error(
            QStringLiteral("%1 returned invalid JSON: %2")
                .arg(context, error.errorString())
                .toStdString());
    }
    return document;
}

QString boundedJsonSample(const QJsonDocument &document)
{
    auto sample = QString::fromUtf8(document.toJson(QJsonDocument::Compact));
    sample = redactSensitiveText(sample);
    constexpr int kMaxSampleChars = 220;
    if (sample.size() > kMaxSampleChars) {
        sample = sample.left(kMaxSampleChars) + QStringLiteral("...");
    }
    return sample;
}

QString normalizedStreamExtension(const QString &candidate, const QString &fallback = QStringLiteral("ts"))
{
    auto normalized = candidate.trimmed().toLower();
    if (normalized.startsWith(u'.')) {
        normalized.remove(0, 1);
    }

    static const QRegularExpression allowedPattern(QStringLiteral(R"(^[a-z0-9]{1,16}$)"));
    if (!allowedPattern.match(normalized).hasMatch()) {
        normalized.clear();
    }

    return normalized.isEmpty() ? fallback : normalized;
}

QString normalizedTimezone(const QString &candidate)
{
    const auto trimmed = candidate.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    const auto zone = QTimeZone(trimmed.toUtf8());
    if (!zone.isValid()) {
        return {};
    }

    return QString::fromUtf8(zone.id());
}

} // namespace

XtreamService::XtreamService(std::shared_ptr<NetworkAccess> network)
    : m_network(std::move(network))
{
}

void XtreamService::setProfile(const ServerProfile &profile)
{
    if (profile.xtreamBaseUrl.trimmed().isEmpty()
        || profile.xtreamUsername.trimmed().isEmpty()
        || profile.xtreamPassword.trimmed().isEmpty()) {
        throw std::invalid_argument("Xtream profile requires base URL, username, and password");
    }

    m_profile = profile;
}

XtreamService::AuthInfo XtreamService::authenticate() const
{
    ensureProfile();
    const auto document = parseJson(m_network->get(apiUrl(std::nullopt)), QStringLiteral("Authenticate"));
    const auto root = document.object();
    AuthInfo info;
    info.authenticated = jsonInt(root.value(QStringLiteral("user_info")).toObject().value(QStringLiteral("auth"))) == 1;
    info.serverTimezone = normalizedTimezone(root.value(QStringLiteral("server_info")).toObject().value(QStringLiteral("timezone")).toString());
    return info;
}

QList<ChannelCategory> XtreamService::getLiveCategories() const
{
    ensureProfile();
    const auto document =
        parseJson(m_network->get(apiUrl(QStringLiteral("get_live_categories"))), QStringLiteral("Categories"));
    if (!document.isArray()) {
        const auto sample = boundedJsonSample(document);
        throw std::runtime_error(
            QStringLiteral("Categories returned unexpected JSON shape (expected array). sample=%1")
                .arg(sample)
                .toStdString());
    }

    QList<ChannelCategory> categories;
    for (const auto &value : document.array()) {
        const auto object = value.toObject();
        ChannelCategory category;
        category.id = normalizeChannelCategoryId(object.value(QStringLiteral("category_id")).toString());
        category.name = object.value(QStringLiteral("category_name")).toString().trimmed();
        if (category.name.isEmpty()) {
            category.name = displayNameForCategoryId(category.id);
        }
        category.parentId = jsonInt(object.value(QStringLiteral("parent_id")));
        categories.push_back(category);
    }

    return categories;
}

QList<Channel> XtreamService::getLiveStreams(const std::optional<QString> &categoryId) const
{
    const auto &profile = this->profile();

    auto url = apiUrl(QStringLiteral("get_live_streams"));
    if (categoryId.has_value()) {
        QUrlQuery query(url);
        query.addQueryItem(QStringLiteral("category_id"), categoryId.value());
        url.setQuery(query);
    }

    const auto document = parseJson(m_network->get(url), QStringLiteral("Live streams"));
    if (!document.isArray()) {
        const auto sample = boundedJsonSample(document);
        throw std::runtime_error(
            QStringLiteral("Live streams returned unexpected JSON shape (expected array). sample=%1")
                .arg(sample)
                .toStdString());
    }

    QList<Channel> channels;
    channels.reserve(document.array().size());
    for (const auto &value : document.array()) {
        const auto object = value.toObject();
        Channel channel;
        channel.id = jsonInt(object.value(QStringLiteral("stream_id")));
        channel.name = object.value(QStringLiteral("name")).toString();
        channel.tvgId = object.value(QStringLiteral("epg_channel_id")).toString();
        channel.tvgName = channel.name;
        channel.categoryId = normalizeChannelCategoryId(object.value(QStringLiteral("category_id")).toString());
        channel.iconUrl = object.value(QStringLiteral("stream_icon")).toString();
        channel.streamUrl = buildStreamUrl(
            channel.id,
            normalizedStreamExtension(object.value(QStringLiteral("container_extension")).toString()));
        channel.source = ChannelSource::Xtream;
        channel.sortOrder = jsonInt(object.value(QStringLiteral("num")));
        channel.profileId = profile.id;
        channel.catchupSupported = jsonInt(object.value(QStringLiteral("tv_archive"))) == 1;
        channel.catchupWindowHours = std::max(0, jsonInt(object.value(QStringLiteral("tv_archive_duration"))) * 24);
        if (channel.catchupSupported && channel.catchupWindowHours <= 0) {
            channel.catchupSupported = false;
        }
        channels.push_back(channel);
    }

    return channels;
}

QByteArray XtreamService::getXmltvBytes() const
{
    const auto &profile = this->profile();

    QUrl url(QStringLiteral("%1/xmltv.php").arg(trimmedBaseUrl(profile.xtreamBaseUrl)));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("username"), profile.xtreamUsername);
    query.addQueryItem(QStringLiteral("password"), profile.xtreamPassword);
    url.setQuery(query);
    return m_network->get(url);
}

QString XtreamService::buildStreamUrl(const int streamId, const QString &ext) const
{
    const auto &profile = this->profile();
    return QStringLiteral("%1/live/%2/%3/%4.%5")
        .arg(trimmedBaseUrl(profile.xtreamBaseUrl))
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(profile.xtreamUsername)))
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(profile.xtreamPassword)))
        .arg(streamId)
        .arg(ext);
}

QUrl XtreamService::apiUrl(const std::optional<QString> &action) const
{
    const auto &profile = this->profile();
    QUrl url(QStringLiteral("%1/player_api.php").arg(trimmedBaseUrl(profile.xtreamBaseUrl)));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("username"), profile.xtreamUsername);
    query.addQueryItem(QStringLiteral("password"), profile.xtreamPassword);
    if (action.has_value()) {
        query.addQueryItem(QStringLiteral("action"), action.value());
    }
    url.setQuery(query);
    return url;
}

void XtreamService::ensureProfile() const
{
    if (!m_profile.has_value()) {
        throw std::runtime_error("Call setProfile() first.");
    }
}

const ServerProfile &XtreamService::profile() const
{
    if (const auto *profile = m_profile ? &*m_profile : nullptr) {
        return *profile;
    }
    throw std::runtime_error("Call setProfile() first.");
}

} // namespace OKILTV::Core
