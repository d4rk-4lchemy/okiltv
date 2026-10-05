// Included by tst_app_models.cpp to exercise the production composition.
void AppModelTests::vodCatalogSessionLibrary()
{
    using namespace OKILTV::Vod;
    QTemporaryDir dir;
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto endpoint = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    QImage poster(100, 150, QImage::Format_RGB32); poster.fill(QColor(QStringLiteral("#315776")));
    QByteArray posterBytes; QBuffer posterBuffer(&posterBytes); posterBuffer.open(QIODevice::WriteOnly); QVERIFY(poster.save(&posterBuffer, "PNG"));
    QJsonArray rows;
    for (int i = 0; i < 105; ++i) rows.append(QJsonObject{{QStringLiteral("stream_id"), i + 1},
        {QStringLiteral("name"), QStringLiteral("Movie %1").arg(i, 3, 10, QChar(u'0'))}, {QStringLiteral("year"), 2024},
        {QStringLiteral("category_id"), i % 2 ? QStringLiteral("drama") : QStringLiteral("action")},
        {QStringLiteral("stream_icon"), endpoint + QStringLiteral("/poster.png?token=synthetic-art-secret")}});
    bool failCatalog = false;
    bool failDetails = false;
    int catalogRequests = 0, imageRequests = 0, mediaRequests = 0;
    bool stallMedia = true;
    QPointer<QTcpSocket> probeConnection;
    QElapsedTimer probeDisconnected;
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (auto *socket = server.nextPendingConnection()) {
            auto request = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, request]() {
                request->append(socket->readAll()); if (!request->contains("\r\n\r\n")) return;
                const auto target = QUrl::fromEncoded(request->split(' ').value(1));
                const auto action = QUrlQuery(target).queryItemValue(QStringLiteral("action"));
                QByteArray body = target.path().startsWith(QStringLiteral("/movie/")) ? QByteArray(32, '\0') : QByteArray("[]");
                if (target.path().startsWith(QStringLiteral("/movie/"))) {
                    ++mediaRequests;
                    if (stallMedia) {
                        probeConnection = socket;
                        connect(socket, &QTcpSocket::disconnected, &server, [&]() { probeDisconnected.start(); });
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 1000000\r\n\r\n");
                        return; // Keep real ffprobe blocked in a network read.
                    }
                }
                if (target.path() == QStringLiteral("/poster.png")) { body = posterBytes; ++imageRequests; QVERIFY(!request->toLower().contains("authorization:")); }
                else if (action == QStringLiteral("get_vod_categories")) body = R"([{"category_id":"action","category_name":"Action"},{"category_id":"drama","category_name":"Drama"}])";
                else if (action == QStringLiteral("get_vod_streams")) { ++catalogRequests; body = failCatalog ? QByteArray("invalid") : QJsonDocument(rows).toJson(QJsonDocument::Compact); }
                else if (action == QStringLiteral("get_vod_info")) {
                    const auto id = QUrlQuery(target).queryItemValue(QStringLiteral("vod_id")).toInt();
                    const QList<QSize> sizes{{320, 240}, {854, 480}, {1280, 720}, {1920, 800}, {2560, 1080}, {3840, 1600}, {0, 0}};
                    const auto size = sizes.value(id - 1);
                    body = QJsonDocument(QJsonObject{
                        {QStringLiteral("info"), QJsonObject{{QStringLiteral("plot"), QStringLiteral("A local test movie.")},
                            {QStringLiteral("duration_secs"), 120}, {QStringLiteral("video"), QJsonObject{
                                {QStringLiteral("width"), size.width()}, {QStringLiteral("height"), size.height()}}}}},
                        {QStringLiteral("movie_data"), QJsonObject{{QStringLiteral("stream_id"), QString::number(id)},
                            {QStringLiteral("container_extension"), QStringLiteral("mp4")}}}}).toJson();
                }
                if (failDetails && action == QStringLiteral("get_vod_info")) body = QByteArray("invalid");
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    SettingsManager settings(dir.filePath(QStringLiteral("settings.json"))); settings.load();
    ServerProfile profile; profile.name = QStringLiteral("Local movies"); profile.xtreamBaseUrl = endpoint;
    profile.xtreamUsername = QStringLiteral("synthetic-user"); profile.xtreamPassword = QStringLiteral("synthetic-password");
    QVERIFY(settings.addProfile(profile));
    ServerProfile other = profile; other.id = QUuid::createUuid(); other.name = QStringLiteral("Second library"); QVERIFY(settings.addProfile(other));
    const auto liveSource = settings.current().activeProfileId;
    QFile settingsFile(settings.settingsFilePath()); QVERIFY(settingsFile.open(QIODevice::ReadOnly)); const auto before = settingsFile.readAll(); settingsFile.close();
    PlayerController player; DvrController dvr(&settings, &player);
    MultiViewController multiview(&settings, nullptr, &player); TimeshiftController timeshift(&settings, &player, &dvr, &multiview);
    Test::Engine *backend = nullptr;
    VodRuntime runtime(&settings, &multiview, &dvr, &timeshift, nullptr, [&]() { auto value = std::make_unique<Test::Engine>(); backend = value.get(); return value; });
    VodCatalogModel catalog(&runtime, &settings);
    VodCatalogModel sidebar(&runtime, &settings, nullptr, VodCatalogModel::Purpose::PlaybackSidebar);
    QVERIFY(!catalog.continueMoviesLoaded());
    QVERIFY(runtime.module()->enabled()); catalog.open();
    QTRY_COMPARE_WITH_TIMEOUT(catalog.count(), 100, 10000);
    QTRY_VERIFY(!catalog.busy());
    QTRY_VERIFY(catalog.continueMoviesLoaded());
    QVERIFY(catalog.continueMovies().isEmpty());
    QVERIFY(catalog.hasMore()); QCOMPARE(catalog.categories().size(), 5); QCOMPARE(catalogRequests, 1);
    QVERIFY(!runtime.active()); QVERIFY(!backend);
    QVERIFY(!settings.current().vodEnabled); QVERIFY(settings.profileById(profile.id)->vodEnabled);
    QCOMPARE(settings.current().activeProfileId, liveSource);
    QVERIFY(settingsFile.open(QIODevice::ReadOnly)); QVERIFY(settingsFile.readAll() != before); settingsFile.close();
    const QStringList resolutions{QStringLiteral("480p"), QStringLiteral("480p"), QStringLiteral("720p"),
        QStringLiteral("1080p"), QStringLiteral("1440p"), QStringLiteral("4K")};
    for (int row = 0; row < resolutions.size(); ++row) {
        catalog.requestResolution(row);
        QTRY_COMPARE(catalog.data(catalog.index(row), VodCatalogModel::ResolutionRole).toString(), resolutions.at(row));
    }
    catalog.fetchMoreMovies(); QTRY_COMPARE(catalog.count(), 105); QVERIFY(!catalog.hasMore());
    catalog.setSearchText(QStringLiteral("missing")); catalog.setSearchText(QStringLiteral("005"));
    QTRY_COMPARE(catalog.count(), 1); QCOMPARE(catalog.data(catalog.index(0), VodCatalogModel::TitleRole).toString(), QStringLiteral("Movie 005"));
    QFile evicted(dir.filePath(QStringLiteral("vod-artwork/old.jpg")));
    QVERIFY(evicted.open(QIODevice::WriteOnly)); QVERIFY(evicted.resize(qint64(256) * 1024 * 1024 + 1)); evicted.close();
    catalog.requestPoster(0);
    QTRY_VERIFY(!catalog.data(catalog.index(0), VodCatalogModel::PosterRole).toString().isEmpty()); QCOMPARE(imageRequests, 1); QVERIFY(!evicted.exists());
    {
        VodCatalogModel warm(&runtime, &settings);
        bool pathsPublishedWithRows = false;
        connect(&warm, &QAbstractItemModel::rowsInserted, &warm, [&](const QModelIndex &, int first, int last) {
            pathsPublishedWithRows = true;
            for (int row = first; row <= last; ++row)
                pathsPublishedWithRows = pathsPublishedWithRows && !warm.data(warm.index(row), VodCatalogModel::PosterRole).toString().isEmpty();
        });
        warm.open(); QTRY_COMPARE(warm.count(), 100); QTRY_VERIFY(!warm.busy());
        QVERIFY(pathsPublishedWithRows);
        warm.requestProgress(0); warm.requestPoster(0);
        QVERIFY(!warm.data(warm.index(0), VodCatalogModel::PosterRole).toString().isEmpty());
        QCOMPARE(imageRequests, 1);
        warm.close();
    }
    QDirIterator files(dir.filePath(QStringLiteral("vod-artwork")), {QStringLiteral("*.url")}, QDir::Files, QDirIterator::Subdirectories);
    QVERIFY(files.hasNext()); QFile registry(files.next()); QVERIFY(registry.open(QIODevice::ReadOnly));
    const auto protectedBytes = registry.readAll(); QVERIFY(!protectedBytes.contains("synthetic-art-secret")); QVERIFY(!protectedBytes.contains("http://"));
    // User lists share durable state across independent catalog models without media requests.
    {
        VodCatalogModel mirror(&runtime, &settings, nullptr, VodCatalogModel::Purpose::PlaybackSidebar);
        mirror.open(); QTRY_COMPARE(mirror.count(), 100); QTRY_VERIFY(!mirror.busy());
        mirror.setSearchText(QStringLiteral("005")); QTRY_COMPARE(mirror.count(), 1);
        const auto key = catalog.data(catalog.index(0), VodCatalogModel::KeyRole).toString();
        const auto mediaBeforeLists = mediaRequests;
        catalog.toggleToWatch(key);
        QTRY_VERIFY(catalog.data(catalog.index(0), VodCatalogModel::ToWatchRole).toBool());
        QTRY_VERIFY(mirror.data(mirror.index(0), VodCatalogModel::ToWatchRole).toBool());
        catalog.toggleFavourite(key);
        QTRY_VERIFY(mirror.data(mirror.index(0), VodCatalogModel::FavouriteRole).toBool());
        mirror.selectCategory(QStringLiteral("__to_watch__")); QTRY_VERIFY(!mirror.busy()); QCOMPARE(mirror.count(), 1);
        mirror.toggleToWatch(key);
        QTRY_COMPARE(mirror.count(), 0); QTRY_VERIFY(!mirror.busy());
        QTRY_VERIFY(!catalog.data(catalog.index(0), VodCatalogModel::ToWatchRole).toBool());
        mirror.selectCategory(QStringLiteral("__favourites__")); QTRY_VERIFY(!mirror.busy()); QCOMPARE(mirror.count(), 1);
        mirror.toggleFavourite(key); QTRY_COMPARE(mirror.count(), 0); QTRY_VERIFY(!mirror.busy());
        QTRY_VERIFY(!catalog.data(catalog.index(0), VodCatalogModel::FavouriteRole).toBool());
        QCOMPARE(mediaRequests, mediaBeforeLists); QVERIFY(!runtime.active()); QVERIFY(!backend);
        mirror.close();
    }
    catalog.selectMovie(0); QTRY_VERIFY(!catalog.busy()); QCOMPARE(catalog.movie().value(QStringLiteral("description")).toString(), QStringLiteral("A local test movie."));
    QCOMPARE(catalog.movie().value(QStringLiteral("resolutionLabel")).toString(), QStringLiteral("4K"));
    QSignalSpy started(&catalog, &VodCatalogModel::playbackStarted);
    QTRY_VERIFY(probeConnection);
    QVERIFY(catalog.movie().value(QStringLiteral("mediaProbeLoading")).toBool());
    QVERIFY(!catalog.busy());
    QVERIFY(catalog.probePlayBlocked());
    catalog.play();
    QVERIFY(!catalog.startingPlayback());
    QVERIFY(!backend);
    QElapsedTimer earlyCompletion;
    earlyCompletion.start();
    probeConnection->disconnectFromHost();
    QTRY_VERIFY(!catalog.probePlayBlocked());
    QVERIFY(earlyCompletion.elapsed() < 5000);
    QVERIFY(!catalog.movie().value(QStringLiteral("mediaProbeLoading")).toBool());
    probeConnection = nullptr;
    probeDisconnected.invalidate();
    catalog.back(); catalog.selectMovie(0);
    QTRY_VERIFY(probeConnection);
    QVERIFY(catalog.probePlayBlocked());
    QTest::qWait(4000);
    QVERIFY(catalog.probePlayBlocked());
    QTRY_VERIFY_WITH_TIMEOUT(!catalog.probePlayBlocked(), 2000);
    QVERIFY(catalog.movie().value(QStringLiteral("mediaProbeLoading")).toBool());
    QVERIFY(!probeDisconnected.isValid());
    catalog.play(); QTRY_COMPARE(started.size(), 1); QCOMPARE(backend->loads, 1);
    QVERIFY(probeDisconnected.isValid());
    QVERIFY(probeDisconnected.elapsed() >= 1900);
    QVERIFY(!catalog.movie().value(QStringLiteral("mediaProbeLoading")).toBool());
    stallMedia = false;
    QVERIFY(runtime.isLoading());
    backend->emitState(OKILTV::Player::EngineState::Loaded);
    QVERIFY(runtime.isLoading());
    backend->emitState(OKILTV::Player::EngineState::Playing);
    QVERIFY(!runtime.isLoading());
    backend->emitState(OKILTV::Player::EngineState::Buffering);
    QVERIFY(runtime.isLoading());
    runtime.togglePause();
    QVERIFY(!runtime.isLoading());
    backend->emitState(OKILTV::Player::EngineState::Paused);
    runtime.togglePause();
    backend->emitState(OKILTV::Player::EngineState::Playing);
    QVERIFY(!runtime.isLoading());
    QTRY_COMPARE(sidebar.count(), 100);
    QCOMPARE(sidebar.sourceId(), profile.id.toString(QUuid::WithoutBraces));
    QTRY_COMPARE(sidebar.playingMovie().value(QStringLiteral("title")).toString(), QStringLiteral("Movie 005"));
    QCOMPARE(sidebar.playingMovie().value(QStringLiteral("year")).toString(), QStringLiteral("2024"));
    QTRY_COMPARE(sidebar.playingMovie().value(QStringLiteral("description")).toString(), QStringLiteral("A local test movie."));
    QVERIFY(!sidebar.playingMovieKey().isEmpty());
    sidebar.fetchMoreMovies(); QTRY_COMPARE(sidebar.count(), 105);
    const auto playingKey = sidebar.playingMovieKey();
    const auto mediaRequestsBeforeGroups = mediaRequests;
    QTRY_COMPARE(sidebar.categories().size(), 5);
    sidebar.selectCategory(QStringLiteral("drama")); QTRY_COMPARE(sidebar.count(), 52);
    QCOMPARE(sidebar.data(sidebar.index(0), VodCatalogModel::TitleRole).toString(), QStringLiteral("Movie 001"));
    sidebar.setSearchText(QStringLiteral("005")); QTRY_COMPARE(sidebar.count(), 1);
    sidebar.selectCategory(QStringLiteral("action")); QTRY_COMPARE(sidebar.count(), 0);
    QCOMPARE(sidebar.searchText(), QStringLiteral("005"));
    sidebar.setSearchText(QString{});
    sidebar.selectCategory(QStringLiteral("drama"));
    sidebar.selectCategory(QStringLiteral("action"));
    QTRY_COMPARE(sidebar.count(), 53); // Latest filter wins, including over a pending search.
    sidebar.fetchMoreMovies(); QTRY_VERIFY(!sidebar.busy()); QCOMPARE(sidebar.count(), 53);
    sidebar.selectCategory(QStringLiteral("__continue_watching__")); QTRY_COMPARE(sidebar.count(), 0);
    sidebar.selectCategory(QString{}); QTRY_COMPARE(sidebar.count(), 100);
    sidebar.fetchMoreMovies(); QTRY_COMPARE(sidebar.count(), 105);
    QCOMPARE(sidebar.playingMovieKey(), playingKey);
    QCOMPARE(backend->loads, 1);
    QCOMPARE(mediaRequests, mediaRequestsBeforeGroups);
    QCOMPARE(catalog.categoryId(), QString{});
    QCOMPARE(catalog.searchText(), QStringLiteral("005"));
    QCOMPARE(settings.current().activeProfileId, liveSource);
    sidebar.setSearchText(QStringLiteral("104")); QTRY_COMPARE(sidebar.count(), 1);
    QCOMPARE(catalog.searchText(), QStringLiteral("005"));
    QCOMPARE(sidebar.playingMovie().value(QStringLiteral("title")).toString(), QStringLiteral("Movie 005"));
    sidebar.setSearchText(QStringLiteral("005")); QTRY_COMPARE(sidebar.count(), 1);
    QTRY_COMPARE(sidebar.data(sidebar.index(0), VodCatalogModel::TitleRole).toString(), QStringLiteral("Movie 005"));
    QSignalSpy sidebarStarted(&sidebar, &VodCatalogModel::playbackStarted);
    sidebar.playRow(0); QCOMPARE(sidebarStarted.size(), 1); QCOMPARE(backend->loads, 1);
    backend->emitState(OKILTV::Player::EngineState::Playing, 65000);
    runtime.stop(); QTRY_VERIFY(!runtime.active());
    QVERIFY(!runtime.isLoading());
    QTRY_COMPARE(catalog.data(catalog.index(0), VodCatalogModel::ProgressRole).toDouble(), 65000.0 / 120000.0);
    catalog.close(); catalog.open(); catalog.requestProgress(0);
    QTRY_COMPARE(catalog.data(catalog.index(0), VodCatalogModel::ProgressRole).toDouble(), 65000.0 / 120000.0);
    catalog.back(); catalog.selectMovie(0); QTRY_VERIFY(catalog.movie().value(QStringLiteral("resumeSeconds")).toInt() >= 60);
    QTRY_COMPARE(catalog.continueMovies().size(), 1);
    QVERIFY(!catalog.continueMovies().first().toMap().value(QStringLiteral("movieKey")).toString().isEmpty());
    QTRY_COMPARE(catalog.continueMovies().first().toMap().value(QStringLiteral("resolutionLabel")).toString(), QStringLiteral("4K"));
    QCOMPARE(catalog.categories().first().toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Continue watching"));
    QCOMPARE(catalog.categories().first().toMap().value(QStringLiteral("iconSource")).toString(), QStringLiteral("qrc:/resources/icons/play.svg"));
    catalog.selectCategory(QStringLiteral("__continue_watching__"));
    QTRY_VERIFY(!catalog.busy()); QCOMPARE(catalog.count(), 1);
    catalog.selectMovie(-1); QTRY_VERIFY(!catalog.busy());
    QCOMPARE(catalog.movie().value(QStringLiteral("title")).toString(), QStringLiteral("Movie 005"));
    catalog.selectCategory(QString{}); QTRY_VERIFY(!catalog.busy());
    catalog.toggleWatched(); QTRY_VERIFY(catalog.movie().value(QStringLiteral("watched")).toBool());
    QTRY_VERIFY(!catalog.busy()); QTRY_COMPARE(catalog.continueMovies().size(), 0);
    catalog.toggleWatched(); QTRY_VERIFY(!catalog.busy());
    QVERIFY(!catalog.movie().value(QStringLiteral("watched")).toBool());
    QCOMPARE(catalog.movie().value(QStringLiteral("resumeSeconds")).toInt(), 0);
    QCOMPARE(catalog.data(catalog.index(0), VodCatalogModel::ProgressRole).toDouble(), 0.0);
    catalog.play(true); QTRY_COMPARE(started.size(), 2);
    backend->emitState(OKILTV::Player::EngineState::Playing, 0); QCOMPARE(backend->seeks, 0);
    QCOMPARE(catalog.data(catalog.index(0), VodCatalogModel::ProgressRole).toDouble(), 0.0);
    QCOMPARE(runtime.title(), QStringLiteral("Movie 005"));
    catalog.back(); catalog.selectMovie(0);
    QCOMPARE(runtime.title(), QStringLiteral("Movie 005"));
    runtime.seekTo(37.25); QCOMPARE(backend->seekPosition, qint64(37250));
    backend->emitState(OKILTV::Player::EngineState::Paused, 37250);
    runtime.seekTo(38); QCOMPARE(backend->seekPosition, qint64(38000));
    runtime.seekTo(-1); QCOMPARE(backend->seekPosition, qint64(0));
    runtime.seekTo(500); QCOMPARE(backend->seekPosition, qint64(120000));
    backend->emitState(OKILTV::Player::EngineState::Paused, 38000, 120000, false);
    const auto seekCount = backend->seeks;
    runtime.seekTo(20); QCOMPARE(backend->seeks, seekCount);
    QTRY_VERIFY(!catalog.busy());
    backend->emitState(OKILTV::Player::EngineState::Paused, 114000);
    QVERIFY(!catalog.movie().value(QStringLiteral("watched")).toBool());
    backend->emitState(OKILTV::Player::EngineState::Paused, 114001);
    QVERIFY(catalog.movie().value(QStringLiteral("watched")).toBool());
    catalog.toggleWatched(); QTRY_VERIFY(!catalog.busy());
    QVERIFY(!catalog.movie().value(QStringLiteral("watched")).toBool());
    backend->emitState(OKILTV::Player::EngineState::Paused, 118000);
    QVERIFY(!catalog.movie().value(QStringLiteral("watched")).toBool());
    runtime.stop(); QVERIFY(runtime.title().isEmpty());
    const auto stoppedRef = runtime.module()->session()->snapshot().ref;
    const auto stoppedSource = std::get<SourceContext>(runtime.store()->snapshot(stoppedRef.profileId));
    RequestContext stoppedRequest; stoppedRequest.source = stoppedSource.revision;
    QTRY_COMPARE(std::get<std::optional<VodProgress>>(runtime.store()->read(stoppedRef, stoppedRequest))->positionMs, qint64(0));
    catalog.back();
    catalog.setSearchText(QString{}); QTRY_COMPARE(catalog.count(), 100);
    // Sidebar handoff preserves the outgoing checkpoint and uses the same resume policy.
    sidebar.setSearchText(QStringLiteral("004")); QTRY_COMPARE(sidebar.count(), 1);
    QTRY_COMPARE(sidebar.data(sidebar.index(0), VodCatalogModel::TitleRole).toString(), QStringLiteral("Movie 004"));
    sidebar.playRow(0); QTRY_COMPARE(sidebarStarted.size(), 2);
    QTRY_COMPARE(runtime.title(), QStringLiteral("Movie 004"));
    QTRY_COMPARE(sidebar.playingMovie().value(QStringLiteral("title")).toString(), QStringLiteral("Movie 004"));
    QTRY_COMPARE(sidebar.playingMovie().value(QStringLiteral("description")).toString(), QStringLiteral("A local test movie."));
    const auto mediaBeforeTelemetry = mediaRequests;
    OKILTV::Player::PlaybackEvent metadataEvent;
    metadataEvent.loadToken = backend->token;
    metadataEvent.state = OKILTV::Player::EngineState::Playing;
    metadataEvent.positionMs = 1000;
    metadataEvent.durationMs = 120000;
    metadataEvent.seekable = true;
    metadataEvent.videoWidth = 1280;
    metadataEvent.videoHeight = 536;
    metadataEvent.tracks = {QVariantMap{{QStringLiteral("id"), 1}, {QStringLiteral("type"), QStringLiteral("video")}},
        QVariantMap{{QStringLiteral("id"), 2}, {QStringLiteral("type"), QStringLiteral("audio")},
            {QStringLiteral("codec"), QStringLiteral("aac")}, {QStringLiteral("lang"), QStringLiteral("pol")}}};
    backend->listener(metadataEvent);
    QCOMPARE(sidebar.playingMovie().value(QStringLiteral("resolutionLabel")).toString(), QStringLiteral("720p"));
    QCOMPARE(sidebar.data(sidebar.index(0), VodCatalogModel::ResolutionRole).toString(), QStringLiteral("720p"));
    QCOMPARE(catalog.data(catalog.index(4), VodCatalogModel::ResolutionRole).toString(), QStringLiteral("720p"));
    catalog.selectMovie(4); QTRY_VERIFY(!catalog.busy());
    QCOMPARE(catalog.movie().value(QStringLiteral("resolutionLabel")).toString(), QStringLiteral("720p"));
    QCOMPARE(catalog.movie().value(QStringLiteral("audioTrackCount")).toInt(), 1);
    catalog.back();
    // Even opening a different movie's details while watching must not probe its media.
    catalog.selectMovie(3); QTRY_VERIFY(!catalog.busy());
    QCOMPARE(mediaRequests, mediaBeforeTelemetry);
    catalog.back();
    const auto metadataRef = runtime.module()->session()->snapshot().ref;
    const auto metadataSource = std::get<SourceContext>(runtime.store()->snapshot(metadataRef.profileId));
    RequestContext metadataRequest; metadataRequest.source = metadataSource.revision;
    const auto cachedPlaybackMetadata = [&]() {
        return std::get<std::optional<VodDetails>>(runtime.store()->readDetails(metadataRef, metadataRequest));
    };
    QTRY_VERIFY(cachedPlaybackMetadata() && cachedPlaybackMetadata()->mediaProbe);
    QCOMPARE(cachedPlaybackMetadata()->mediaProbe->videoWidth, std::optional<int>(1280));
    QCOMPARE(cachedPlaybackMetadata()->mediaProbe->audioTracks.first().language, QStringLiteral("pol"));
    QCOMPARE(mediaRequests, mediaBeforeTelemetry);
    // A reply from an earlier mpv load must not affect the new movie's badges/cache.
    metadataEvent.loadToken = QUuid::createUuid(); metadataEvent.videoWidth = 3840;
    backend->listener(metadataEvent);
    QCOMPARE(catalog.data(catalog.index(4), VodCatalogModel::ResolutionRole).toString(), QStringLiteral("720p"));
    backend->emitState(OKILTV::Player::EngineState::Playing, 70000);
    const auto outgoing = runtime.module()->session()->snapshot().ref;
    sidebar.setSearchText(QStringLiteral("005")); QTRY_COMPARE(sidebar.count(), 1);
    QTRY_COMPARE(sidebar.data(sidebar.index(0), VodCatalogModel::TitleRole).toString(), QStringLiteral("Movie 005"));
    sidebar.playRow(0); QTRY_COMPARE(sidebarStarted.size(), 3);
    const auto outgoingSource = std::get<SourceContext>(runtime.store()->snapshot(outgoing.profileId));
    RequestContext outgoingRequest; outgoingRequest.source = outgoingSource.revision;
    QTRY_VERIFY(std::get<std::optional<VodProgress>>(runtime.store()->read(outgoing, outgoingRequest)).has_value());
    QTRY_COMPARE(std::get<std::optional<VodProgress>>(runtime.store()->read(outgoing, outgoingRequest))->positionMs, qint64(70000));
    sidebar.setSearchText(QStringLiteral("004")); QTRY_COMPARE(sidebar.count(), 1);
    QTRY_COMPARE(sidebar.data(sidebar.index(0), VodCatalogModel::TitleRole).toString(), QStringLiteral("Movie 004"));
    sidebar.playRow(0); QTRY_COMPARE(sidebarStarted.size(), 4);
    backend->emitState(OKILTV::Player::EngineState::Playing, 0);
    backend->emitState(OKILTV::Player::EngineState::Loaded, 0);
    QTRY_COMPARE(backend->seekPosition, qint64(65000));
    backend->emitSeekCompleted(65000);
    catalog.selectSource(other.id.toString(QUuid::WithoutBraces));
    QVERIFY(!catalog.continueMoviesLoaded());
    QTRY_COMPARE(catalog.count(), 100); QTRY_VERIFY(catalog.continueMoviesLoaded());
    QCOMPARE(sidebar.sourceId(), profile.id.toString(QUuid::WithoutBraces));
    QCOMPARE(sidebar.playingMovie().value(QStringLiteral("title")).toString(), QStringLiteral("Movie 004"));
    failDetails = true;
    sidebar.setSearchText(QStringLiteral("103"));
    QTRY_COMPARE(sidebar.data(sidebar.index(0), VodCatalogModel::TitleRole).toString(), QStringLiteral("Movie 103"));
    sidebar.playRow(0);
    QTRY_VERIFY(!sidebar.startingPlayback());
    QVERIFY(!sidebar.errorText().isEmpty());
    QCOMPARE(sidebarStarted.size(), 4);
    QCOMPARE(runtime.title(), QStringLiteral("Movie 004"));
    QCOMPARE(sidebar.playingMovie().value(QStringLiteral("title")).toString(), QStringLiteral("Movie 004"));
    failDetails = false;
    sidebar.setSearchText(QStringLiteral("005"));
    QTRY_COMPARE(sidebar.data(sidebar.index(0), VodCatalogModel::TitleRole).toString(), QStringLiteral("Movie 005"));
    sidebar.playRow(0); QTRY_COMPARE(sidebarStarted.size(), 5);
    QVERIFY(sidebar.errorText().isEmpty());
    runtime.stop(); QTRY_VERIFY(!runtime.active()); QVERIFY(sidebar.playingMovie().isEmpty());
    catalog.selectSource(profile.id.toString(QUuid::WithoutBraces)); QTRY_COMPARE(catalog.count(), 100);
    failCatalog = true; catalog.refresh(); QTRY_VERIFY(!catalog.errorText().isEmpty()); QTRY_VERIFY(!catalog.busy()); QCOMPARE(catalog.count(), 100);
    failCatalog = false;
    catalog.selectCategory(QStringLiteral("drama")); QTRY_COMPARE(catalog.count(), 52);
    catalog.close(); const auto calls = catalogRequests; catalog.open(); QTRY_VERIFY(!catalog.busy()); QCOMPARE(catalog.count(), 52); QCOMPARE(catalogRequests, calls);
    catalog.selectSource(other.id.toString(QUuid::WithoutBraces)); QTRY_COMPARE(catalog.count(), 100); QCOMPARE(settings.current().activeProfileId, liveSource);
    QSignalSpy progressChanged(&catalog, &QAbstractItemModel::dataChanged);
    catalog.requestProgress(0);
    QTRY_VERIFY(!progressChanged.isEmpty());
    QCOMPARE(catalog.data(catalog.index(0), VodCatalogModel::ProgressRole).toDouble(), 0.0);
    rows = {}; catalog.refresh(); QTRY_COMPARE(catalog.count(), 0); QTRY_VERIFY(!catalog.busy());
    const auto emptyCalls = catalogRequests; catalog.close(); catalog.open(); QTRY_VERIFY(!catalog.busy()); QCOMPARE(catalogRequests, emptyCalls);
    {
        const auto source = std::get<SourceContext>(runtime.store()->snapshot(profile.id));
        const ContentRef savedRef{profile.id, source.revision.catalogNamespace, ContentKind::Movie, QStringLiteral("5"), {}};
        RequestContext request; request.source = source.revision;
        VodProgress saved; saved.positionMs = 30000; saved.durationMs = 120000;
        saved.sessionToken = QUuid::createUuid(); saved.sequence = 1; saved.updatedAtUtc = QDateTime::currentDateTimeUtc();
        QVERIFY(std::holds_alternative<Success>(runtime.store()->beginSession(savedRef, saved.sessionToken, request)));
        QVERIFY(std::holds_alternative<Success>(runtime.store()->checkpoint(savedRef, saved, request)));
        VodCatalogModel restored(&runtime, &settings);
        restored.open();
        restored.selectSource(profile.id.toString(QUuid::WithoutBraces));
        restored.setSearchText(QStringLiteral("004"));
        QTRY_COMPARE(restored.count(), 1);
        QSignalSpy progressRead(&restored, &QAbstractItemModel::dataChanged);
        restored.requestProgress(0);
        QTRY_VERIFY(!progressRead.isEmpty());
        QCOMPARE(restored.data(restored.index(0), VodCatalogModel::ProgressRole).toDouble(), 0.25);
        saved.durationMs.reset(); ++saved.sequence;
        QVERIFY(std::holds_alternative<Success>(runtime.store()->checkpoint(savedRef, saved, request)));
        restored.close(); restored.open(); progressRead.clear(); restored.requestProgress(0);
        QTRY_VERIFY(!progressRead.isEmpty());
        QCOMPARE(restored.data(restored.index(0), VodCatalogModel::ProgressRole).toDouble(), 0.0);
        restored.selectMovie(0); QTRY_VERIFY(!restored.busy());
        QTRY_VERIFY(restored.movie().value(QStringLiteral("progressLoaded")).toBool());
        restored.toggleWatched(); QTRY_VERIFY(!restored.busy());
        QCOMPARE(std::get<std::optional<VodProgress>>(runtime.store()->read(savedRef, request))->status, WatchStatus::Watched);
        restored.back(); restored.selectMovie(0); QTRY_VERIFY(!restored.busy());
        QTRY_VERIFY(restored.movie().value(QStringLiteral("progressLoaded")).toBool());
        QVERIFY(restored.movie().value(QStringLiteral("watched")).toBool());
        restored.toggleWatched(); QTRY_VERIFY(!restored.busy());
        const auto reset = std::get<std::optional<VodProgress>>(runtime.store()->read(savedRef, request));
        QVERIFY(reset); QCOMPARE(reset->positionMs, qint64(0)); QCOMPARE(reset->status, WatchStatus::InProgress);
        restored.close();
    }
    catalog.close(); runtime.shutdown();
    {
        const auto previousCatalogueRequests = catalogRequests;
        const auto previousImageRequests = imageRequests;
        VodRuntime restarted(&settings, &multiview, &dvr, &timeshift, nullptr, []() { return std::make_unique<Test::Engine>(); });
        VodCatalogModel fresh(&restarted, &settings);
        bool cachedOnPublication = false;
        connect(&fresh, &QAbstractItemModel::rowsInserted, &fresh, [&](const QModelIndex &, int first, int last) {
            cachedOnPublication = true;
            for (int row = first; row <= last; ++row)
                cachedOnPublication = cachedOnPublication && !fresh.data(fresh.index(row), VodCatalogModel::PosterRole).toString().isEmpty();
        });
        fresh.open(); QTRY_VERIFY(fresh.count() > 0); QTRY_VERIFY(!fresh.busy());
        QVERIFY(cachedOnPublication);
        QCOMPARE(catalogRequests, previousCatalogueRequests); QCOMPARE(imageRequests, previousImageRequests);
        fresh.close(); restarted.shutdown();
    }
}

void AppModelTests::vodArtworkPersistentConcurrentCache()
{
    using namespace OKILTV::Vod;
    QTemporaryDir dir;
    QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
    QImage image(100, 150, QImage::Format_RGB32); image.fill(Qt::blue);
    QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
    QList<QPointer<QTcpSocket>> replies;
    int requests = 0;
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (auto *socket = server.nextPendingConnection()) {
            auto received = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, received]() {
                received->append(socket->readAll());
                if (!received->contains("\r\n\r\n")) return;
                disconnect(socket, &QTcpSocket::readyRead, nullptr, nullptr);
                ++requests; replies.append(socket);
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    auto respond = [&]() {
        for (const auto &socket : std::exchange(replies, {})) {
            if (!socket) continue;
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: " + QByteArray::number(bytes.size()) + "\r\nConnection: close\r\n\r\n" + bytes);
            socket->disconnectFromHost();
        }
    };
    const auto source = Test::makeSource();
    const auto request = Test::requestFor(source);
    const auto profile = source.revision.profileId;
    const auto url = QUrl(QStringLiteral("http://127.0.0.1:%1/poster.png").arg(server.serverPort()));
    VodArtworkCache cache(dir.path());
    const auto ref = cache.remember(source, url, request); QVERIFY(ref);
    QVERIFY(!std::get<std::optional<ArtworkRef>>(cache.cached(profile, *ref, request)));
    QCOMPARE(requests, 0);
    auto first = QtConcurrent::run([&]() { return cache.resolve(profile, *ref, request); });
    QTRY_COMPARE(requests, 1);
    const auto secondRequest = Test::requestFor(source);
    auto second = QtConcurrent::run([&]() { return cache.resolve(profile, *ref, secondRequest); });
    const auto cancelledRequest = Test::requestFor(source);
    auto cancelled = QtConcurrent::run([&]() { return cache.resolve(profile, *ref, cancelledRequest); });
    cancelledRequest.cancelled->store(true);
    QTRY_VERIFY(cancelled.isFinished()); QCOMPARE(std::get<Error>(cancelled.result()).code, ErrorCode::Cancelled);
    QVERIFY(!first.isFinished());
    respond();
    QTRY_VERIFY(first.isFinished()); QTRY_VERIFY(second.isFinished());
    QVERIFY(std::holds_alternative<ArtworkRef>(first.result())); QVERIFY(std::holds_alternative<ArtworkRef>(second.result()));
    QCOMPARE(requests, 1);
    const auto local = std::get<ArtworkRef>(first.result()); QVERIFY(local.cachedPath);
    QCOMPARE(std::get<ArtworkRef>(second.result()).cachedPath, local.cachedPath);
    QFile file(*local.cachedPath); QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(file.setFileTime(QDateTime::currentDateTimeUtc().addDays(-1), QFileDevice::FileModificationTime)); file.close();
    VodArtworkCache reopened(dir.path());
    const auto cached = std::get<std::optional<ArtworkRef>>(reopened.cached(profile, *ref, request));
    QVERIFY(cached); QCOMPARE(cached->cachedPath, local.cachedPath);
    QVERIFY(QFileInfo(*local.cachedPath).lastModified() > QDateTime::currentDateTimeUtc().addSecs(-60));
    server.close();
    QVERIFY(std::holds_alternative<ArtworkRef>(reopened.resolve(profile, *ref, request))); QCOMPARE(requests, 1);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write("invalid image"); file.close();
    QVERIFY(!std::get<std::optional<ArtworkRef>>(reopened.cached(profile, *ref, request))); QVERIFY(!QFile::exists(*local.cachedPath));
    const auto changed = reopened.remember(source, QUrl(url.toString() + QStringLiteral("?version=2")), request);
    QVERIFY(changed); QVERIFY(changed->id != ref->id);
    QVERIFY(server.listen(QHostAddress::LocalHost, url.port()));
    auto repair = QtConcurrent::run([&]() { return reopened.resolve(profile, *ref, request); });
    QTRY_COMPARE(requests, 2); respond(); QTRY_VERIFY(repair.isFinished());
    QVERIFY(std::holds_alternative<ArtworkRef>(repair.result()));
    QVERIFY(QFile::remove(*local.cachedPath));
    auto removed = QtConcurrent::run([&]() { return reopened.resolve(profile, *ref, request); });
    QTRY_COMPARE(requests, 3);
    reopened.removeSource(profile); respond(); QTRY_VERIFY(removed.isFinished());
    QCOMPARE(std::get<Error>(removed.result()).code, ErrorCode::Cancelled);
    QVERIFY(!QFile::exists(*local.cachedPath));
    // A waiter can time out independently, and cancellation of the downloader
    // releases ownership so another caller can fetch and publish the same poster.
    const auto anotherSource = Test::makeSource();
    const auto ownerRequest = Test::requestFor(anotherSource);
    const auto anotherRef = reopened.remember(anotherSource, url, ownerRequest); QVERIFY(anotherRef);
    auto owner = QtConcurrent::run([&]() { return reopened.resolve(anotherSource.revision.profileId, *anotherRef, ownerRequest); });
    QTRY_COMPARE(requests, 4);
    auto shortRequest = Test::requestFor(anotherSource); shortRequest.deadline = QDeadlineTimer(50);
    auto timedOut = QtConcurrent::run([&]() { return reopened.resolve(anotherSource.revision.profileId, *anotherRef, shortRequest); });
    QTRY_VERIFY(timedOut.isFinished()); QCOMPARE(std::get<Error>(timedOut.result()).code, ErrorCode::Timeout);
    QVERIFY(!owner.isFinished()); QCOMPARE(requests, 4);
    const auto retryRequest = Test::requestFor(anotherSource);
    auto retry = QtConcurrent::run([&]() { return reopened.resolve(anotherSource.revision.profileId, *anotherRef, retryRequest); });
    ownerRequest.cancelled->store(true);
    QTRY_VERIFY(owner.isFinished()); QCOMPARE(std::get<Error>(owner.result()).code, ErrorCode::Cancelled);
    QTRY_COMPARE(requests, 5); respond(); QTRY_VERIFY(retry.isFinished());
    QVERIFY(std::holds_alternative<ArtworkRef>(retry.result())); QCOMPARE(requests, 5);
}

void AppModelTests::vodArtworkInvalidAndRemoved()
{
    using namespace OKILTV::Vod;
    QTemporaryDir dir; VodArtworkCache cache(dir.path());
    const auto source = Test::makeSource(); const auto request = Test::requestFor(source);
    QVERIFY(!cache.remember(source, QUrl(QStringLiteral("file:///tmp/anything")), request));
    const auto ref = cache.remember(source, QUrl(QStringLiteral("https://poster.invalid/p.jpg?token=secret")), request); QVERIFY(ref);
    const ArtworkRef invalid{QStringLiteral("../../secret"), QStringLiteral("poster"), {}};
    QCOMPARE(std::get<Error>(cache.resolve(source.revision.profileId, invalid, request)).code, ErrorCode::InvalidResponse);
    cache.removeSource(source.revision.profileId);
    QCOMPARE(std::get<Error>(cache.resolve(source.revision.profileId, *ref, request)).code, ErrorCode::Cancelled);
    QVERIFY(!cache.remember(source, QUrl(QStringLiteral("https://poster.invalid/p.jpg")), request));
    QVERIFY(!QDir(dir.filePath(source.revision.profileId.toString(QUuid::WithoutBraces))).exists());
}

void AppModelTests::vodLibraryWaitsForStorage_data()
{
    QTest::addColumn<bool>("series");
    QTest::addColumn<QString>("scenario");
    QTest::newRow("movies-retry") << false << QStringLiteral("retry");
    QTest::newRow("series-retry") << true << QStringLiteral("retry");
    QTest::newRow("series-timeout") << true << QStringLiteral("timeout");
    QTest::newRow("movies-close") << false << QStringLiteral("close");
}
void AppModelTests::vodLibraryWaitsForStorage()
{
    using namespace OKILTV::Vod;
    QFETCH(bool, series);
    QFETCH(QString, scenario);
    QTemporaryDir dir;
    QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (auto *socket = server.nextPendingConnection()) {
            auto bytes = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, socket, [socket, bytes]() {
                bytes->append(socket->readAll()); if (!bytes->contains("\r\n\r\n")) return;
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n[]");
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    SettingsManager settings(dir.filePath(QStringLiteral("settings.json"))); settings.load();
    ServerProfile profile; profile.xtreamBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    profile.xtreamUsername = QStringLiteral("synthetic"); profile.xtreamPassword = QStringLiteral("synthetic");
    QVERIFY(settings.addProfile(profile));
    const auto name = guidToString(QUuid::createUuid());
    const auto removeConnection = qScopeGuard([&]() { QSqlDatabase::removeDatabase(name); });
    auto writer = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    writer.setDatabaseName(dir.filePath(QStringLiteral("iptv.db"))); QVERIFY(writer.open());
    QSqlQuery lock(writer); QVERIFY(lock.exec(QStringLiteral("CREATE TABLE fixture(id INTEGER)")));
    QVERIFY(lock.exec(QStringLiteral("BEGIN EXCLUSIVE"))); // Reproduce contention during startup migration.
    PlayerController player; DvrController dvr(&settings, &player);
    MultiViewController multiview(&settings, nullptr, &player); TimeshiftController timeshift(&settings, &player, &dvr, &multiview);
    VodRuntime runtime(&settings, &multiview, &dvr, &timeshift, nullptr, []() { return std::make_unique<Test::Engine>(); });
    QSignalSpy errors(&runtime, &VodRuntime::errorOccurred);
    VodCatalogModel catalog(&runtime, &settings, nullptr, VodCatalogModel::Purpose::Library, series ? CatalogKind::Series : CatalogKind::Movies);
    runtime.synchronizeSource(guidToString(profile.id));
    QVERIFY(runtime.sourceSyncInProgress());
    QElapsedTimer waiting; waiting.start(); catalog.open();
    QVERIFY(catalog.busy()); QVERIFY(catalog.errorText().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 5000);
    QVERIFY(!runtime.sourceSyncInProgress());
    QVERIFY(!runtime.syncError(profile.id).isEmpty());
    QVERIFY(!runtime.ready()); QVERIFY(catalog.busy()); QVERIFY(catalog.errorText().isEmpty());
    if (scenario == QStringLiteral("timeout")) {
        QTRY_VERIFY_WITH_TIMEOUT(!catalog.busy(), 17000);
        QVERIFY(waiting.elapsed() >= 15000); QVERIFY(waiting.elapsed() < 17000);
        QVERIFY(catalog.errorText().contains(QStringLiteral("15 seconds")));
        QVERIFY(lock.exec(QStringLiteral("COMMIT")));
        catalog.refresh(); QVERIFY(catalog.busy()); QVERIFY(catalog.errorText().isEmpty());
    } else {
        if (scenario == QStringLiteral("close")) {
            catalog.close(); QVERIFY(!catalog.busy());
        }
        QVERIFY(lock.exec(QStringLiteral("COMMIT")));
        if (scenario == QStringLiteral("close")) catalog.open();
    }
    QTRY_VERIFY_WITH_TIMEOUT(runtime.ready(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(catalog.continueMoviesLoaded(), 5000);
    QTRY_VERIFY(!catalog.busy()); QVERIFY(catalog.errorText().isEmpty());
    QVERIFY(!runtime.active());
    catalog.close(); runtime.shutdown();
}
