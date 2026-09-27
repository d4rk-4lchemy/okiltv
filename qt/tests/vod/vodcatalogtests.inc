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
    int catalogRequests = 0, imageRequests = 0;
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (auto *socket = server.nextPendingConnection()) {
            auto request = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, request]() {
                request->append(socket->readAll()); if (!request->contains("\r\n\r\n")) return;
                const auto target = QUrl::fromEncoded(request->split(' ').value(1));
                const auto action = QUrlQuery(target).queryItemValue(QStringLiteral("action"));
                QByteArray body = target.path().startsWith(QStringLiteral("/movie/")) ? QByteArray(32, '\0') : QByteArray("[]");
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
    QVERIFY(!runtime.module()->enabled()); catalog.open();
    QTRY_COMPARE_WITH_TIMEOUT(catalog.count(), 100, 10000);
    QTRY_VERIFY(!catalog.busy());
    QVERIFY(catalog.hasMore()); QCOMPARE(catalog.categories().size(), 3); QCOMPARE(catalogRequests, 1);
    QVERIFY(!runtime.active()); QVERIFY(!backend);
    QVERIFY(!settings.current().vodEnabled); QVERIFY(!settings.profileById(profile.id)->vodEnabled);
    QCOMPARE(settings.current().activeProfileId, liveSource);
    QVERIFY(settingsFile.open(QIODevice::ReadOnly)); QCOMPARE(settingsFile.readAll(), before); settingsFile.close();
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
    QDirIterator files(dir.filePath(QStringLiteral("vod-artwork")), {QStringLiteral("*.url")}, QDir::Files, QDirIterator::Subdirectories);
    QVERIFY(files.hasNext()); QFile registry(files.next()); QVERIFY(registry.open(QIODevice::ReadOnly));
    const auto protectedBytes = registry.readAll(); QVERIFY(!protectedBytes.contains("synthetic-art-secret")); QVERIFY(!protectedBytes.contains("http://"));
    catalog.selectMovie(0); QTRY_VERIFY(!catalog.busy()); QCOMPARE(catalog.movie().value(QStringLiteral("description")).toString(), QStringLiteral("A local test movie."));
    QCOMPARE(catalog.movie().value(QStringLiteral("resolutionLabel")).toString(), QStringLiteral("4K"));
    QSignalSpy started(&catalog, &VodCatalogModel::playbackStarted);
    catalog.play(); QTRY_COMPARE(started.size(), 1); QCOMPARE(backend->loads, 1);
    backend->emitState(OKILTV::Player::EngineState::Playing, 65000);
    runtime.stop(); QTRY_VERIFY(!runtime.active());
    QTRY_COMPARE(catalog.data(catalog.index(0), VodCatalogModel::ProgressRole).toDouble(), 65000.0 / 120000.0);
    catalog.close(); catalog.open(); catalog.requestProgress(0);
    QTRY_COMPARE(catalog.data(catalog.index(0), VodCatalogModel::ProgressRole).toDouble(), 65000.0 / 120000.0);
    catalog.back(); catalog.selectMovie(0); QTRY_VERIFY(catalog.movie().value(QStringLiteral("resumeSeconds")).toInt() >= 60);
    QTRY_COMPARE(catalog.continueMovies().size(), 1);
    QTRY_COMPARE(catalog.continueMovies().first().toMap().value(QStringLiteral("resolutionLabel")).toString(), QStringLiteral("4K"));
    QCOMPARE(catalog.categories().first().toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Continue watching"));
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
        restored.toggleWatched(); QTRY_VERIFY(!restored.busy());
        QCOMPARE(std::get<std::optional<VodProgress>>(runtime.store()->read(savedRef, request))->status, WatchStatus::Watched);
        restored.back(); restored.selectMovie(0); QTRY_VERIFY(!restored.busy());
        QVERIFY(restored.movie().value(QStringLiteral("watched")).toBool());
        restored.toggleWatched(); QTRY_VERIFY(!restored.busy());
        const auto reset = std::get<std::optional<VodProgress>>(runtime.store()->read(savedRef, request));
        QVERIFY(reset); QCOMPARE(reset->positionMs, qint64(0)); QCOMPARE(reset->status, WatchStatus::InProgress);
        restored.close();
    }
    catalog.close(); runtime.shutdown();
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
