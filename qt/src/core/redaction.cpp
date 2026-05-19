#include "redaction.h"

#include <QRegularExpression>
#include <QUrlQuery>

namespace OKILTV::Core {

namespace {

bool shouldMaskQueryKey(const QString &key)
{
    const auto normalized = key.trimmed().toLower();
    return normalized == QStringLiteral("username")
        || normalized == QStringLiteral("password")
        || normalized == QStringLiteral("user")
        || normalized == QStringLiteral("pass")
        || normalized == QStringLiteral("token")
        || normalized == QStringLiteral("access_token")
        || normalized == QStringLiteral("refresh_token")
        || normalized == QStringLiteral("auth")
        || normalized == QStringLiteral("authorization")
        || normalized == QStringLiteral("bearer")
        || normalized == QStringLiteral("api_key")
        || normalized == QStringLiteral("apikey")
        || normalized == QStringLiteral("signature")
        || normalized == QStringLiteral("sig");
}

QString maskXtreamPathSegments(const QString &path)
{
    auto parts = path.split(u'/', Qt::KeepEmptyParts);
    auto firstSegmentIndex = -1;
    for (int index = 0; index < parts.size(); ++index) {
        if (!parts.at(index).isEmpty()) {
            firstSegmentIndex = index;
            break;
        }
    }

    if (firstSegmentIndex < 0) {
        return path;
    }

    const auto rootSegment = parts.at(firstSegmentIndex).toLower();
    if ((rootSegment == QStringLiteral("live")
            || rootSegment == QStringLiteral("movie")
            || rootSegment == QStringLiteral("series"))
        && firstSegmentIndex + 2 < parts.size()) {
        parts[firstSegmentIndex + 1] = QStringLiteral("***");
        parts[firstSegmentIndex + 2] = QStringLiteral("***");
    }
    if (rootSegment == QStringLiteral("timeshift") && firstSegmentIndex + 2 < parts.size()) {
        parts[firstSegmentIndex + 1] = QStringLiteral("***");
        parts[firstSegmentIndex + 2] = QStringLiteral("***");
    }

    return parts.join(u'/');
}

QString redactUrlLikeString(const QString &value)
{
    auto parsed = QUrl(value);
    if (!parsed.isValid() || parsed.scheme().trimmed().isEmpty()) {
        parsed = QUrl::fromUserInput(value);
    }
    if (!parsed.isValid() || parsed.scheme().trimmed().isEmpty()) {
        return value;
    }

    parsed.setPath(maskXtreamPathSegments(parsed.path()));

    QUrlQuery query(parsed);
    if (!query.isEmpty()) {
        QUrlQuery sanitizedQuery;
        const auto items = query.queryItems(QUrl::FullyDecoded);
        for (const auto &item : items) {
            if (shouldMaskQueryKey(item.first)) {
                sanitizedQuery.addQueryItem(item.first, QStringLiteral("***"));
            } else {
                sanitizedQuery.addQueryItem(item.first, item.second);
            }
        }
        parsed.setQuery(sanitizedQuery);
    }

    return parsed.toString(QUrl::FullyEncoded);
}

} // namespace

QString redactSensitiveUrl(const QString &rawUrl)
{
    return redactUrlLikeString(rawUrl.trimmed());
}

QString redactSensitiveText(const QString &text)
{
    auto redacted = text;
    redacted.replace(
        QRegularExpression(
            QStringLiteral(
                "(?i)\\b(username|password|user|pass|token|access_token|refresh_token|auth|authorization|bearer|api_key|apikey|signature|sig)=([^&\\s]+)")),
        QStringLiteral("\\1=***"));
    redacted.replace(
        QRegularExpression(QStringLiteral("(?i)\\b(authorization)\\s*[:=]\\s*([^\\r\\n]+)")),
        QStringLiteral("\\1=***"));
    redacted.replace(
        QRegularExpression(QStringLiteral("(?i)\\b(bearer)\\s+([A-Za-z0-9._~+/=-]+)")),
        QStringLiteral("\\1 ***"));
    redacted.replace(
        QRegularExpression(QStringLiteral("/(live|movie|series|timeshift)/[^/\\s]+/[^/\\s]+/")),
        QStringLiteral("/\\1/***/***/"));

    const QRegularExpression urlPattern(QStringLiteral(R"(([A-Za-z][A-Za-z0-9+.-]*://[^\s"']+))"));
    auto match = urlPattern.match(redacted);
    while (match.hasMatch()) {
        const auto originalUrl = match.captured(1);
        const auto sanitizedUrl = redactUrlLikeString(originalUrl);
        redacted.replace(match.capturedStart(1), match.capturedLength(1), sanitizedUrl);
        match = urlPattern.match(redacted, match.capturedStart(1) + sanitizedUrl.size());
    }

    return redacted;
}

QString networkCategoryForUrl(const QUrl &url)
{
    const auto path = url.path().toLower();
    if (path.endsWith(QStringLiteral("/player_api.php"))) {
        const auto action = QUrlQuery(url).queryItemValue(QStringLiteral("action")).toLower();
        if (!action.isEmpty()) {
            return QStringLiteral("xtream.api.%1").arg(action);
        }
        return QStringLiteral("xtream.api.authenticate");
    }
    if (path.endsWith(QStringLiteral("/xmltv.php"))) {
        return QStringLiteral("xtream.xmltv");
    }
    if (path.endsWith(QStringLiteral(".m3u")) || path.endsWith(QStringLiteral(".m3u8"))) {
        return QStringLiteral("playlist");
    }
    if (path.endsWith(QStringLiteral(".xml")) || path.endsWith(QStringLiteral(".xml.gz"))) {
        return QStringLiteral("epg.xmltv");
    }
    return QStringLiteral("network.http");
}

} // namespace OKILTV::Core
