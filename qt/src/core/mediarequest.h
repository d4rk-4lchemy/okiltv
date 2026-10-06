#pragma once

#include "models.h"
#include <QRegularExpression>
#include <QUrl>

namespace OKILTV::Core {
using MediaRequestHeaders = QList<QPair<QByteArray, QByteArray>>;
inline MediaRequestHeaders mediaRequestHeaders(const QString &playerUserAgent, const QMap<QString, QString> &mpvOptions)
{
    MediaRequestHeaders headers;
    auto appendHeader = [&headers](QByteArray name, QByteArray value) {
        name = name.trimmed();
        value = value.trimmed();
        if (name.isEmpty() || value.isEmpty()) {
            return;
        }
        for (auto &header : headers) {
            if (header.first.compare(name, Qt::CaseInsensitive) == 0) {
                header.second = value;
                return;
            }
        }
        headers.append(qMakePair(std::move(name), std::move(value)));
    };

    const auto trimmedUserAgent = mpvOptions.value(QStringLiteral("user-agent"), playerUserAgent).trimmed();
    appendHeader(QByteArrayLiteral("User-Agent"),
                 (trimmedUserAgent.isEmpty() ? defaultPlayerUserAgent() : trimmedUserAgent).toUtf8());

    const auto rawHeaderFields = mpvOptions.value(QStringLiteral("http-header-fields")).trimmed();
    if (!rawHeaderFields.isEmpty()) {
        const QRegularExpression splitPattern(
            QStringLiteral(",(?=\\s*[!#$%&'*+.^_`|~0-9A-Za-z-]+\\s*:)"));
        const auto headerEntries = rawHeaderFields.split(splitPattern, Qt::SkipEmptyParts);
        for (const auto &entry : headerEntries) {
            const auto separatorIndex = entry.indexOf(u':');
            if (separatorIndex <= 0) {
                continue;
            }
            appendHeader(
                entry.left(separatorIndex).trimmed().toUtf8(),
                entry.mid(separatorIndex + 1).trimmed().toUtf8());
        }
    }

    const auto referrer = mpvOptions.value(QStringLiteral("referrer")).trimmed();
    if (!referrer.isEmpty()) {
        appendHeader(QByteArrayLiteral("Referer"), referrer.toUtf8());
    }

    return headers;
}

// Input options belong before -i (or the positional ffprobe input).
inline QStringList mediaInputOptions(const QString &url, const QMap<QString, QString> &options, const QString &userAgent)
{
    const auto scheme = QUrl(url).scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) return {};
    QStringList arguments;
    QByteArray otherHeaders;
    for (const auto &header : mediaRequestHeaders(userAgent, options)) {
        // Reject control characters rather than injecting another HTTP header.
        if (header.first.contains('\r') || header.first.contains('\n')
            || header.second.contains('\r') || header.second.contains('\n')) continue;
        if (header.first.compare("User-Agent", Qt::CaseInsensitive) == 0)
            arguments << QStringLiteral("-user_agent") << QString::fromUtf8(header.second);
        else otherHeaders += header.first + ": " + header.second + "\r\n";
    }
    if (!otherHeaders.isEmpty()) arguments << QStringLiteral("-headers") << QString::fromUtf8(otherHeaders);
    return arguments;
}
} // namespace OKILTV::Core
