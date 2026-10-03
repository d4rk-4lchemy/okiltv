#include "player/mpvplaybackengine.h"
#include "core/debuglogger.h"
#include "core/trackpreferences.h"
#include <QFile>
#include <QGuiApplication>
#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickFramebufferObject>
#include <QQuickWindow>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

using namespace OKILTV::Player;
namespace {
class VideoSurface final : public QQuickFramebufferObject {
public:
    MpvPlayer *player = nullptr;
    class VideoRenderer final : public QQuickFramebufferObject::Renderer {
        QPointer<MpvPlayer> m_player;
    public:
        ~VideoRenderer() override { if (m_player && m_player->nativeRenderSurface()) m_player->releaseRenderContext(); }
        void synchronize(QQuickFramebufferObject *item) override { m_player = static_cast<VideoSurface *>(item)->player; }
        QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
        {
            QOpenGLFramebufferObjectFormat format;
            format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
            return new QOpenGLFramebufferObject(size, format);
        }
        void render() override
        {
            QQuickOpenGLUtils::resetOpenGLState();
            const auto *fbo = framebufferObject();
            m_player->renderToFbo(static_cast<int>(fbo->handle()), fbo->width(), fbo->height());
            QQuickOpenGLUtils::resetOpenGLState();
        }
    };
    Renderer *createRenderer() const override { return new VideoRenderer; }
};
std::unique_ptr<MpvPlayer> backend(bool render = false)
{
    auto player = std::make_unique<MpvPlayer>();
    player->configureOptions({{QStringLiteral("vo"), render ? QStringLiteral("libmpv") : QStringLiteral("null")},
        {QStringLiteral("ao"), QStringLiteral("null")}, {QStringLiteral("hwdec"), QStringLiteral("no")}});
    return player;
}
class RangeServer : public QTcpServer {
public:
    QByteArray data;
    int ranges = 0;
    bool ignoreRanges = false;
    bool stall = false;
    bool unavailable = false;
    RangeServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto request = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, request]() {
                    request->append(socket->readAll());
                    if (!request->contains("\r\n\r\n")) return;
                    qint64 start = 0;
                    qint64 end = data.size() - 1;
                    const auto offset = request->indexOf("Range: bytes=");
                    if (offset >= 0) {
                        ++ranges;
                        if (!ignoreRanges) {
                            const auto parts = request->mid(offset + 13).split('\r').first().split('-');
                            start = parts.first().toLongLong();
                            if (parts.size() > 1 && !parts[1].isEmpty()) end = std::min(end, parts[1].toLongLong());
                        }
                    }
                    if (stall) return;
                    if (unavailable) {
                        socket->write("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    if (start < 0 || start >= data.size()) { socket->disconnectFromHost(); return; }
                    const auto body = data.mid(start, end - start + 1);
                    const bool partial = offset >= 0 && !ignoreRanges;
                    QByteArray headers = partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
                    if (partial) headers += "Content-Range: bytes " + QByteArray::number(start) + '-' + QByteArray::number(end) + '/' + QByteArray::number(data.size()) + "\r\n";
                    headers += "Accept-Ranges: bytes\r\nContent-Type: video/MP2T\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
                    socket->write(headers + body); socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        listen(QHostAddress::LocalHost);
    }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/film.ts").arg(serverPort())); }
};
}
class VodMpvTests : public QObject {
    Q_OBJECT
    QTemporaryDir m_media;
private slots:
    void initTestCase()
    {
        qunsetenv("OKILTV_HEADLESS_TEST");
        QVERIFY(m_media.isValid());
        for (const auto &extension : {QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("ts")}) {
            QProcess ffmpeg;
            ffmpeg.start(QStringLiteral("ffmpeg"), {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("color=c=red:s=320x240:r=10"),
                QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=frequency=440:sample_rate=48000"),
                QStringLiteral("-t"), QStringLiteral("2"), QStringLiteral("-c:v"), QStringLiteral("mpeg2video"),
                QStringLiteral("-c:a"), QStringLiteral("aac"), QStringLiteral("-y"), m_media.filePath(QStringLiteral("film.") + extension)});
            QVERIFY(ffmpeg.waitForFinished(30000));
            QCOMPARE(ffmpeg.exitCode(), 0);
        }
    }
    void explicitTracksRestoreAcrossLoads()
    {
        const auto subtitlePath = m_media.filePath(QStringLiteral("tracks.srt"));
        QFile subtitles(subtitlePath); QVERIFY(subtitles.open(QIODevice::WriteOnly));
        subtitles.write("1\n00:00:00,000 --> 00:00:20,000\nFixture subtitles\n"); subtitles.close();
        const auto path = m_media.filePath(QStringLiteral("tracks.mkv"));
        QProcess ffmpeg;
        ffmpeg.start(QStringLiteral("ffmpeg"), {QStringLiteral("-v"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=frequency=440"),
            QStringLiteral("-i"), subtitlePath, QStringLiteral("-map"), QStringLiteral("0:a"),
            QStringLiteral("-map"), QStringLiteral("0:a"), QStringLiteral("-map"), QStringLiteral("1:s"),
            QStringLiteral("-metadata:s:a:0"), QStringLiteral("language=eng"),
            QStringLiteral("-metadata:s:a:1"), QStringLiteral("language=pol"),
            QStringLiteral("-t"), QStringLiteral("20"), QStringLiteral("-c:a"), QStringLiteral("aac"),
            QStringLiteral("-c:s"), QStringLiteral("srt"), QStringLiteral("-y"), path});
        QVERIFY(ffmpeg.waitForFinished(30000)); QCOMPARE(ffmpeg.exitCode(), 0);
        QJsonObject saved;
        for (int pass = 0; pass < 4; ++pass) {
            if (pass == 3) {
                // Movie details use ffprobe metadata and per-type ordinals,
                // before any mpv track IDs exist.
                saved = {{QStringLiteral("audio"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("track")},
                    {QStringLiteral("ordinal"), 1}, {QStringLiteral("lang"), QStringLiteral("pol")},
                    {QStringLiteral("codec"), QStringLiteral("aac")}}},
                    {QStringLiteral("sub"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("track")},
                    {QStringLiteral("ordinal"), 0}, {QStringLiteral("codec"), QStringLiteral("subrip")}}}};
            }
            MpvPlaybackEngine engine(backend());
            PlaybackEvent last;
            engine.setListener([&](const PlaybackEvent &event) { last = event; });
            QSignalSpy ready(engine.player(), &MpvPlayer::trackListReady);
            PlaybackRequest request; request.mediaUri = QUrl::fromLocalFile(path);
            request.startPaused = true; request.trackPreferences = saved;
            engine.load(request, QUuid::createUuid());
            QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 5000);
            if (pass == 0) {
                engine.player()->selectAudioTrack(2, true);
                engine.player()->selectSubtitleTrack(1, true);
                QTRY_VERIFY(last.trackPreferences && last.trackPreferences->contains(QStringLiteral("audio"))
                    && last.trackPreferences->contains(QStringLiteral("sub")));
                saved = *last.trackPreferences;
            } else {
                QTRY_COMPARE(OKILTV::Core::selectedTrackId(engine.player()->trackList(), QStringLiteral("audio")), pass == 2 ? 1 : 2);
                QTRY_COMPARE(OKILTV::Core::selectedTrackId(engine.player()->trackList(), QStringLiteral("sub")), pass == 2 ? 0 : 1);
                if (pass == 1) {
                    // Explicit baseline selection is remembered for VOD too.
                    engine.player()->selectAudioTrack(1, true);
                    engine.player()->selectSubtitleTrack(0, true);
                    QTRY_VERIFY(last.trackPreferences
                        && last.trackPreferences->value(QStringLiteral("audio")).toObject().value(QStringLiteral("id")).toInt() == 1
                        && last.trackPreferences->value(QStringLiteral("sub")).toObject().value(QStringLiteral("mode")).toString() == QStringLiteral("off"));
                    saved = *last.trackPreferences;
                }
            }
            engine.stop(EndReason::UserStop);
            QTRY_VERIFY(last.end.has_value());
        }
    }
    void finiteFilesHaveDurationAndOneNaturalEnd_data()
    {
        QTest::addColumn<QString>("extension");
        QTest::newRow("mp4") << QStringLiteral("mp4");
        QTest::newRow("mkv") << QStringLiteral("mkv");
        QTest::newRow("ts") << QStringLiteral("ts");
    }
    void finiteFilesHaveDurationAndOneNaturalEnd()
    {
        QFETCH(QString, extension);
        MpvPlaybackEngine engine(backend());
        QList<PlaybackEvent> events;
        engine.setListener([&](const PlaybackEvent &event) { events.append(event); });
        PlaybackRequest request; request.mediaUri = QUrl::fromLocalFile(m_media.filePath(QStringLiteral("film.") + extension));
        const auto token = QUuid::createUuid();
        engine.load(request, token);
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(events.cbegin(), events.cend(), [](const auto &event) { return event.state == EngineState::Loaded && event.durationMs && *event.durationMs > 1000 && event.seekable; }), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(events.cbegin(), events.cend(), [token](const auto &event) {
            return event.loadToken == token && event.videoWidth == 320 && event.videoHeight == 240 && event.tracks.size() >= 2;
        }), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!events.isEmpty() && events.last().end.has_value(), 5000);
        QCOMPARE(events.last().end, std::optional<EndReason>(EndReason::NaturalEnd));
        QCOMPARE(events.last().loadToken, token);
        QCOMPARE(std::count_if(events.cbegin(), events.cend(), [](const auto &event) { return event.end.has_value(); }), 1);
        QVERIFY(!engine.player()->usesLiveMpegTsTransport());
    }
    void globalOptionsAreInheritedBelowOnDemandPolicy()
    {
        auto player = std::make_unique<MpvPlayer>();
        player->configureOptions({{QStringLiteral("vo"), QStringLiteral("null")},
            {QStringLiteral("ao"), QStringLiteral("null")}, {QStringLiteral("hwdec"), QStringLiteral("no")},
            {QStringLiteral("volume"), QStringLiteral("37")}, {QStringLiteral("pause"), QStringLiteral("yes")},
            {QStringLiteral("keep-open"), QStringLiteral("always")}, {QStringLiteral("loop-file"), QStringLiteral("inf")},
            {QStringLiteral("loop-playlist"), QStringLiteral("inf")},
            {QStringLiteral("ab-loop-a"), QStringLiteral("0.4")}, {QStringLiteral("ab-loop-b"), QStringLiteral("0.8")},
            {QStringLiteral("start"), QStringLiteral("0.6")}, {QStringLiteral("end"), QStringLiteral("0.8")},
            {QStringLiteral("length"), QStringLiteral("0.2")}, {QStringLiteral("frames"), QStringLiteral("1")}});
        MpvPlaybackEngine engine(std::move(player));
        QList<PlaybackEvent> events;
        engine.setListener([&](const PlaybackEvent &event) { events.append(event); });
        PlaybackRequest request;
        request.mediaUri = QUrl::fromLocalFile(m_media.filePath(QStringLiteral("film.mkv")));
        request.validatedEngineOptions.insert(QStringLiteral("keep-open"), QStringLiteral("no"));
        engine.load(request, QUuid::createUuid());
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(events.cbegin(), events.cend(), [](const auto &event) {
            return event.state == EngineState::Playing;
        }), 5000);
        const auto volume = engine.player()->volumePercent();
        QVERIFY(volume);
        QCOMPARE(*volume, 37.0);
        QTRY_VERIFY_WITH_TIMEOUT(!events.isEmpty() && events.last().end.has_value(), 5000);
        QCOMPARE(events.last().end, std::optional<EndReason>(EndReason::NaturalEnd));
        QCOMPARE(std::count_if(events.cbegin(), events.cend(), [](const auto &event) { return event.end.has_value(); }), 1);
        QVERIFY(std::any_of(events.cbegin(), events.cend(), [](const auto &event) { return event.positionMs > 1000; }));
    }
    void invalidSessionOptionsAreRejectedBeforeLoading()
    {
        auto player = backend();
        PlaybackRequest request;
        request.mediaUri = QUrl::fromLocalFile(m_media.filePath(QStringLiteral("film.mkv")));
        request.validatedEngineOptions.insert(QStringLiteral("keep-open"), QStringLiteral("yes"));
        QVERIFY(!player->play(request, QUuid::createUuid()));
        request.validatedEngineOptions = {{QStringLiteral("script"), QStringLiteral("untrusted.lua")}};
        QVERIFY(!player->play(request, QUuid::createUuid()));
        request.validatedEngineOptions.clear();
        QVERIFY(player->play(request, QUuid::createUuid()));
        player->stop();
    }
    void stopBeforeStartOrRenderHasOneAcknowledgement_data()
    {
        QTest::addColumn<bool>("waitingForRender");
        QTest::newRow("immediate-native-stop") << false;
        QTest::newRow("before-render-context") << true;
    }
    void stopBeforeStartOrRenderHasOneAcknowledgement()
    {
        QFETCH(bool, waitingForRender);
        MpvPlaybackEngine engine(backend(waitingForRender), waitingForRender);
        QList<PlaybackEvent> events;
        engine.setListener([&](const PlaybackEvent &event) { events.append(event); });
        PlaybackRequest request; request.mediaUri = QUrl::fromLocalFile(m_media.filePath(QStringLiteral("film.mkv")));
        const auto token = QUuid::createUuid();
        engine.load(request, token);
        engine.stop(EndReason::UserStop);
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(events.cbegin(), events.cend(), [](const auto &event) { return event.end.has_value(); }), 5000);
        QTest::qWait(150);
        const auto ends = std::count_if(events.cbegin(), events.cend(), [](const auto &event) { return event.end.has_value(); });
        QCOMPARE(ends, 1);
        QCOMPARE(events.last().loadToken, token);
        QCOMPARE(events.last().end, std::optional<EndReason>(EndReason::UserStop));
    }
    void rapidReplacementRetainsLoadIdentity()
    {
        MpvPlaybackEngine engine(backend());
        QList<PlaybackEvent> events;
        engine.setListener([&](const PlaybackEvent &event) { events.append(event); });
        PlaybackRequest request; request.mediaUri = QUrl::fromLocalFile(m_media.filePath(QStringLiteral("film.mkv")));
        const auto first = QUuid::createUuid();
        const auto second = QUuid::createUuid();
        engine.load(request, first);
        engine.load(request, second);
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(events.cbegin(), events.cend(), [&](const auto &event) { return event.loadToken == second && event.state == EngineState::Loaded; }), 5000);
        auto ended = std::find_if(events.cbegin(), events.cend(), [&](const auto &event) { return event.loadToken == first && event.end; });
        QVERIFY(ended != events.cend()); QCOMPARE(ended->end, std::optional<EndReason>(EndReason::Replaced));
        engine.stop(EndReason::UserStop);
        QTRY_VERIFY(!events.isEmpty() && events.last().end.has_value());
        QCOMPARE(events.last().loadToken, second);
        QCOMPARE(events.last().end, std::optional<EndReason>(EndReason::UserStop));
    }
    void nativeHttpTsSupportsSeekAndPause()
    {
        RangeServer server;
        QFile file(m_media.filePath(QStringLiteral("film.ts"))); QVERIFY(file.open(QIODevice::ReadOnly)); server.data = file.readAll();
        MpvPlaybackEngine engine(backend());
        QList<PlaybackEvent> events;
        engine.setListener([&](const PlaybackEvent &event) { events.append(event); });
        PlaybackRequest request; request.mediaUri = server.url();
        engine.load(request, QUuid::createUuid());
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(events.cbegin(), events.cend(), [](const auto &event) { return event.state == EngineState::Loaded && event.seekable; }), 5000);
        QVERIFY(!engine.player()->usesLiveMpegTsTransport());
        engine.pause();
        QTRY_VERIFY(!events.isEmpty() && events.last().state == EngineState::Paused);
        engine.seek(1000);
        QTRY_VERIFY(std::any_of(events.cbegin(), events.cend(), [](const auto &event) { return event.seekCompleted; }));
        QVERIFY(server.ranges > 0);
        engine.stop(EndReason::UserStop);
        QTRY_VERIFY(!events.isEmpty() && events.last().end.has_value());
    }
    void httpMp4PlaybackAndFallback_data()
    {
        QTest::addColumn<bool>("ignoreRanges");
        QTest::newRow("cached-ranges") << false;
        QTest::newRow("native-no-ranges") << true;
    }
    void stopDuringHttpPreparationCannotStartRetiredLoad()
    {
        RangeServer server; server.stall = true;
        MpvPlaybackEngine engine(backend());
        QList<PlaybackEvent> events;
        engine.setListener([&](const PlaybackEvent &event) { events.append(event); });
        PlaybackRequest request; request.mediaUri = server.url(); request.mediaUri.setPath(QStringLiteral("/film.mp4"));
        const auto retired = QUuid::createUuid();
        engine.load(request, retired);
        QTRY_COMPARE(server.ranges, 1);
        engine.stop(EndReason::UserStop);
        QCOMPARE(events.last().loadToken, retired);
        QCOMPARE(events.last().end, std::optional<EndReason>(EndReason::UserStop));
        request.mediaUri = QUrl::fromLocalFile(m_media.filePath(QStringLiteral("film.mp4")));
        const auto current = QUuid::createUuid();
        engine.load(request, current);
        QTRY_VERIFY(std::any_of(events.cbegin(), events.cend(), [current](const auto &event) {
            return event.loadToken == current && event.state == EngineState::Playing;
        }));
        QCOMPARE(std::count_if(events.cbegin(), events.cend(), [retired](const auto &event) {
            return event.loadToken == retired && event.end.has_value();
        }), 1);
        QVERIFY(std::none_of(events.cbegin(), events.cend(), [retired](const auto &event) {
            return event.loadToken == retired && event.state == EngineState::Loaded;
        }));
        engine.stop(EndReason::UserStop);
    }
    void httpUnavailableIsRetryableWithoutImmediateNativeRetry()
    {
        RangeServer server; server.unavailable = true;
        MpvPlaybackEngine engine(backend());
        QList<PlaybackEvent> events;
        engine.setListener([&](const PlaybackEvent &event) { events.append(event); });
        PlaybackRequest request; request.mediaUri = server.url(); request.mediaUri.setPath(QStringLiteral("/film.mp4"));
        engine.load(request, QUuid::createUuid());
        QTRY_VERIFY(!events.isEmpty() && events.last().end.has_value());
        QCOMPARE(events.last().end, std::optional<EndReason>(EndReason::Error));
        QVERIFY(events.last().retryable);
        QCOMPARE(server.ranges, 1);
    }
    void pauseIntentSurvivesHttpPreparation_data()
    {
        QTest::addColumn<bool>("pause");
        QTest::newRow("pause-during-open") << true;
        QTest::newRow("resume-during-open") << false;
    }
    void pauseIntentSurvivesHttpPreparation()
    {
        QFETCH(bool, pause);
        RangeServer server;
        QFile file(m_media.filePath(QStringLiteral("film.mp4"))); QVERIFY(file.open(QIODevice::ReadOnly)); server.data = file.readAll();
        MpvPlaybackEngine engine(backend());
        QList<PlaybackEvent> events;
        engine.setListener([&](const PlaybackEvent &event) { events.append(event); });
        PlaybackRequest request; request.mediaUri = server.url(); request.mediaUri.setPath(QStringLiteral("/film.mp4"));
        request.startPaused = !pause;
        engine.load(request, QUuid::createUuid());
        if (pause) engine.pause(); else engine.resume();
        QTRY_VERIFY(!events.isEmpty() && events.last().state == (pause ? EngineState::Paused : EngineState::Playing));
        engine.stop(EndReason::UserStop);
    }
    void httpMp4PlaybackAndFallback()
    {
        QFETCH(bool, ignoreRanges);
        RangeServer server; server.ignoreRanges = ignoreRanges;
        QFile file(m_media.filePath(QStringLiteral("film.mp4"))); QVERIFY(file.open(QIODevice::ReadOnly)); server.data = file.readAll();
        MpvPlaybackEngine engine(backend());
        QList<PlaybackEvent> events;
        engine.setListener([&](const PlaybackEvent &event) { events.append(event); });
        PlaybackRequest request; request.mediaUri = server.url(); request.mediaUri.setPath(QStringLiteral("/film.mp4"));
        request.startPaused = true;
        const auto token = QUuid::createUuid();
        engine.load(request, token);
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(events.cbegin(), events.cend(), [](const auto &event) {
            return event.state == EngineState::Loaded && event.durationMs.value_or(0) > 1000;
        }), 5000);
        QTRY_VERIFY(!events.isEmpty() && events.last().state == EngineState::Paused);
        if (!ignoreRanges) {
            engine.seek(1000);
            QTRY_VERIFY(std::any_of(events.cbegin(), events.cend(), [](const auto &event) { return event.seekCompleted; }));
            QCOMPARE(server.ranges, 1); // Seek stays within the previously read byte block.
        }
        engine.resume();
        QTRY_VERIFY(std::any_of(events.cbegin(), events.cend(), [](const auto &event) { return event.positionMs > 1100; }));
        engine.stop(EndReason::UserStop);
        QTRY_VERIFY(!events.isEmpty() && events.last().end.has_value());
        QVERIFY(std::all_of(events.cbegin(), events.cend(), [token](const auto &event) { return event.loadToken == token; }));
    }
    void missingMediaIsError()
    {
        MpvPlaybackEngine engine(backend());
        std::optional<EndReason> end;
        engine.setListener([&](const PlaybackEvent &event) { if (event.end) end = event.end; });
        PlaybackRequest request; request.mediaUri = QUrl::fromLocalFile(m_media.filePath(QStringLiteral("missing.mkv")));
        engine.load(request, QUuid::createUuid());
        QTRY_VERIFY(end.has_value());
        QCOMPARE(end, std::optional<EndReason>(EndReason::Error));
    }
    void rendererProducesVideoFrame()
    {
        const auto logCursor = OKILTV::Core::DebugLogger::instance().latestCursor();
        MpvPlaybackEngine engine(backend(true), true);
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
        QQuickWindow window;
        window.resize(128, 96);
        auto *surface = new VideoSurface;
        surface->player = engine.player();
        surface->setParentItem(window.contentItem());
        surface->setSize(QSizeF(128, 96));
        PlaybackRequest request; request.mediaUri = QUrl::fromLocalFile(m_media.filePath(QStringLiteral("film.mkv")));
        engine.load(request, QUuid::createUuid());
        QVERIFY(!engine.player()->renderContextAvailable());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        bool redFrame = false;
        QTimer render;
        connect(&render, &QTimer::timeout, this, [&]() {
            surface->update();
            const auto frame = window.grabWindow();
            if (frame.isNull()) return;
            const auto pixel = frame.pixelColor(frame.width() / 2, frame.height() / 2);
            redFrame = redFrame || (pixel.red() > 100 && pixel.green() < 80);
        });
        render.start(20);
        const bool rendered = QTest::qWaitFor([&]() { return redFrame; }, 5000);
        if (!redFrame) {
            for (const auto &entry : OKILTV::Core::DebugLogger::instance().entriesSince(logCursor)) qInfo().noquote() << entry.line;
        }
        QVERIFY(rendered);
        render.stop();
        engine.stop(EndReason::UserStop);
    }
};
QTEST_MAIN(VodMpvTests)
#include "tst_vod_mpv.moc"
