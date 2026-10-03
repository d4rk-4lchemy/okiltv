#include "httptransport.h"
#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QLocale>
#include <QTimeZone>
#include <algorithm>
#include <array>
#include <limits>
#if defined(OKILTV_USE_QT_ZLIB)
#include <QtZlib/zlib.h>
#else
#include <zlib.h>
#endif

namespace OKILTV::Vod {
bool httpUrlAllowed(const QUrl &url)
{
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty()
        && !url.hasFragment() && (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http"));
}
bool sameOrigin(const QUrl &left, const QUrl &right)
{
    const auto port = [](const QUrl &url) { return url.port(url.scheme() == QStringLiteral("https") ? 443 : 80); };
    return left.scheme() == right.scheme() && left.host() == right.host() && port(left) == port(right);
}
std::optional<qint64> retryAfterMs(const QByteArray &value, const QDateTime &now)
{
    const auto trimmed = value.trimmed();
    if (trimmed.isEmpty()) return {};
    if (std::all_of(trimmed.cbegin(), trimmed.cend(), [](char c) { return c >= '0' && c <= '9'; })) {
        bool ok = false;
        const auto seconds = trimmed.toLongLong(&ok);
        if (ok && seconds <= std::numeric_limits<qint64>::max() / 1000) return seconds * 1000;
        return {};
    }
    auto date = QLocale::c().toDateTime(QString::fromLatin1(trimmed), QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
    date.setTimeZone(QTimeZone::UTC);
    if (!date.isValid()) return {};
    return std::max(qint64(0), now.msecsTo(date));
}
Result<HttpResponse> QtHttpTransport::get(const HttpRequest &input, const RequestContext &context) const
{
    if (const auto error = context.interruption()) return *error;
    if (!httpUrlAllowed(input.url) || context.responseByteLimit <= 0
        || context.responseByteLimit > qint64(64) * 1024 * 1024)
        return Error{ErrorCode::InvalidResponse, context.operationId};
    // These are application-approved headers, not arbitrary provider fields.
    for (auto it = input.headers.cbegin(); it != input.headers.cend(); ++it) {
        const auto name = it.key().toLower();
        if ((name != "user-agent" && name != "accept" && name != "authorization")
            || std::any_of(it.value().cbegin(), it.value().cend(), [](unsigned char c) { return c < 32 || c == 127; }))
            return Error{ErrorCode::InvalidResponse, context.operationId};
    }
    QElapsedTimer elapsed;
    elapsed.start();
    QNetworkAccessManager manager;
    auto url = input.url;
    for (int redirect = 0; redirect <= 5; ++redirect) {
        if (const auto error = context.interruption()) return *error;
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
        request.setTransferTimeout(30000);
        // Explicit encoding disables Qt's automatic decompression. Inflate in
        // bounded chunks below, with a limit on decoded bytes, including when a
        // server sends compressed data despite the identity preference.
        request.setRawHeader("Accept-Encoding", "identity");
        for (auto it = input.headers.cbegin(); it != input.headers.cend(); ++it) request.setRawHeader(it.key(), it.value());
        std::unique_ptr<QNetworkReply> reply(manager.get(request));
        reply->setReadBufferSize(qint64(64) * 1024);
        HttpResponse response;
        std::optional<Error> failure;
        QEventLoop loop;
        QTimer interrupt;
        interrupt.setInterval(10);
        struct Inflater {
            z_stream stream{};
            bool initialized = false;
            bool ended = false;
            ~Inflater() { if (initialized) inflateEnd(&stream); }
        } inflater;
        qint64 received = 0;
        const auto append = [&](const char *data, qint64 size) {
            if (size > context.responseByteLimit - response.body.size()) {
                failure = Error{ErrorCode::ResponseTooLarge, context.operationId};
                return;
            }
            response.body.append(data, static_cast<qsizetype>(size));
        };
        const auto drain = [&]() {
            if (failure) return;
            while (reply->bytesAvailable() > 0) {
                auto chunk = reply->read(qint64(64) * 1024);
                if (chunk.isEmpty()) break;
                received += chunk.size();
                if (received > context.responseByteLimit) {
                    failure = Error{ErrorCode::ResponseTooLarge, context.operationId};
                    reply->abort();
                    return;
                }
                const auto encoding = reply->rawHeader("Content-Encoding").trimmed().toLower();
                if (encoding.isEmpty() || encoding == "identity") append(chunk.constData(), chunk.size());
                else if (encoding == "gzip" || encoding == "deflate") {
                    if (!inflater.initialized) {
                        inflater.initialized = inflateInit2(&inflater.stream, MAX_WBITS + 32) == Z_OK;
                        if (!inflater.initialized) failure = Error{ErrorCode::InvalidResponse, context.operationId};
                    }
                    inflater.stream.next_in = reinterpret_cast<Bytef *>(chunk.data());
                    inflater.stream.avail_in = static_cast<uInt>(chunk.size());
                    std::array<char, std::size_t(64) * 1024> output{};
                    if (inflater.ended) failure = Error{ErrorCode::InvalidResponse, context.operationId};
                    while (!failure && !inflater.ended && (inflater.stream.avail_in > 0 || inflater.stream.avail_out == 0)) {
                        inflater.stream.next_out = reinterpret_cast<Bytef *>(output.data());
                        inflater.stream.avail_out = static_cast<uInt>(output.size());
                        const auto status = inflate(&inflater.stream, Z_NO_FLUSH);
                        append(output.data(), static_cast<qint64>(output.size() - inflater.stream.avail_out));
                        if (status == Z_STREAM_END) inflater.ended = true;
                        else if (status != Z_OK) failure = Error{ErrorCode::InvalidResponse, context.operationId};
                        if (const auto error = context.interruption()) failure = error;
                    }
                    if (inflater.ended && inflater.stream.avail_in > 0) failure = Error{ErrorCode::InvalidResponse, context.operationId};
                } else failure = Error{ErrorCode::InvalidResponse, context.operationId};
                if (failure) { reply->abort(); return; }
            }
        };
        QObject::connect(reply.get(), &QNetworkReply::readyRead, &loop, drain);
        QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&interrupt, &QTimer::timeout, &loop, [&]() {
            if (const auto error = context.interruption()) { failure = error; reply->abort(); loop.quit(); }
        });
        interrupt.start();
        if (!reply->isFinished()) loop.exec();
        interrupt.stop();
        drain();
        if (failure) return *failure;
        if (inflater.initialized && !inflater.ended) return Error{ErrorCode::InvalidResponse, context.operationId};
        if (const auto error = context.interruption()) return *error;
        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.effectiveUrl = reply->url();
        response.elapsedMs = elapsed.elapsed();
        for (const auto &header : reply->rawHeaderPairs()) response.headers.insert(header.first.toLower(), header.second);
        const auto target = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        if (response.status >= 300 && response.status < 400 && !target.isEmpty()) {
            const auto next = url.resolved(target);
            if (input.redirects == RedirectPolicy::Reject || redirect == 5 || !httpUrlAllowed(next)
                || !sameOrigin(input.url, next)) return Error{ErrorCode::InvalidResponse, context.operationId};
            url = next;
            continue;
        }
        if (response.status == 0 || (reply->error() != QNetworkReply::NoError && response.status < 400))
            return Error{ErrorCode::ProviderUnavailable, context.operationId, true};
        return response;
    }
    return Error{ErrorCode::InvalidResponse, context.operationId};
}
}
