// Source settings and synchronization use protected temporary storage and local HTTP only.
void AppModelTests::vodSourceGroupDrafts()
{
    QTemporaryDir dir;
    SettingsManager settings(dir.filePath(QStringLiteral("settings.json"))); settings.load();
    ServerProfile first, second; QVERIFY(settings.addProfile(first)); QVERIFY(settings.addProfile(second));
    QList<ChannelCategory> categories{{QStringLiteral("1"), QStringLiteral("One"), 0}, {QStringLiteral("2"), QStringLiteral("Two"), 0}};
    auto loader = [&](const QUuid &) { return categories; };
    SourceGroupsModel movies(&settings, nullptr, nullptr, QStringLiteral("movies"), loader);
    SourceGroupsModel series(&settings, nullptr, nullptr, QStringLiteral("series"), loader);
    movies.setAutoPersist(false); series.setAutoPersist(false);
    const auto firstId = guidToString(first.id), secondId = guidToString(second.id);
    movies.setProfileId(firstId); series.setProfileId(firstId);
    QTRY_COMPARE(movies.totalCount(), 2); QTRY_COMPARE(series.totalCount(), 2);
    QTRY_VERIFY(!movies.loading() && !series.loading());
    QVERIFY(movies.setGroupSelected(QStringLiteral("1"), false));
    movies.setSearchText(QStringLiteral("Two")); movies.setHideUnchecked(true);
    QVERIFY(series.setGroupSelected(QStringLiteral("2"), false));
    movies.setProfileId(secondId); QTRY_VERIFY(!movies.loading());
    QVERIFY(movies.deselectAll()); QVERIFY(movies.hasEmptyDraftSelection());
    movies.setProfileId(firstId); QTRY_VERIFY(!movies.loading());
    QCOMPARE(movies.searchText(), QStringLiteral("Two")); QCOMPARE(movies.selectedCount(), 1); QVERIFY(movies.hideUnchecked());
    categories.append({QStringLiteral("3"), QStringLiteral("Three"), 0});
    movies.reload(); QTRY_COMPARE(movies.totalCount(), 3); QTRY_VERIFY(!movies.loading());
    QCOMPARE(movies.selectedCount(), 2); QVERIFY(!movies.get(0).value(QStringLiteral("selected")).toBool());
    const auto savedPath = settings.settingsFilePath() + QStringLiteral(".held");
    QVERIFY(QFile::rename(settings.settingsFilePath(), savedPath)); QVERIFY(QDir().mkdir(settings.settingsFilePath()));
    QVERIFY(!movies.saveDraftChanges()); QVERIFY(movies.dirty()); QVERIFY(!movies.errorText().isEmpty());
    QVERIFY(QDir().rmdir(settings.settingsFilePath())); QVERIFY(QFile::rename(savedPath, settings.settingsFilePath()));
    QVERIFY(movies.saveDraftChanges()); QVERIFY(series.saveDraftChanges());
    SettingsManager restarted(settings.settingsFilePath()); restarted.load();
    QCOMPARE(restarted.current().hiddenGroupsByProfile.value(firstId + QStringLiteral("|movies")), QStringList{QStringLiteral("1")});
    QCOMPARE(restarted.current().hiddenGroupsByProfile.value(firstId + QStringLiteral("|series")), QStringList{QStringLiteral("2")});
    QVERIFY(restarted.current().hiddenGroupsByProfile.value(firstId).isEmpty());
    QCOMPARE(restarted.current().hiddenGroupsByProfile.value(secondId + QStringLiteral("|movies")).size(), 2);
    QVERIFY(movies.selectAll()); movies.discardDraftChanges(); QTRY_VERIFY(!movies.loading());
    QCOMPARE(movies.selectedCount(), 2); QVERIFY(!movies.dirty());
    // Large category discoveries retain previous picks and leave new categories unchecked.
    while (categories.size() < 51) categories.append({QString::number(categories.size() + 1), QStringLiteral("New"), 0});
    movies.reload(); QTRY_COMPARE(movies.totalCount(), 51); QTRY_VERIFY(!movies.loading());
    QCOMPARE(movies.selectedCount(), 2);
}

