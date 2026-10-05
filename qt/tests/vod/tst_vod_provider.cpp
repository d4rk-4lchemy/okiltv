#include "core/vod/providers/xtreamvodprovider.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <QtTest>

using namespace OKILTV::Vod;
namespace {
class Server : public QTcpServer {
public:
    QByteArray response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n[]";
    int requests = 0;
    bool stall = false;
    Server()
    {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto request = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, request]() {
                    request->append(socket->readAll());
                    if (!request->contains("\r\n\r\n")) return;
                    ++requests;
                    if (stall) return;
                    socket->write(response);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        listen(QHostAddress::LocalHost);
    }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/catalog").arg(serverPort())); }
};
class Transport : public IHttpTransport {
public:
    mutable QList<QUrl> urls;
    QMap<QString, QByteArray> bodies;
    int status = 200;
    Result<HttpResponse> get(const HttpRequest &request, const RequestContext &) const override
    {
        urls.append(request.url);
        const auto action = QUrlQuery(request.url).queryItemValue(QStringLiteral("action"));
        return HttpResponse{status, {}, bodies.value(action, "[]"), request.url, 0};
    }
};
SourceContext source()
{
    return {{QUuid::createUuid(), QUuid::createUuid(), 1}, QStringLiteral("xtream"), true,
        QUrl(QStringLiteral("https://fixture.invalid/base")), QStringLiteral("synthetic+user"), QStringLiteral("synthetic/p%ass")};
}
RequestContext requestFor(const SourceContext &source)
{ RequestContext request; request.source = source.revision; return request; }
ContentRef movieFor(const SourceContext &source)
{ return {source.revision.profileId, source.revision.catalogNamespace, ContentKind::Movie, QStringLiteral("007"), {}}; }
CatalogScope scopeFor(const SourceContext &source)
{ return {source.revision.profileId, source.revision.catalogNamespace, CatalogKind::Movies, {}}; }
}
class VodProviderTests : public QObject {
    Q_OBJECT
private slots:
    void seriesCatalogIsLazyAndEpisodesHaveIndependentIdentity()
    {
        const auto src = source();
        auto transport = std::make_shared<Transport>();
        transport->bodies[QStringLiteral("get_series")] = R"([{"series_id":"007","name":"Series","cover":"https://fixture.invalid/series.jpg","category_id":"2"}])";
        transport->bodies[QStringLiteral("get_series_info")] = R"({"info":{"name":"Series","plot":"Overview","cover":"https://fixture.invalid/series.jpg"},"seasons":[{"season_number":10},{"season_number":2},{"season_number":0}],"episodes":{"10":[{"id":"010","season":10,"episode_num":1,"title":"Tenth season","container_extension":"ts"}],"2":[{"id":902,"season":2,"episode_num":2,"title":"Second","container_extension":"mkv"},{"id":"0901","season":2,"episode_num":1,"title":"First","container_extension":"mp4","info":{"duration_secs":60,"plot":"Episode description","movie_image":"https://fixture.invalid/episode.jpg"}}],"0":[{"id":"special","season":0,"title":"Special","container_extension":"mkv"}]}})";
        XtreamVodProvider provider(transport, true);
        QList<QUrl> artworkUrls;
        provider.registerArtwork=[&](const SourceContext &,const QUrl &url,const RequestContext &) -> std::optional<ArtworkRef> {
            if (url.isEmpty()) return {};
            artworkUrls.append(url);return ArtworkRef{url.fileName(),QStringLiteral("poster"),{}};
        };
        auto scope = scopeFor(src); scope.kind = CatalogKind::Series;
        const auto catalog = provider.fetchCatalog(src, scope, {}, requestFor(src));
        QVERIFY(std::holds_alternative<CatalogBatch>(catalog));
        QCOMPARE(transport->urls.size(), 1);
        const auto series = std::get<SeriesSummary>(std::get<CatalogBatch>(catalog).items.first()).ref;
        QCOMPARE(series.providerItemId, QStringLiteral("007"));
        QCOMPARE(std::get<SeriesSummary>(std::get<CatalogBatch>(catalog).items.first()).artwork.first().id,QStringLiteral("series.jpg"));
        QVERIFY(series.key() != movieFor(src).key());
        auto detail = provider.fetchDetails(src, series, requestFor(src));
        QVERIFY(std::holds_alternative<VodDetails>(detail));
        const auto details = std::get<VodDetails>(detail);
        QCOMPARE(details.title,QStringLiteral("Series"));
        QCOMPARE(details.artwork.first().id,QStringLiteral("series.jpg"));
        QCOMPARE(details.episodes[1].durationMs,std::optional<qint64>(60000));
        QCOMPARE(details.episodes[1].description,QStringLiteral("Episode description"));
        QCOMPARE(details.episodes[1].artwork.first().id,QStringLiteral("episode.jpg"));
        QCOMPARE(artworkUrls.size(),3);
        QCOMPARE(details.seasons.size(), 3);
        QCOMPARE(details.seasons[0].number, std::optional<int>(0));
        QCOMPARE(details.seasons[1].number, std::optional<int>(2));
        QCOMPARE(details.seasons[2].number, std::optional<int>(10));
        QVERIFY(!details.episodes[0].number);
        QCOMPARE(details.episodes[1].ref.providerItemId, QStringLiteral("0901"));
        const auto episode = details.episodes[1].ref;
        QCOMPARE(episode.kind, ContentKind::Episode);
        QCOMPARE(episode.parentNamespace, std::optional<QString>(series.providerItemId));
        const auto resolved = provider.resolvePlayback(src, episode, {}, requestFor(src));
        QVERIFY(std::holds_alternative<PlaybackDescriptor>(resolved));
        const auto uri = std::get<PlaybackDescriptor>(resolved).mediaUri;
        QVERIFY(uri.path().contains(QStringLiteral("/series/")));
        QVERIFY(uri.path().endsWith(QStringLiteral("/0901.mp4")));
        const auto calls = transport->urls.size();
        QVERIFY(std::holds_alternative<Error>(provider.resolvePlayback(src, series, {}, requestFor(src))));
        QCOMPARE(transport->urls.size(), calls); // no implicit first episode/autoplay
        auto missing = episode; missing.providerItemId = QStringLiteral("missing");
        QCOMPARE(std::get<Error>(provider.resolvePlayback(src, missing, {}, requestFor(src))).code, ErrorCode::ContentUnavailable);
        XtreamVodProvider disabled(transport);
        QVERIFY(std::holds_alternative<Error>(disabled.fetchCatalog(src, scope, {}, requestFor(src))));
    }
    void malformedSeriesCannotPublishPartialEpisodes()
    {
        const auto src = source();
        const ContentRef series{src.revision.profileId, src.revision.catalogNamespace, ContentKind::Series, QStringLiteral("series"), {}};
        auto transport = std::make_shared<Transport>();
        XtreamVodProvider provider(transport, true);
        for (const auto &body : {
            QByteArray(R"({"info":{},"episodes":{"1":[{"id":"x","title":"one"},{"id":"x","title":"duplicate"}]}})"),
            QByteArray(R"({"info":{},"episodes":{"1":[{"id":"x","title":"one","season":2}]}})"),
            QByteArray(R"({"info":{},"episodes":{"1":{}}})"),
            QByteArray(R"({"info":{}})")}) {
            transport->bodies[QStringLiteral("get_series_info")] = body;
            QVERIFY(std::holds_alternative<Error>(provider.fetchDetails(src, series, requestFor(src))));
        }
        transport->bodies[QStringLiteral("get_series_info")] = R"({"info":{},"episodes":[]})";
        auto empty = provider.fetchDetails(src, series, requestFor(src));
        QVERIFY(std::holds_alternative<VodDetails>(empty));
        QVERIFY(std::get<VodDetails>(empty).episodes.isEmpty());
    }
    void httpStatusHeadersAndNoAuthenticationRetry()
    {
        Server server;
        server.response = "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 13\r\nContent-Length: 2\r\nConnection: close\r\n\r\n[]";
        QtHttpTransport transport;
        const auto result = transport.get({server.url()}, {});
        QVERIFY(std::holds_alternative<HttpResponse>(result));
        const auto response = std::get<HttpResponse>(result);
        QCOMPARE(response.status, 429);
        QCOMPARE(response.headers.value("retry-after"), QByteArray("13"));
        QCOMPARE(retryAfterMs(response.headers.value("retry-after"), QDateTime::currentDateTimeUtc()), std::optional<qint64>(13000));
        QCOMPARE(response.body, QByteArray("[]"));
        QCOMPARE(server.requests, 1);
    }
    void retryAfterDateAndInvalidValues()
    {
        const auto now = QDateTime::fromString(QStringLiteral("2026-09-27T12:00:00Z"), Qt::ISODate);
        QCOMPARE(retryAfterMs("Sun, 27 Sep 2026 12:00:03 GMT", now), std::optional<qint64>(3000));
        QVERIFY(!retryAfterMs("-1", now));
        QVERIFY(!retryAfterMs("99999999999999999999999", now));
        QVERIFY(!retryAfterMs("invalid", now));
    }
    void crossOriginRedirectNeverReachesDestination()
    {
        Server origin, destination;
        origin.response = "HTTP/1.1 302 Found\r\nLocation: " + destination.url().toEncoded()
            + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        QtHttpTransport transport;
        const auto result = transport.get({origin.url(), {{"Authorization", "Bearer synthetic"}}}, {});
        QVERIFY(std::holds_alternative<Error>(result));
        QCOMPARE(destination.requests, 0);
        QCOMPARE(origin.requests, 1);
    }
    void redirectLoopIsBounded()
    {
        Server server;
        server.response = "HTTP/1.1 302 Found\r\nLocation: /again\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        const auto result = QtHttpTransport{}.get({server.url()}, {});
        QVERIFY(std::holds_alternative<Error>(result));
        QCOMPARE(server.requests, 6);
    }
    void responseLimitsAndCompressedBoundaries()
    {
        Server server;
        QtHttpTransport transport;
        for (const qint64 limit : {qint64(-1), qint64(0), qint64(64) * 1024 * 1024 + 1}) {
            RequestContext context; context.responseByteLimit = limit;
            QCOMPARE(std::get<Error>(transport.get({server.url()}, context)).code, ErrorCode::InvalidResponse);
        }
        QCOMPARE(server.requests, 0);
        for (const int size : {65535, 65536, 65537, 131072}) {
            const QByteArray body(size, 'x');
            const auto compressed = qCompress(body).mid(4);
            server.response = "HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: "
                + QByteArray::number(compressed.size()) + "\r\nConnection: close\r\n\r\n" + compressed;
            RequestContext context; context.responseByteLimit = size;
            const auto result = transport.get({server.url()}, context);
            QVERIFY(std::holds_alternative<HttpResponse>(result));
            QCOMPARE(std::get<HttpResponse>(result).body, body);
            context.responseByteLimit = size - 1;
            QCOMPARE(std::get<Error>(transport.get({server.url()}, context)).code, ErrorCode::ResponseTooLarge);
        }
        server.response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n[]";
        RequestContext context; context.responseByteLimit = 2;
        QVERIFY(std::holds_alternative<HttpResponse>(transport.get({server.url()}, context)));
        context.responseByteLimit = 1;
        QCOMPARE(std::get<Error>(transport.get({server.url()}, context)).code, ErrorCode::ResponseTooLarge);
    }
    void limitsDecompressedBytes()
    {
        Server server;
        const auto compressed = qCompress(QByteArray(4096, 'x')).mid(4);
        server.response = "HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: "
            + QByteArray::number(compressed.size()) + "\r\nConnection: close\r\n\r\n" + compressed;
        RequestContext context; context.responseByteLimit = 100;
        const auto result = QtHttpTransport{}.get({server.url()}, context);
        QVERIFY(std::holds_alternative<Error>(result));
        QCOMPARE(std::get<Error>(result).code, ErrorCode::ResponseTooLarge);
    }
    void cancellationAndTotalDeadlineAbortTransport()
    {
        Server server; server.stall = true;
        RequestContext context;
        QTimer::singleShot(30, this, [context]() { context.cancelled->store(true); });
        auto result = QtHttpTransport{}.get({server.url()}, context);
        QCOMPARE(std::get<Error>(result).code, ErrorCode::Cancelled);
        context = {}; context.deadline = QDeadlineTimer(30);
        result = QtHttpTransport{}.get({server.url()}, context);
        QCOMPARE(std::get<Error>(result).code, ErrorCode::Timeout);
    }
    void partialBodyCannotBecomeACompleteCatalog()
    {
        Server server;
        server.response = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\n[]";
        const auto result = QtHttpTransport{}.get({server.url()}, {});
        QVERIFY(std::holds_alternative<Error>(result));
    }
    void duplicateIdentitiesMergeBeforeLocalFiltering()
    {
        auto http = std::make_shared<Transport>();
        http->bodies[QStringLiteral("get_vod_streams")] = R"([{"stream_id":"007","name":"Movie","category_id":"excluded"},{"stream_id":"007","name":"Movie","category_id":"selected"}])";
        http->bodies[QStringLiteral("get_vod_categories")] = R"([{"category_id":"selected","category_name":"One"},{"category_id":"selected","category_name":"Duplicate"}])";
        http->bodies[QStringLiteral("get_series_categories")] = R"([{"category_id":"series","category_name":"Series"}])";
        XtreamVodProvider provider(http); const auto src = source();
        auto scope = scopeFor(src); scope.categoryId = QStringLiteral("selected");
        const auto response = provider.fetchCatalog(src, scope, {}, requestFor(src));
        QVERIFY(std::holds_alternative<CatalogBatch>(response));
        const auto batch = std::get<CatalogBatch>(response); QCOMPARE(batch.items.size(), 1);
        const auto movie = std::get<MovieSummary>(batch.items.first());
        QVERIFY(movie.categoryIds.contains(QStringLiteral("selected"))); QVERIFY(movie.categoryIds.contains(QStringLiteral("excluded")));
        scope.categoryId.reset();
        QCOMPARE(std::get<QList<VodCategory>>(provider.listCategories(src, scope, requestFor(src))).size(), 1);
        scope.kind = CatalogKind::Series;
        // Category configuration does not opt into series catalogue/playback.
        QCOMPARE(std::get<QList<VodCategory>>(provider.listCategories(src, scope, requestFor(src))).size(), 1);
        QVERIFY(std::holds_alternative<Error>(provider.fetchCatalog(src, scope, {}, requestFor(src))));
    }
    void movieMappingAndLocalCategoryFilter()
    {
        auto http = std::make_shared<Transport>();
        http->bodies[QStringLiteral("get_vod_streams")] = R"([{"stream_id":"007","name":"Synthetic movie","year":"2025","category_id":"a","category_ids":["a",2]}, {"stream_id":8,"name":"Other","category_id":"b"}])";
        XtreamVodProvider provider(http);
        const auto src = source();
        auto scope = scopeFor(src); scope.categoryId = QStringLiteral("2");
        const auto result = provider.fetchCatalog(src, scope, {}, requestFor(src));
        QVERIFY(std::holds_alternative<CatalogBatch>(result));
        const auto batch = std::get<CatalogBatch>(result);
        QCOMPARE(batch.items.size(), 1);
        const auto movie = std::get<MovieSummary>(batch.items.first());
        QCOMPARE(movie.ref.providerItemId, QStringLiteral("007"));
        QCOMPARE(movie.year, std::optional<int>(2025));
        QVERIFY(batch.complete);
        QCOMPARE(QUrlQuery(http->urls.last()).queryItemValue(QStringLiteral("category_id")), QStringLiteral("2"));
        QVERIFY(http->urls.last().toEncoded().contains("synthetic%2Buser"));
    }
    void movieDetailsExposeOnlyCompleteDeclaredResolution()
    {
        auto http = std::make_shared<Transport>();
        XtreamVodProvider provider(http);
        const auto src = source();
        http->bodies[QStringLiteral("get_vod_info")] = R"({"info":{"video":{"width":"1920","height":1080}},"movie_data":{"stream_id":"007"}})";
        auto result = provider.fetchDetails(src, movieFor(src), requestFor(src));
        QVERIFY(std::holds_alternative<VodDetails>(result));
        QCOMPARE(std::get<VodDetails>(result).declaredVideoWidth, std::optional<int>(1920));
        QCOMPARE(std::get<VodDetails>(result).declaredVideoHeight, std::optional<int>(1080));

        http->bodies[QStringLiteral("get_vod_info")] = R"({"info":{"video":[{"width":3840}]},"movie_data":{"stream_id":"007"}})";
        result = provider.fetchDetails(src, movieFor(src), requestFor(src));
        QVERIFY(std::holds_alternative<VodDetails>(result));
        QVERIFY(!std::get<VodDetails>(result).declaredVideoWidth);
        QVERIFY(!std::get<VodDetails>(result).declaredVideoHeight);
    }
    void invalidPayloadAndAuthErrorsStayErrors()
    {
        auto http = std::make_shared<Transport>();
        XtreamVodProvider provider(http);
        const auto src = source();
        for (const auto &body : {QByteArray("{}"), QByteArray("[null]"), QByteArray("<html>private</html>")}) {
            http->bodies[QStringLiteral("get_vod_streams")] = body;
            const auto result = provider.fetchCatalog(src, scopeFor(src), {}, requestFor(src));
            QCOMPARE(std::get<Error>(result).code, ErrorCode::InvalidResponse);
        }
        for (const int status : {401, 403}) {
            http->status = status;
            const auto count = http->urls.size();
            const auto result = provider.fetchCatalog(src, scopeFor(src), {}, requestFor(src));
            QCOMPARE(std::get<Error>(result).code, status == 401 ? ErrorCode::Unauthorized : ErrorCode::Forbidden);
            QCOMPARE(http->urls.size(), count + 1);
        }
    }
    void resolverUsesProviderExtensionAndEncodesSegments()
    {
        auto http = std::make_shared<Transport>();
        XtreamVodProvider provider(http);
        const auto src = source();
        http->bodies[QStringLiteral("get_vod_info")] = R"({"info":{},"movie_data":{"stream_id":"007","container_extension":"mkv"}})";
        const auto result = provider.resolvePlayback(src, movieFor(src), {}, requestFor(src));
        QVERIFY(std::holds_alternative<PlaybackDescriptor>(result));
        const auto descriptor = std::get<PlaybackDescriptor>(result);
        QVERIFY(descriptor.mediaUri.toEncoded().endsWith("/movie/synthetic%2Buser/synthetic%2Fp%25ass/007.mkv"));
        http->bodies[QStringLiteral("get_vod_info")] = R"({"info":{},"movie_data":{"stream_id":"007"}})";
        QVERIFY(std::holds_alternative<Error>(provider.resolvePlayback(src, movieFor(src), {}, requestFor(src))));
        http->bodies[QStringLiteral("get_vod_info")] = R"({"info":{},"movie_data":{"stream_id":"007","direct_source":"file:///tmp/private"}})";
        QVERIFY(std::holds_alternative<Error>(provider.resolvePlayback(src, movieFor(src), {}, requestFor(src))));
    }
};
QTEST_GUILESS_MAIN(VodProviderTests)
#include "tst_vod_provider.moc"
