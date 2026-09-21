#include "redaction.h"

#include <QJsonArray>
#include <QJsonObject>
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
        || normalized == QStringLiteral("sig")
        || normalized == QStringLiteral("xtreamusername")
        || normalized == QStringLiteral("xtreampassword")
        || normalized == QStringLiteral("secret")
        || normalized == QStringLiteral("cookie")
        || normalized == QStringLiteral("set-cookie");
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

    if (!parsed.userInfo().isEmpty()) {
        parsed.setUserInfo(QStringLiteral("***"));
    }
    const auto maskedPath = maskXtreamPathSegments(parsed.path());
    const auto scheme = parsed.scheme().toLower();
    const bool networkUrl = scheme == QStringLiteral("http") || scheme == QStringLiteral("https")
        || scheme == QStringLiteral("rtsp") || scheme == QStringLiteral("rtmp");
    // Unknown provider layouts may put tokens anywhere in the path.
    parsed.setPath(networkUrl && maskedPath == parsed.path() && !parsed.path().isEmpty()
        ? QStringLiteral("/***") : maskedPath);
    if (networkUrl && !parsed.fragment().isEmpty()) parsed.setFragment(QStringLiteral("***"));

    QUrlQuery query(parsed);
    if (!query.isEmpty()) {
        QUrlQuery sanitizedQuery;
        const auto items = query.queryItems(QUrl::FullyDecoded);
        for (const auto &item : items) {
            if (networkUrl || shouldMaskQueryKey(item.first)) {
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
    // Match JSON string values including escaped quotes, also inside log prefixes.
    redacted.replace(
        QRegularExpression(QStringLiteral(
            R"re(("(?:username|password|xtreamUsername|xtreamPassword|user|pass|token|access_token|refresh_token|auth|authorization|api_key|apikey|secret|cookie|set-cookie)"\s*:\s*)"(?:[^"\\]|\\.)*")re"),
            QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("\\1\"***\""));
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

QJsonValue redactSensitiveJson(const QJsonValue &value)
{
    if (value.isObject()) {
        auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            it.value() = shouldMaskQueryKey(it.key())
                ? QJsonValue(QStringLiteral("***")) : redactSensitiveJson(it.value());
        }
        return object;
    }
    if (value.isArray()) {
        QJsonArray result;
        for (const auto &entry : value.toArray()) {
            result.append(redactSensitiveJson(entry));
        }
        return result;
    }
    return value.isString() ? QJsonValue(redactSensitiveText(value.toString())) : value;
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