void AppModelTests::vodSourceSynchronizationAndDisable()
{
    using namespace OKILTV::Vod;
    QTemporaryDir dir;
    QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
    QStringList catalogRequests, seriesRequests;
    QString failCategory, failSeriesCategory;
    bool stall = false, delayRecovery = false;
    QByteArray recoveryBody;
    QPointer<QTcpSocket> stalled, recoverySocket;
    int categoryRequests = 0, detailRequests = 0;
    QJsonArray categories, rows, seriesRows;
    for (int i = 1; i <= 10; ++i) {
        categories.append(QJsonObject{{QStringLiteral("category_id"), QString::number(i)}, {QStringLiteral("category_name"), QStringLiteral("Category %1").arg(i)}});
        const QJsonObject row{{QStringLiteral("stream_id"), i}, {QStringLiteral("name"), QStringLiteral("Movie %1").arg(i)}, {QStringLiteral("category_id"), QString::number(i)}};
        rows.append(row); rows.append(row); // Provider duplicates are deduplicated.
        auto seriesRow=row;seriesRow.remove(QStringLiteral("stream_id"));seriesRow.insert(QStringLiteral("series_id"),i);
        seriesRows.append(seriesRow);seriesRows.append(seriesRow);
    }
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (server.hasPendingConnections()) {
            auto *socket = server.nextPendingConnection();
            auto bytes = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes]() {
                bytes->append(socket->readAll()); if (!bytes->contains("\r\n\r\n")) return;
                const auto target = QUrl(QString::fromUtf8(bytes->split(' ').value(1)));
                const QUrlQuery query(target); const auto action = query.queryItemValue(QStringLiteral("action"));
                QByteArray body("[]");
                if (action == QStringLiteral("get_vod_categories") || action == QStringLiteral("get_series_categories")) { ++categoryRequests; body = QJsonDocument(categories).toJson(); }
                if (action == QStringLiteral("get_vod_streams")) {
                    const auto category = query.queryItemValue(QStringLiteral("category_id")); catalogRequests.append(category);
                    if (stall) { stalled = socket; return; }
                    body = !failCategory.isEmpty() && category == failCategory ? QByteArray("invalid") : QJsonDocument(rows).toJson(); // Intentionally ignore category_id.
                }
                if (action == QStringLiteral("get_series")) {
                    const auto category=query.queryItemValue(QStringLiteral("category_id"));seriesRequests.append(category);
                    body=!failSeriesCategory.isEmpty() && category==failSeriesCategory ? QByteArray("invalid") : QJsonDocument(seriesRows).toJson();
                }
                if (action == QStringLiteral("get_vod_info")) {
                    ++detailRequests;
                    body = R"({"info":{},"movie_data":{"stream_id":"1","container_extension":"mp4"}})";
                    if (delayRecovery) { recoverySocket = socket; recoveryBody = body; return; }
                }
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    SettingsManager settings(dir.filePath(QStringLiteral("settings.json"))); settings.load();
    ServerProfile profile; profile.xtreamBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    profile.xtreamUsername = QStringLiteral("synthetic"); profile.xtreamPassword = QStringLiteral("synthetic");
    QVERIFY(settings.addProfile(profile)); QVERIFY(!settings.current().vodEnabled);
    PlayerController player; DvrController dvr(&settings, &player);
    MultiViewController multiview(&settings, nullptr, &player); TimeshiftController timeshift(&settings, &player, &dvr, &multiview);
    Test::Engine *engine = nullptr;
    VodRuntime runtime(&settings, &multiview, &dvr, &timeshift, nullptr, [&]() { auto backend = std::make_unique<Test::Engine>(); engine = backend.get(); return backend; });
    QTRY_VERIFY(runtime.ready());
    const auto id = guidToString(profile.id), key = id + QStringLiteral("|movies");
    runtime.synchronizeSource(id); QTRY_VERIFY(!runtime.syncing(profile.id));
    QCOMPARE(categoryRequests, 2); QCOMPARE(catalogRequests, QStringList{QString{}}); QVERIFY(runtime.syncError(profile.id).isEmpty());
    QCOMPARE(runtime.sourceCategories(profile.id, CatalogKind::Series).size(), 10);
    const auto source = std::get<SourceContext>(runtime.store()->snapshot(profile.id));
    const CatalogScope scope{profile.id, source.revision.catalogNamespace, CatalogKind::Movies, {}};
    RequestContext request; request.source = source.revision;
    CatalogQuery query; query.scope = scope;
    auto page = [&]() { return std::get<CatalogPage>(runtime.store()->query(query, request)); };
    QCOMPARE(page().items.size(), 10);
    CatalogQuery seriesQuery;seriesQuery.scope={profile.id,source.revision.catalogNamespace,CatalogKind::Series,{}};
    auto seriesPage=[&]() {return std::get<CatalogPage>(runtime.store()->query(seriesQuery,request));};
    QCOMPARE(seriesRequests,QStringList{QString{}});QCOMPARE(seriesPage().items.size(),10);
    auto selectSeries=[&](int count) {
        QStringList hidden;for (int i=count+1;i<=10;++i) hidden.append(QString::number(i));
        settings.current().hiddenGroupsByProfile[id+QStringLiteral("|series")]=hidden;settings.save();
        seriesRequests.clear();runtime.policyChanged(profile.id);
    };
    selectSeries(2);QTRY_VERIFY(!runtime.syncing(profile.id));
    QCOMPARE(seriesRequests,(QStringList{QStringLiteral("1"),QStringLiteral("2")}));QCOMPARE(seriesPage().items.size(),2);
    selectSeries(3);QTRY_VERIFY(!runtime.syncing(profile.id));QCOMPARE(seriesRequests,QStringList{QString{}});QCOMPARE(seriesPage().items.size(),3);
    selectSeries(0);QTRY_VERIFY(!runtime.syncing(profile.id));QVERIFY(seriesRequests.isEmpty());QVERIFY(seriesPage().items.isEmpty());
    selectSeries(4);QTRY_VERIFY(!runtime.syncing(profile.id));
    const auto seriesBeforeFailure=seriesPage().generation;
    const auto movieBeforeSeriesFailure=page().generation;
    failSeriesCategory=QStringLiteral("2");selectSeries(2);QTRY_VERIFY(!runtime.syncing(profile.id));
    QVERIFY(!runtime.syncError(profile.id).isEmpty());QCOMPARE(seriesPage().generation,seriesBeforeFailure);
    QVERIFY(page().generation>movieBeforeSeriesFailure);QCOMPARE(page().items.size(),10);
    failSeriesCategory.clear();selectSeries(4);QTRY_VERIFY(!runtime.syncing(profile.id));

    auto select = [&](int count) {
        QStringList hidden; for (int i = count + 1; i <= 10; ++i) hidden.append(QString::number(i));
        settings.current().hiddenGroupsByProfile[key] = hidden; settings.save(); catalogRequests.clear(); runtime.policyChanged(profile.id);
    };
    select(2); QTRY_VERIFY(!runtime.syncing(profile.id));
    QCOMPARE(catalogRequests, (QStringList{QStringLiteral("1"), QStringLiteral("2")})); QCOMPARE(page().items.size(), 2);
    select(3); QTRY_VERIFY(!runtime.syncing(profile.id)); QCOMPARE(catalogRequests, QStringList{QString{}}); QCOMPARE(page().items.size(), 3);
    select(4); QTRY_VERIFY(!runtime.syncing(profile.id)); QCOMPARE(catalogRequests, QStringList{QString{}}); QCOMPARE(page().items.size(), 4);
    const auto beforeFailure = page().generation;
    const auto seriesBeforeMovieFailure=seriesPage().generation;
    failCategory = QStringLiteral("2"); select(2); QTRY_VERIFY(!runtime.syncing(profile.id));
    QVERIFY(!runtime.syncError(profile.id).isEmpty()); QCOMPARE(page().generation, beforeFailure); QCOMPARE(page().items.size(), 4);
    QVERIFY(seriesPage().generation>seriesBeforeMovieFailure);QCOMPARE(seriesPage().items.size(),4);
    failCategory.clear(); select(0); QTRY_VERIFY(!runtime.syncing(profile.id)); QVERIFY(catalogRequests.isEmpty()); QVERIFY(page().items.isEmpty());
    select(4); QTRY_VERIFY(!runtime.syncing(profile.id));
    // SQL category filtering runs before pagination and special-list membership.
    query.allowedCategories = QStringList{QStringLiteral("4")}; query.pageSize = 1;
    QCOMPARE(std::get<MovieSummary>(page().items.first()).ref.providerItemId, QStringLiteral("4")); QVERIFY(!page().next);
    query.allowedCategories.reset(); query.pageSize = 100;
    const ContentRef ref{profile.id, source.revision.catalogNamespace, ContentKind::Movie, QStringLiteral("1"), {}};
    const auto play = runtime.module()->controller()->play(ref); Q_UNUSED(play);
    QTRY_VERIFY(engine && engine->loads == 1); engine->emitState(OKILTV::Player::EngineState::Playing, 10000); QTRY_VERIFY(runtime.isPlaying());
    const auto token = runtime.module()->session()->snapshot().sessionToken;
    // Deselecting the active movie's category keeps its token and transport.
    settings.current().hiddenGroupsByProfile[key] = {QStringLiteral("1")}; settings.save(); runtime.policyChanged(profile.id);
    QTRY_VERIFY(!runtime.syncing(profile.id)); QCOMPARE(engine->stops, 0); QCOMPARE(runtime.module()->session()->snapshot().sessionToken, token);
    stall = true; runtime.synchronizeSource(id); QTRY_VERIFY(stalled);
    const auto beforeDisable = page().generation;
    delayRecovery = true;
    bool pendingRecoveryCompleted = false;
    runtime.module()->controller()->resolveForRecovery(ref, [&](Result<PlaybackDescriptor> result) {
        QVERIFY(std::holds_alternative<PlaybackDescriptor>(result)); pendingRecoveryCompleted = true;
    });
    QTRY_VERIFY(recoverySocket);
    auto disabled = *settings.profileById(profile.id); const auto revision = disabled.vodCredentialRevision; disabled.vodEnabled = false;
    QVERIFY(settings.replaceProfile(profile.id, disabled)); QCOMPARE(settings.profileById(profile.id)->vodCredentialRevision, revision);
    QCOMPARE(engine->stops, 0); QVERIFY(runtime.active()); QTRY_VERIFY(!runtime.syncing(profile.id));
    QTRY_VERIFY(!stalled || stalled->state() == QAbstractSocket::UnconnectedState);
    QCOMPARE(page().generation, beforeDisable);
    QVERIFY(recoverySocket && recoverySocket->state() == QAbstractSocket::ConnectedState);
    recoverySocket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(recoveryBody.size()) + "\r\nConnection: close\r\n\r\n" + recoveryBody);
    recoverySocket->disconnectFromHost(); delayRecovery = false;
    QTRY_VERIFY(pendingRecoveryCompleted);
    const auto connections = catalogRequests.size();
    const auto details = detailRequests;
    VodCatalogModel library(&runtime, &settings); library.open(); QVERIFY(library.sources().isEmpty());
    runtime.module()->controller()->play(ref); runtime.synchronizeSource(id); QTest::qWait(200);
    QCOMPARE(catalogRequests.size(), connections); QCOMPARE(detailRequests, details); QCOMPARE(engine->loads, 1);
    runtime.togglePause(); QCOMPARE(engine->pauses, 1); engine->emitState(OKILTV::Player::EngineState::Paused, 20000);
    runtime.seekTo(25); QCOMPARE(engine->seekPosition, qint64(25000)); engine->emitSeekCompleted(25000);
    // The only disabled-source resolver exception belongs to this active token.
    bool recovered = false;
    runtime.module()->controller()->resolveForRecovery(ref, [&](Result<PlaybackDescriptor> result) { QVERIFY(std::holds_alternative<PlaybackDescriptor>(result)); recovered = true; });
    QTRY_VERIFY(recovered); QCOMPARE(runtime.module()->session()->snapshot().sessionToken, token);
    runtime.stop(); QTRY_VERIFY(!runtime.active());
    QTRY_VERIFY(std::get<std::optional<VodProgress>>(runtime.store()->read(ref, request)).has_value());
    QTRY_COMPARE(std::get<std::optional<VodProgress>>(runtime.store()->read(ref, request))->positionMs, qint64(25000));
    bool rejected = false;
    runtime.module()->controller()->resolveForRecovery(ref, [&](Result<PlaybackDescriptor> result) { rejected = std::holds_alternative<Error>(result); });
    QVERIFY(rejected);
    runtime.shutdown();
}
