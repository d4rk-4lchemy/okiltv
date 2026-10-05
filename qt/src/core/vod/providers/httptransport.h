#pragma once
#include "../vodmodels.h"

namespace OKILTV::Vod {
enum class RedirectPolicy { Reject, SameOrigin };
struct HttpRequest {
    QUrl url;
    QMap<QByteArray, QByteArray> headers;
    RedirectPolicy redirects = RedirectPolicy::SameOrigin;
};
// Trusted transport result, never emitted by the public facade.
struct HttpResponse {
    int status = 0;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;
    QUrl effectiveUrl;
    qint64 elapsedMs = 0;
};
class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;
    virtual Result<HttpResponse> get(const HttpRequest &, const RequestContext &) const = 0;
};
class QtHttpTransport final : public IHttpTransport {
public:
    // Call from a worker. All Qt network objects are owned by that invocation.
    Result<HttpResponse> get(const HttpRequest &, const RequestContext &) const override;
};
[[nodiscard]] bool httpUrlAllowed(const QUrl &url);
[[nodiscard]] bool sameOrigin(const QUrl &left, const QUrl &right);
[[nodiscard]] std::optional<qint64> retryAfterMs(const QByteArray &value, const QDateTime &now);
}
