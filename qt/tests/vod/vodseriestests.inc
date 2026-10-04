void AppModelTests::vodSeriesRuntime_data() {
    QTest::addColumn<QString>("scenario");
    for (const auto &scenario : {"probe", "season-tracks", "status", "autoplay", "completion-cache", "refresh", "back", "fast-end", "resume", "replace", "stop", "disable", "resolver-error", "storage-error", "premature", "unknown", "special", "shutdown", "dvr"})
        QTest::newRow(scenario) << QString::fromLatin1(scenario);
}
void AppModelTests::vodSeriesRuntime() {
    using namespace OKILTV::Vod;
    QFETCH(QString,scenario);
    QTemporaryDir dir;
    QByteArray media;
    QStringList mediaPaths;
    QPointer<QTcpSocket> stalledProbe;
    if (scenario == QStringLiteral("probe")) {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()
            || QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty()) QSKIP("ffmpeg/ffprobe required for episode probing");
        QFile subtitles(dir.filePath(QStringLiteral("subtitle.srt")));
        QVERIFY(subtitles.open(QIODevice::WriteOnly)); subtitles.write("1\n00:00:00,000 --> 00:00:01,000\nFixture subtitle\n"); subtitles.close();
        QProcess generator;
        generator.start(QStringLiteral("ffmpeg"), {QStringLiteral("-v"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("color=s=160x120:r=10:d=2"),
            QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=duration=2"),
            QStringLiteral("-i"), subtitles.fileName(),
            QStringLiteral("-map"), QStringLiteral("0:v"), QStringLiteral("-map"), QStringLiteral("1:a"),
            QStringLiteral("-map"), QStringLiteral("1:a"), QStringLiteral("-map"), QStringLiteral("2:s"),
            QStringLiteral("-metadata:s:a:0"), QStringLiteral("language=pol"),
            QStringLiteral("-metadata:s:a:1"), QStringLiteral("language=eng"),
            QStringLiteral("-metadata:s:s:0"), QStringLiteral("language=pol"),
            QStringLiteral("-c:a"), QStringLiteral("ac3"), QStringLiteral("-c:v"), QStringLiteral("mpeg2video"),
            dir.filePath(QStringLiteral("episode.mkv"))});
        QVERIFY(generator.waitForFinished(30000)); QCOMPARE(generator.exitCode(), 0);
        QFile file(dir.filePath(QStringLiteral("episode.mkv"))); QVERIFY(file.open(QIODevice::ReadOnly)); media = file.readAll();
    }
    QTcpServer server;QVERIFY(server.listen(QHostAddress::LocalHost));
    bool failResolver=false, newEpisode=scenario==QStringLiteral("season-tracks");
    int seriesImports=0;
    connect(&server,&QTcpServer::newConnection,&server,[&]() {
        while (server.hasPendingConnections()) {
            auto *socket=server.nextPendingConnection();auto bytes=std::make_shared<QByteArray>();
            connect(socket,&QTcpSocket::readyRead,socket,[&,socket,bytes]() {
                bytes->append(socket->readAll());if (!bytes->contains("\r\n\r\n")) return;
                const QUrl target(QString::fromUtf8(bytes->split(' ').value(1)));
                const QUrlQuery query(target);
                if (target.path().startsWith(QStringLiteral("/series/"))) {
                    mediaPaths.append(target.path());
                    if (target.path().endsWith(QStringLiteral("/303.mp4"))) {
                        stalledProbe = socket;
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 1000000\r\n\r\n"); return;
                    }
                    const auto body = target.path().endsWith(QStringLiteral("/304.mp4")) ? QByteArray("invalid media") : media;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost(); return;
                }
                const auto action=query.queryItemValue(QStringLiteral("action"));QByteArray body("[]");
                if (action==QStringLiteral("get_series_categories")) body=R"([{"category_id":"a","category_name":"Series"}])";
                if (action==QStringLiteral("get_series")) { ++seriesImports;body=R"([{"series_id":"7","name":"Fixture series","category_id":"a"}])"; }
                if (action==QStringLiteral("get_series_info")) body=failResolver ? QByteArray("invalid") : QByteArray(R"({"info":{"name":"Fixture series","plot":"Series details"},"seasons":[{"season_number":0},{"season_number":1},{"season_number":3}],"episodes":{"0":[{"id":"100","title":"Special","season":0,"episode_num":1,"container_extension":"mp4","info":{"duration_secs":120}}],"1":[{"id":"101","title":"First","season":1,"episode_num":1,"container_extension":"mp4","info":{"duration_secs":120}}],"3":[{"id":"303","title":"Next","season":3,"episode_num":3,"container_extension":"mp4","info":{"duration_secs":120}},{"id":"304","title":"Last","season":3,"episode_num":4,"container_extension":"mp4","info":{"duration_secs":120}}]}})");
                if (action==QStringLiteral("get_series_info") && newEpisode && !failResolver) {
                    auto root=QJsonDocument::fromJson(body).object();auto groups=root.value(QStringLiteral("episodes")).toObject();auto season=groups.value(QStringLiteral("3")).toArray();
                    season.append(QJsonObject{{QStringLiteral("id"),QStringLiteral("305")},{QStringLiteral("title"),QStringLiteral("New episode")},{QStringLiteral("season"),3},{QStringLiteral("episode_num"),5},{QStringLiteral("container_extension"),QStringLiteral("mp4")}});
                    groups.insert(QStringLiteral("3"),season);root.insert(QStringLiteral("episodes"),groups);body=QJsonDocument(root).toJson();
                }
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);socket->disconnectFromHost();
            });connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        }
    });
    SettingsManager settings(dir.filePath(QStringLiteral("settings.json")));settings.load();
    ServerProfile profile;profile.xtreamBaseUrl=QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());profile.xtreamUsername=QStringLiteral("synthetic");profile.xtreamPassword=QStringLiteral("synthetic");QVERIFY(settings.addProfile(profile));
    QVERIFY(!settings.current().vodSeriesEnabled); // The old global flag no longer blocks the saved enabled source.
    PlayerController player;DvrController dvr(&settings,&player);MultiViewController multiview(&settings,nullptr,&player);TimeshiftController timeshift(&settings,&player,&dvr,&multiview);
    Test::Engine *engine=nullptr;
    VodRuntime runtime(&settings,&multiview,&dvr,&timeshift,nullptr,[&]() {auto value=std::make_unique<Test::Engine>();engine=value.get();return value;});
    QTRY_VERIFY(runtime.ready());runtime.synchronizeSource(guidToString(profile.id));QTRY_VERIFY(!runtime.syncing(profile.id));QCOMPARE(seriesImports,1);QVERIFY(runtime.syncError(profile.id).isEmpty());
    const auto source=std::get<SourceContext>(runtime.store()->snapshot(profile.id));RequestContext context;context.source=source.revision;
    const ContentRef series{profile.id,source.revision.catalogNamespace,ContentKind::Series,QStringLiteral("7"),{}};
    ContentRef first{profile.id,source.revision.catalogNamespace,ContentKind::Episode,scenario==QStringLiteral("special") ? QStringLiteral("100") : scenario==QStringLiteral("completion-cache") ? QStringLiteral("304") : QStringLiteral("101"),series.providerItemId};
    auto *episodes=qobject_cast<VodEpisodesModel *>(runtime.episodesObject());QVERIFY(episodes);
    QSignalSpy errors(&runtime,&VodRuntime::errorOccurred);
    if (scenario == QStringLiteral("season-tracks")) {
        runtime.module()->controller()->details(series);
        QTRY_VERIFY(std::get<std::optional<VodDetails>>(runtime.store()->readDetails(series, context)).has_value());
        auto storeProbe = [&](QString id, QString language, int ageSeconds) {
            ContentRef ref{series.profileId, series.catalogNamespace, ContentKind::Episode, id, series.providerItemId};
            auto details = std::get<std::optional<VodDetails>>(runtime.store()->readDetails(ref, context));
            QVERIFY(details);
            VodMediaProbe metadata; metadata.observedAtUtc = QDateTime::currentDateTimeUtc().addSecs(-ageSeconds);
            metadata.videoWidth = 1920; metadata.videoHeight = 1080;
            metadata.audioTracks = {{0, 0, QStringLiteral("audio"), QStringLiteral("aac"), {}, language},
                {1, 1, QStringLiteral("audio"), QStringLiteral("aac"), {}, QStringLiteral("pol")}};
            metadata.subtitleTracks = {{2, 0, QStringLiteral("sub"), QStringLiteral("srt"), {}, language}};
            details->mediaProbe = metadata;
            QVERIFY(std::holds_alternative<Success>(runtime.store()->storeDetails(*details, context)));
        };
        storeProbe(QStringLiteral("101"), QStringLiteral("eng"), 20);
        storeProbe(QStringLiteral("304"), QStringLiteral("fra"), 0);
        storeProbe(QStringLiteral("305"), QStringLiteral("spa"), 10);
        VodCatalogModel catalog(&runtime, &settings, nullptr, VodCatalogModel::Purpose::Library, CatalogKind::Series);
        catalog.open(); QTRY_COMPARE(catalog.count(), 1); catalog.selectMovie(0);
        auto *library = qobject_cast<VodEpisodesModel *>(catalog.episodesObject()); QVERIFY(library);
        QTRY_VERIFY(catalog.movie().value(QStringLiteral("trackOptionsEditable")).toBool());
        QTRY_VERIFY(catalog.movie().value(QStringLiteral("progressLoaded")).toBool());
        QTRY_VERIFY(!catalog.busy());
        const auto summary = catalog.movie();
        library->selectSeason(QStringLiteral("3"));
        QVERIFY(!catalog.busy());
        QTRY_COMPARE(catalog.movie().value(QStringLiteral("audioTrackOptions")).toList().size(), 3);
        QTRY_VERIFY(catalog.movie().value(QStringLiteral("audioTrackOptions")).toList().at(1).toMap().value(QStringLiteral("label")).toString().contains(QStringLiteral("FRA")));
        QVERIFY(!catalog.movie().value(QStringLiteral("mediaProbeReady")).toBool());
        QVERIFY(!catalog.movie().value(QStringLiteral("trackOptionsEditable")).toBool());
        QTRY_VERIFY(catalog.movie().value(QStringLiteral("progressLoaded")).toBool());
        catalog.selectAudioOption(2); catalog.selectSubtitleOption(2);
        QCOMPARE(catalog.movie().value(QStringLiteral("audioTrackIndex")).toInt(), 0);
        QCOMPARE(catalog.movie().value(QStringLiteral("subtitleTrackIndex")).toInt(), 0);
        for (const auto &key : {"title", "poster", "description", "cast", "genres", "resolution", "resolutionLabel", "durationMinutes"})
            QCOMPARE(catalog.movie().value(QString::fromLatin1(key)), summary.value(QString::fromLatin1(key)));
        library->selectEpisode(2);
        QTRY_VERIFY(catalog.movie().value(QStringLiteral("trackOptionsEditable")).toBool());
        QVERIFY(catalog.movie().value(QStringLiteral("audioTrackOptions")).toList().at(1).toMap().value(QStringLiteral("label")).toString().contains(QStringLiteral("SPA")));
        library->selectSeason(QStringLiteral("0"));
        QTRY_VERIFY(catalog.movie().value(QStringLiteral("progressLoaded")).toBool());
        QVERIFY(catalog.movie().value(QStringLiteral("audioTrackOptions")).toList().isEmpty());
        library->selectSeason(QStringLiteral("3")); library->selectSeason(QStringLiteral("1")); library->selectSeason(QStringLiteral("3"));
        QTRY_COMPARE(library->selectedRef().providerItemId, QStringLiteral("303"));
        QTRY_VERIFY(catalog.movie().value(QStringLiteral("audioTrackOptions")).toList().size() == 3);
        QTRY_VERIFY(catalog.movie().value(QStringLiteral("progressLoaded")).toBool());
        QVERIFY(!catalog.movie().value(QStringLiteral("trackOptionsEditable")).toBool());
        QVERIFY(mediaPaths.isEmpty()); QVERIFY(!engine); QVERIFY(!catalog.probePlayBlocked());
        catalog.play(); QTRY_VERIFY(engine && engine->loads == 1);
        QVERIFY(engine->request.trackPreferences.isEmpty());
        const ContentRef unprobed{series.profileId, series.catalogNamespace, ContentKind::Episode, QStringLiteral("303"), series.providerItemId};
        QVERIFY(!std::get<std::optional<VodDetails>>(runtime.store()->readDetails(unprobed, context))->mediaProbe);
        runtime.shutdown(); return;
    }
    if (scenario == QStringLiteral("probe")) {
        VodCatalogModel catalog(&runtime, &settings, nullptr, VodCatalogModel::Purpose::Library, CatalogKind::Series);
        catalog.open(); QTRY_COMPARE(catalog.count(), 1);
        QCOMPARE(mediaPaths.size(), 0); QVERIFY(!engine); // Browsing does not probe or play.
        catalog.selectMovie(0);
        auto *library = qobject_cast<VodEpisodesModel *>(catalog.episodesObject()); QVERIFY(library);
        QTRY_COMPARE(library->selectedRef(), first);
        QTRY_VERIFY(catalog.movie().value(QStringLiteral("mediaProbeReady")).toBool());
        QTRY_VERIFY(catalog.movie().value(QStringLiteral("progressLoaded")).toBool());
        QCOMPARE(catalog.movie().value(QStringLiteral("audioTrackOptions")).toList().size(), 3);
        QCOMPARE(catalog.movie().value(QStringLiteral("subtitleTrackOptions")).toList().size(), 3);
        QVERIFY(!catalog.probePlayBlocked()); QVERIFY(!runtime.active()); QVERIFY(!engine);
        const auto cached = std::get<std::optional<VodDetails>>(runtime.store()->readDetails(first, context));
        QVERIFY(cached && cached->mediaProbe); QCOMPARE(cached->mediaProbe->audioTracks.size(), 2);
        const auto probedRequests = mediaPaths.size();
        catalog.back(); catalog.selectMovie(0);
        QTRY_VERIFY(catalog.movie().value(QStringLiteral("mediaProbeReady")).toBool());
        QCOMPARE(mediaPaths.size(), probedRequests); // Fresh episode metadata is reused.
        library->selectSeason(QStringLiteral("3"));
        QTRY_VERIFY(!catalog.busy());
        QCOMPARE(mediaPaths.size(), probedRequests); // Later seasons never probe media.
        QVERIFY(!stalledProbe);
        QVERIFY(!catalog.probePlayBlocked());
        QVERIFY(!catalog.movie().value(QStringLiteral("mediaProbeLoading")).toBool());
        QVERIFY(!catalog.movie().value(QStringLiteral("mediaProbeReady")).toBool());
        library->selectEpisode(1);
        QTRY_VERIFY(!catalog.busy());
        QCOMPARE(mediaPaths.size(), probedRequests);
        library->selectSeason(QStringLiteral("0"));
        QTRY_VERIFY(!catalog.busy());
        QCOMPARE(mediaPaths.size(), probedRequests); // Specials also do not probe.
        library->selectSeason(QStringLiteral("1"));
        QTRY_VERIFY(catalog.movie().value(QStringLiteral("mediaProbeReady")).toBool());
        QVERIFY(!catalog.probePlayBlocked());
        // Stale cached metadata triggers a new probe, and cancellation fences late results.
        auto stale = *cached; stale.mediaProbe->observedAtUtc = QDateTime::currentDateTimeUtc().addDays(-8);
        QVERIFY(std::holds_alternative<Success>(runtime.store()->storeDetails(stale, context)));
        const auto beforeStale = mediaPaths.size();
        catalog.back(); catalog.selectMovie(0);
        QTRY_VERIFY(mediaPaths.size() > beforeStale);
        QTRY_VERIFY(!catalog.movie().value(QStringLiteral("mediaProbeLoading")).toBool());
        catalog.selectAudioOption(2); catalog.selectSubtitleOption(2); catalog.play();
        QTRY_VERIFY(engine && engine->loads == 1);
        QCOMPARE(engine->request.trackPreferences.value(QStringLiteral("audio")).toObject().value(QStringLiteral("lang")).toString(), QStringLiteral("eng"));
        QCOMPARE(engine->request.trackPreferences.value(QStringLiteral("sub")).toObject().value(QStringLiteral("lang")).toString(), QStringLiteral("pol"));
        const auto beforePlaybackBrowse = mediaPaths.size();
        library->selectSeason(QStringLiteral("3"));
        QTRY_VERIFY(!catalog.busy()); QCOMPARE(mediaPaths.size(), beforePlaybackBrowse);
        runtime.stop();
        library->selectEpisode(1);
        QTRY_VERIFY(!catalog.busy());
        QCOMPARE(mediaPaths.size(), beforePlaybackBrowse);
        QVERIFY(catalog.movie().value(QStringLiteral("mediaProbeError")).toString().isEmpty());
        QVERIFY(!catalog.probePlayBlocked());
        catalog.play(); QTRY_COMPARE(engine->loads, 2); // Unprobed episodes remain playable.
        runtime.shutdown(); return;
    }
    if (scenario==QStringLiteral("completion-cache")) {
        // History survives cache eviction; playback must restore episode links
        // before an immediate Watched checkpoint can clear the series list.
        for (const auto &id : {QStringLiteral("101"),QStringLiteral("303")}) {
            auto ref=first;ref.providerItemId=id;VodProgress progress;
            progress.sessionToken=QUuid::createUuid();progress.sequence=1;progress.status=WatchStatus::Watched;
            progress.positionMs=120000;progress.durationMs=120000;progress.playbackCheckpoint=false;
            QVERIFY(std::holds_alternative<Success>(runtime.store()->beginSession(ref,progress.sessionToken,context)));
            QVERIFY(std::holds_alternative<Success>(runtime.store()->checkpoint(ref,progress,context,true)));
        }
        QVERIFY(std::holds_alternative<MovieListState>(runtime.store()->setMovieList(series,MovieList::ToWatch,true,context)));
        QVERIFY(!std::get<std::optional<VodDetails>>(runtime.store()->readDetails(series,context)));
    }
    runtime.module()->controller()->play(first);
    QTRY_VERIFY(engine && engine->loads==1);engine->emitState(OKILTV::Player::EngineState::Playing,scenario==QStringLiteral("premature") ? 40000 : scenario==QStringLiteral("resume") ? 73000 : 119000,scenario==QStringLiteral("unknown") ? std::nullopt : std::optional<qint64>(120000));
    if (scenario!=QStringLiteral("fast-end")) QTRY_VERIFY(!episodes->busy());
    QVERIFY(runtime.activeSeries());
    if (scenario==QStringLiteral("resume")) {
        runtime.stop();
        bool flushed=false;runtime.module()->progressService()->flushSource(profile.id,[&](Outcome result) {flushed=std::holds_alternative<Success>(result);});QTRY_VERIFY(flushed);
        QCOMPARE(std::get<SeriesProgress>(runtime.store()->readSeriesProgress(series,context)).lastEpisode,std::optional<ContentRef>(first));
        runtime.shutdown();
        Test::Engine *restored=nullptr;
        VodRuntime restarted(&settings,&multiview,&dvr,&timeshift,nullptr,[&]() {auto value=std::make_unique<Test::Engine>();restored=value.get();return value;});
        QTRY_VERIFY(restarted.ready());restarted.module()->controller()->play(series);
        QTRY_VERIFY(restored && restored->loads==1);QCOMPARE(restarted.module()->session()->snapshot().ref,first);
        restored->emitState(OKILTV::Player::EngineState::Loaded,0,120000,true);
        QTRY_COMPARE(restored->seekPosition,qint64(68000));
        restarted.shutdown();return;
    }
    if (scenario==QStringLiteral("completion-cache")) {
        QTRY_VERIFY(!std::get<MovieListState>(runtime.store()->readMovieLists(series,context)).toWatch);
        engine->emitEnd(engine->token,OKILTV::Player::EndReason::NaturalEnd);
        QTRY_VERIFY(runtime.libraryReturnPending());QCOMPARE(engine->loads,1);
        runtime.shutdown();return;
    }
    if (scenario == QStringLiteral("status")) {
        VodCatalogModel catalog(&runtime, &settings, nullptr, VodCatalogModel::Purpose::Library, CatalogKind::Series);
        catalog.open(); QTRY_COMPARE(catalog.rowCount(), 1); catalog.selectMovie(0);
        auto *library = qobject_cast<VodEpisodesModel *>(catalog.episodesObject()); QVERIFY(library);
        QTRY_VERIFY(library->statusReady()); QTRY_VERIFY(episodes->statusReady());
        QAbstractItemModelTester modelTester(library, QAbstractItemModelTester::FailureReportingMode::QtTest);
        const auto selected = library->selectedRef(); const auto season = library->seasonId();
        QSignalSpy resets(library, &QAbstractItemModel::modelReset);
        QSignalSpy changes(library, &QAbstractItemModel::dataChanged);
        QSignalSpy selections(library, &VodEpisodesModel::selectionChanged);
        auto history = [&]() { return std::get<SeriesProgress>(runtime.store()->readSeriesProgress(series, context)); };
        auto key = [&](const QString &id) { auto ref = first; ref.providerItemId = id; return QString::fromLatin1(ref.key().toHex()); };
        catalog.toggleWatched();
        QTRY_VERIFY(library->statusReady() && !library->statusBusy() && library->allWatched());
        QTRY_VERIFY(episodes->allWatched());
        QVERIFY(catalog.movie().value(QStringLiteral("watched")).toBool());
        for (const auto &item : library->details().episodes) QCOMPARE(history().episodes.value(item.ref.key()).status, WatchStatus::Watched);
        QVERIFY(history().continuationHidden); QCOMPARE(history().lastEpisode, std::optional<ContentRef>(first));
        library->toggleEpisodeWatched(key(QStringLiteral("100")));
        QTRY_VERIFY(library->statusReady() && !library->statusBusy());
        QCOMPARE(history().episodes.value(QByteArray::fromHex(key(QStringLiteral("100")).toLatin1())).status, WatchStatus::InProgress);
        QVERIFY(library->allWatched()); // Specials are not part of the series aggregate.
        episodes->toggleEpisodeWatched(key(QStringLiteral("101")));
        QTRY_VERIFY(library->statusReady() && !library->statusBusy() && !library->allWatched());
        QVERIFY(!catalog.movie().value(QStringLiteral("watched")).toBool());
        engine->emitState(OKILTV::Player::EngineState::Playing, 100000, 120000);
        bool flushed = false; runtime.module()->progressService()->flushSource(profile.id, [&](Outcome result) { flushed = std::holds_alternative<Success>(result); }); QTRY_VERIFY(flushed);
        QCOMPARE(history().episodes.value(first.key()).positionMs, qint64(0));
        QVERIFY(history().continuationHidden); QCOMPARE(engine->loads, 1);
        QTRY_VERIFY(library->statusReady()); catalog.toggleWatched();
        QTRY_VERIFY(library->statusReady() && !library->statusBusy() && library->allWatched()); catalog.toggleWatched();
        QTRY_VERIFY(library->statusReady() && !library->statusBusy() && !library->allWatched());
        for (const auto &item : library->details().episodes) {
            const auto progress = history().episodes.value(item.ref.key());
            QCOMPARE(progress.status, WatchStatus::InProgress); QCOMPARE(progress.positionMs, qint64(0));
        }
        QCOMPARE(library->selectedRef(), selected); QCOMPARE(library->seasonId(), season);
        QCOMPARE(resets.size(), 0); QCOMPARE(selections.size(), 0); QVERIFY(changes.size() > 0);
        runtime.module()->controller()->play(first); QTRY_COMPARE(engine->loads, 2);
        engine->emitState(OKILTV::Player::EngineState::Playing, 20000, 120000);
        flushed = false; runtime.module()->progressService()->flushSource(profile.id, [&](Outcome result) { flushed = std::holds_alternative<Success>(result); }); QTRY_VERIFY(flushed);
        QVERIFY(!history().continuationHidden);
        runtime.shutdown(); return;
    }
    if (scenario==QStringLiteral("refresh")) {
        VodCatalogModel catalog(&runtime,&settings,nullptr,VodCatalogModel::Purpose::Library,CatalogKind::Series);
        catalog.open();QTRY_COMPARE(catalog.rowCount(),1);catalog.selectMovie(0);
        auto *libraryEpisodes=qobject_cast<VodEpisodesModel *>(catalog.episodesObject());QVERIFY(libraryEpisodes);
        QTRY_VERIFY(!libraryEpisodes->busy() && libraryEpisodes->selectedRef().valid());
        QCOMPARE(libraryEpisodes->details().episodes.size(),4);
        const auto selected=libraryEpisodes->selectedRef();
        QSignalSpy selections(libraryEpisodes,&VodEpisodesModel::selectionChanged);
        libraryEpisodes->selectEpisode(0);QCOMPARE(selections.size(),0); // Enter preserves current pre-play preferences.
        newEpisode=true;catalog.refresh();
        QTRY_VERIFY(!runtime.syncing(profile.id));
        QTRY_COMPARE(libraryEpisodes->details().episodes.size(),5);
        QCOMPARE(libraryEpisodes->selectedRef(),selected);QCOMPARE(engine->loads,1);
        runtime.shutdown();return;
    }
    if (scenario==QStringLiteral("back")) {
        engine->acknowledgeStop=false;
        runtime.returnToSeriesLibrary();
        QVERIFY(!runtime.libraryReturnPending());
        engine->emitEnd(engine->token,OKILTV::Player::EndReason::UserStop);
        QTRY_VERIFY(runtime.libraryReturnPending());
        QSignalSpy returned(&runtime,&VodRuntime::libraryRequested);
        runtime.deliverLibraryReturn();QCOMPARE(returned.size(),1);
        QVERIFY(!runtime.active());runtime.shutdown();return;
    }
    const auto token=engine->token;
    const auto previousSession=runtime.module()->session()->snapshot().sessionToken;
    // Target preferences take precedence; absent preferences inherit confirmed tracks from the current series.
    OKILTV::Player::PlaybackEvent tracks;tracks.loadToken=engine->token;tracks.state=OKILTV::Player::EngineState::Playing;tracks.positionMs=119000;tracks.durationMs=scenario==QStringLiteral("unknown") ? std::nullopt : std::optional<qint64>(120000);
    tracks.trackPreferences=QJsonObject{{QStringLiteral("sub"),QJsonObject{{QStringLiteral("mode"),QStringLiteral("off")}}}};
    if (scenario!=QStringLiteral("premature")) engine->listener(tracks);
    if (scenario==QStringLiteral("resolver-error")) failResolver=true;
    if (scenario==QStringLiteral("storage-error")) {
        const auto name=QUuid::createUuid().toString();
        {
            auto db=QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),name);db.setDatabaseName(dir.filePath(QStringLiteral("iptv.db")));QVERIFY(db.open());
            QSqlQuery query(db);QVERIFY(query.exec(QStringLiteral("CREATE TRIGGER fail_series_checkpoint BEFORE UPDATE OF position ON vod_progress WHEN NEW.kind=2 BEGIN SELECT RAISE(ABORT,'synthetic'); END")));
        }
        QSqlDatabase::removeDatabase(name);
    }
    engine->emitEnd(token,OKILTV::Player::EndReason::NaturalEnd);
    engine->emitEnd(token,OKILTV::Player::EndReason::NaturalEnd); // Duplicate end must not queue a second transition.
    if (scenario==QStringLiteral("replace")) {
        auto target=first;target.providerItemId=QStringLiteral("304");runtime.module()->controller()->play(target);
        QTRY_COMPARE(engine->loads,2);QCOMPARE(runtime.module()->session()->snapshot().ref,target);
        engine->emitEnd(token,OKILTV::Player::EndReason::NaturalEnd);QTest::qWait(100);QCOMPARE(engine->loads,2);
        QVERIFY(!runtime.libraryReturnPending());runtime.shutdown();return;
    }
    if (scenario==QStringLiteral("stop")) runtime.stop();
    else if (scenario==QStringLiteral("disable")) {auto disabled=*settings.profileById(profile.id);disabled.vodEnabled=false;QVERIFY(settings.replaceProfile(profile.id,disabled));}
    else if (scenario==QStringLiteral("shutdown")) runtime.shutdown();
    else if (scenario==QStringLiteral("dvr")) {bool prepared=false;runtime.module()->prepareRecording([&](Outcome result) {prepared=std::holds_alternative<Success>(result);});QTRY_VERIFY(prepared);}
    if (scenario==QStringLiteral("autoplay") || scenario==QStringLiteral("fast-end") || scenario==QStringLiteral("unknown")) {
        QTRY_COMPARE(engine->loads,2);QCOMPARE(runtime.module()->session()->snapshot().ref.providerItemId,QStringLiteral("303"));QVERIFY(runtime.module()->session()->snapshot().sessionToken!=previousSession);
        QCOMPARE(engine->request.initialPositionMs,std::optional<qint64>{});
        if (scenario==QStringLiteral("unknown")) {
            const auto progress=std::get<std::optional<VodProgress>>(runtime.store()->read(first,context));
            QVERIFY(progress);QCOMPARE(progress->status,WatchStatus::InProgress);QVERIFY(!progress->durationMs);
        }
        QCOMPARE(engine->request.trackPreferences.value(QStringLiteral("sub")).toObject().value(QStringLiteral("mode")).toString(),QStringLiteral("off"));
        engine->emitEnd(token,OKILTV::Player::EndReason::NaturalEnd);QCOMPARE(engine->loads,2);
        engine->emitState(OKILTV::Player::EngineState::Playing,119000);QTRY_VERIFY(!episodes->busy());
        QTRY_VERIFY(!runtime.episodeTransition());
        QVERIFY(runtime.hasPreviousEpisode());QVERIFY(runtime.hasNextEpisode());
        runtime.nextEpisode();QTRY_COMPARE(engine->loads,3);QCOMPARE(runtime.module()->session()->snapshot().ref.providerItemId,QStringLiteral("304"));
        engine->emitState(OKILTV::Player::EngineState::Playing,119000);QTRY_VERIFY(!episodes->busy());QVERIFY(!runtime.hasNextEpisode());
        engine->emitEnd(engine->token,OKILTV::Player::EndReason::NaturalEnd);QTRY_VERIFY(runtime.libraryReturnPending());QCOMPARE(engine->loads,3);QVERIFY(!runtime.active());
        QSignalSpy returned(&runtime,&VodRuntime::libraryRequested);runtime.deliverLibraryReturn();QCOMPARE(returned.size(),1);
    } else {
        QTRY_VERIFY(!runtime.episodeTransition());QTest::qWait(100);QCOMPARE(engine->loads,1);
        if (scenario==QStringLiteral("resolver-error") || scenario==QStringLiteral("storage-error")) QVERIFY(!errors.isEmpty());
        if (scenario==QStringLiteral("special")) QTRY_VERIFY(runtime.libraryReturnPending());
        else QVERIFY(!runtime.libraryReturnPending());
    }
    runtime.shutdown();
}
