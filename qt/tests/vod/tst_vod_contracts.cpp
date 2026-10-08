#include "fakes/vodfakes.h"
#include <QSemaphore>
#include <QScopeGuard>
#include <QtTest>

using namespace OKILTV::Vod;
using namespace OKILTV::Vod::Test;
using namespace OKILTV::Player;
class VodContractTests : public QObject {
    Q_OBJECT
private slots:
    void busyJobsRetryWithoutBlockingOtherSources() {
        VodJobRunner jobs(1, 1);
        RequestContext request;
        std::atomic_int attempts{0};
        std::atomic_bool identityMatches{true};
        int deliveries = 0;
        jobs.submit(QUuid::createUuid(), request, [&](RequestContext context) -> Result<JobReply> {
            if (context.operationId != request.operationId) identityMatches = false;
            if (++attempts < 3) return Error{ErrorCode::StorageBusy, context.operationId, true};
            return JobReply{{}, Success{}, {}};
        }, [&](Result<JobReply> result) { QVERIFY(std::holds_alternative<JobReply>(result)); ++deliveries; });
        bool otherFinished = false;
        int otherSawAttempts = 0;
        jobs.submit(QUuid::createUuid(), {}, [](RequestContext) -> Result<JobReply> { return JobReply{{}, Success{}, {}}; },
            [&](Result<JobReply> result) { QVERIFY(std::holds_alternative<JobReply>(result)); otherSawAttempts = attempts.load(); otherFinished = true; });
        QTRY_VERIFY(otherFinished);
        QVERIFY(otherSawAttempts < 3);
        QTRY_COMPARE(deliveries, 1);
        QCOMPARE(attempts.load(), 3); QVERIFY(identityMatches.load()); QCOMPARE(jobs.pending(), 0);
    }
    void busyJobsRespectCancellationAndDeadline_data() {
        QTest::addColumn<bool>("cancel");
        QTest::addColumn<bool>("sourceCancel");
        QTest::newRow("cancelled") << true << false;
        QTest::newRow("source-cancelled") << true << true;
        QTest::newRow("deadline") << false << false;
    }
    void busyJobsRespectCancellationAndDeadline() {
        QFETCH(bool, cancel);
        QFETCH(bool, sourceCancel);
        VodJobRunner jobs;
        const auto profile = QUuid::createUuid();
        RequestContext request; request.deadline = QDeadlineTimer(cancel ? 5000 : 150);
        std::atomic_int attempts{0};
        std::optional<Result<JobReply>> completed;
        jobs.submit(profile, request, [&](RequestContext context) -> Result<JobReply> {
            ++attempts; return Error{ErrorCode::StorageBusy, context.operationId, true};
        }, [&](Result<JobReply> result) { completed = std::move(result); });
        QTRY_VERIFY(attempts.load() > 0);
        if (sourceCancel) jobs.cancelSource(profile);
        else if (cancel) jobs.cancel(request.operationId);
        QTRY_VERIFY(completed.has_value());
        QVERIFY(std::holds_alternative<Error>(*completed));
        QCOMPARE(std::get<Error>(*completed).code, cancel ? ErrorCode::Cancelled : ErrorCode::Timeout);
        QCOMPARE(jobs.pending(), 0);
    }
    void shutdownDropsBusyRetries() {
        VodJobRunner jobs;
        std::atomic_int attempts{0};
        bool delivered = false;
        jobs.submit(QUuid::createUuid(), {}, [&](RequestContext context) -> Result<JobReply> {
            ++attempts; return Error{ErrorCode::StorageBusy, context.operationId, true};
        }, [&](Result<JobReply>) { delivered = true; });
        QTRY_VERIFY(attempts.load() > 0);
        jobs.shutdown();
        QTRY_COMPARE(jobs.pending(), 0);
        const auto count = attempts.load(); QTest::qWait(150);
        QCOMPARE(attempts.load(), count); QVERIFY(!delivered);
    }
    void permanentStorageFailureIsNotRetried() {
        VodJobRunner jobs;
        std::atomic_int attempts{0};
        bool completed = false;
        jobs.submit(QUuid::createUuid(), {}, [&](RequestContext context) -> Result<JobReply> {
            ++attempts; return Error{ErrorCode::StorageUnavailable, context.operationId};
        }, [&](Result<JobReply> result) {
            QCOMPARE(std::get<Error>(result).code, ErrorCode::StorageUnavailable); completed = true;
        });
        QTRY_VERIFY(completed); QCOMPARE(attempts.load(), 1);
    }
    void episodeTrackFallbackUsesCurrentConfirmation_data() {
        QTest::addColumn<bool>("savedTarget");
        QTest::newRow("current-confirmation") << false;
        QTest::newRow("saved-target") << true;
    }
    void episodeTrackFallbackUsesCurrentConfirmation() {
        QFETCH(bool,savedTarget);
        Fixture fixture;const auto request=requestFor(fixture.source);
        const ContentRef series{fixture.source.revision.profileId,fixture.source.revision.catalogNamespace,ContentKind::Series,QStringLiteral("series"),{}};
        const ContentRef previous{series.profileId,series.catalogNamespace,ContentKind::Episode,QStringLiteral("first"),series.providerItemId};
        auto target=previous;target.providerItemId=QStringLiteral("second");
        const QJsonObject oldChoice{{QStringLiteral("sub"),QJsonObject{{QStringLiteral("mode"),QStringLiteral("track")},{QStringLiteral("ordinal"),0}}}};
        const QJsonObject currentChoice{{QStringLiteral("sub"),QJsonObject{{QStringLiteral("mode"),QStringLiteral("off")}}}};
        VodDetails details;details.ref=series;details.episodes={{previous,series,QStringLiteral("1"),1,0,QStringLiteral("First"),Availability::Available},{target,series,QStringLiteral("1"),2,1,QStringLiteral("Second"),Availability::Available}};
        QVERIFY(std::holds_alternative<Success>(fixture.store->storeDetails(details,request)));
        auto save=[&](const ContentRef &ref) {
            VodProgress progress;progress.sessionToken=QUuid::createUuid();progress.sequence=1;progress.trackPreferences=oldChoice;
            QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(ref,progress.sessionToken,request)));
            QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(ref,progress,request)));
        };
        save(previous);if(savedTarget)save(target);
        VodModule module({true},[&](){return fixture.composition();});
        PlaybackPreferences options;options.inheritedTracks=currentChoice;
        module.controller()->play(target,options);
        QTRY_VERIFY(fixture.engine && fixture.engine->loads==1);
        QCOMPARE(fixture.engine->request.trackPreferences,savedTarget ? oldChoice : currentChoice);
        module.shutdown();
    }
    void movieListsCompletionAndRewatch()
    {
        Fixture fixture;
        VodProgressService service(fixture.dependencies());
        const auto ref = refFor(fixture.source);
        const auto request = requestFor(fixture.source);
        auto setList = [&](MovieList list) {
            bool done = false;
            service.setMovieList(ref, list, true, [&](Result<MovieListState> result) { QVERIFY(std::holds_alternative<MovieListState>(result)); done = true; });
            QTRY_VERIFY(done);
        };
        setList(MovieList::ToWatch); setList(MovieList::Favourites);
        SessionSnapshot snapshot; snapshot.ref = ref; snapshot.sessionToken = QUuid::createUuid(); snapshot.positionValid = true;
        snapshot.positionMs = 95000; snapshot.durationMs = 100000;
        service.observe(snapshot, true); QTRY_COMPARE(fixture.store->checkpoints.load(), 1);
        QVERIFY(std::get<MovieListState>(fixture.store->readMovieLists(ref, request)).toWatch);
        snapshot.positionMs = 95001;
        service.observe(snapshot, false); QTRY_COMPARE(fixture.store->checkpoints.load(), 2);
        QCOMPARE(std::get<MovieListState>(fixture.store->readMovieLists(ref, request)), (MovieListState{false, true}));
        setList(MovieList::ToWatch);
        snapshot.positionMs = 96000; service.observe(snapshot, true); QTRY_COMPARE(fixture.store->checkpoints.load(), 3);
        QVERIFY(std::get<MovieListState>(fixture.store->readMovieLists(ref, request)).toWatch);
        snapshot.sessionToken = QUuid::createUuid(); snapshot.positionMs = 0;
        service.observe(snapshot, true); QTRY_COMPARE(fixture.store->checkpoints.load(), 4);
        snapshot.positionMs = 99000; service.observe(snapshot, false); QTRY_COMPARE(fixture.store->checkpoints.load(), 5);
        QCOMPARE(std::get<MovieListState>(fixture.store->readMovieLists(ref, request)), (MovieListState{false, true}));
        setList(MovieList::ToWatch);
        bool marked = false;
        service.setWatched(ref, true, [&](Result<VodProgress> result) { QVERIFY(std::holds_alternative<VodProgress>(result)); marked = true; });
        QTRY_VERIFY(marked);
        QCOMPARE(std::get<MovieListState>(fixture.store->readMovieLists(ref, request)), (MovieListState{false, true}));
        setList(MovieList::ToWatch);
        marked = false;
        service.setWatched(ref, false, [&](Result<VodProgress> result) { QVERIFY(std::holds_alternative<VodProgress>(result)); marked = true; });
        QTRY_VERIFY(marked);
        snapshot.positionMs = 99999; service.observe(snapshot, true); service.flush();
        QVERIFY(service.movieListWritePending(ref) == false);
        service.shutdown();
        QCOMPARE(std::get<MovieListState>(fixture.store->readMovieLists(ref, request)), (MovieListState{true, true}));
    }
    void playbackWaitsForProbeTeardown_data()
    {
        QTest::addColumn<int>("action");
        QTest::newRow("play") << 0;
        QTest::newRow("cancel-during-teardown") << 1;
        QTest::newRow("cancel-during-cooldown") << 2;
        QTest::newRow("replace-during-cooldown") << 3;
        QTest::newRow("source-change-during-cooldown") << 4;
        QTest::newRow("shutdown-during-cooldown") << 5;
    }
    void playbackWaitsForProbeTeardown()
    {
        QFETCH(int, action);
        Fixture fixture;
        struct ProbeState {
            std::atomic_bool entered = false;
            std::atomic_bool cancelled = false;
            QSemaphore finish;
        };
        const auto probe = std::make_shared<ProbeState>();
        VodModule module({true}, [&]() {
            auto composition = fixture.composition();
            composition.dependencies.mediaProbe = [probe](const PlaybackDescriptor &, const RequestContext &context) -> Result<VodMediaProbe> {
                probe->entered = true;
                while (!context.interruption()) QThread::msleep(5);
                probe->cancelled = true;
                probe->finish.acquire(); // Simulate slow process teardown after cancellation.
                return Error{ErrorCode::Cancelled, context.operationId};
            };
            return composition;
        });
        const auto unblock = qScopeGuard([&]() { probe->finish.release(); });
        const auto ref = refFor(fixture.source);
        const auto probeId = module.controller()->probe(ref);
        QTRY_VERIFY(probe->entered.load());
        QElapsedTimer reaped;
        connect(module.controller(), &VodController::eventCompleted, module.controller(), [&](const VodEvent &event) {
            if (event.operationId == probeId) reaped.start();
        });
        module.controller()->play(ref);
        QTRY_VERIFY(probe->cancelled.load());
        if (action == 1) module.controller()->cancelPendingPlayback();
        if (action == 0) QTest::qWait(2100);
        QVERIFY(!fixture.engine); // A two-second timer from Play is insufficient.
        probe->finish.release();
        QTRY_VERIFY(reaped.isValid());
        if (action == 2) module.controller()->cancelPendingPlayback();
        if (action == 3) module.controller()->play(refFor(fixture.source, QStringLiteral("replacement")));
        if (action == 4) module.controller()->sourceChanged(ref.profileId);
        if (action == 5) module.controller()->shutdown();
        QTest::qWait(1700);
        QVERIFY(!fixture.engine);
        if (action == 0 || action == 3) {
            QTRY_VERIFY(fixture.engine);
            QVERIFY(reaped.elapsed() >= 2000);
            QCOMPARE(fixture.engine->loads, 1);
            if (action == 3) QCOMPARE(module.session()->snapshot().ref.providerItemId, QStringLiteral("replacement"));
        } else {
            QTest::qWait(500);
            QVERIFY(!fixture.engine);
        }
    }
    void pausedTelemetryCheckpointsOnlyOnTransition()
    {
        const auto source = makeSource();
        auto engine = std::make_unique<Engine>();
        auto *backend = engine.get();
        VodPlaybackSession session(std::move(engine));
        int checkpoints = 0;
        session.changed = [&](const SessionSnapshot &, bool checkpoint) { checkpoints += checkpoint ? 1 : 0; };
        QVERIFY(std::holds_alternative<Success>(session.open(
            descriptorFor(source, refFor(source)), QUuid::createUuid(), {})));
        backend->emitState(EngineState::Loaded);
        backend->emitState(EngineState::Playing, 45000);
        QCOMPARE(checkpoints, 0);
        session.pause();
        backend->emitState(EngineState::Paused, 45000);
        QCOMPARE(checkpoints, 1);
        for (int i = 0; i < 100; ++i) backend->emitState(EngineState::Paused, 45000);
        QCOMPARE(checkpoints, 1);

        PlaybackEvent change;
        change.loadToken = backend->token;
        change.state = EngineState::Paused;
        change.positionMs = 45000;
        change.trackPreferences = QJsonObject{{QStringLiteral("sub"),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("off")}}}};
        backend->listener(change);
        QCOMPARE(checkpoints, 2);
        backend->listener(change);
        QCOMPARE(checkpoints, 2);
        change.positionMs = 50000;
        change.seekCompleted = true;
        backend->listener(change);
        QCOMPARE(checkpoints, 3);
        change.seekCompleted = false;
        backend->listener(change);
        QCOMPARE(checkpoints, 3);

        session.resume();
        backend->emitState(EngineState::Playing, 50000);
        session.pause();
        backend->emitState(EngineState::Paused, 50000);
        QCOMPARE(checkpoints, 4);
    }
    void confirmedTracksCheckpointAndRestore_data()
    {
        QTest::addColumn<bool>("fromBeginning");
        QTest::newRow("resume") << false;
        QTest::newRow("from-beginning") << true;
    }
    void confirmedTracksCheckpointAndRestore()
    {
        QFETCH(bool, fromBeginning);
        Fixture fixture;
        VodModule module({true}, [&]() { return fixture.composition(); });
        const auto ref = refFor(fixture.source);
        module.controller()->play(ref);
        QTRY_VERIFY(fixture.engine && fixture.engine->loads == 1);
        fixture.engine->emitState(EngineState::Loaded);
        fixture.engine->emitState(EngineState::Playing, 60000);
        const QJsonObject preferences{{QStringLiteral("sub"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("off")}}},
            {QStringLiteral("audio"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("track")}, {QStringLiteral("id"), 2}}}};
        PlaybackEvent choice;
        choice.loadToken = fixture.engine->token; choice.state = EngineState::Playing;
        choice.positionMs = 60000; choice.trackPreferences = preferences;
        auto stale = choice; stale.loadToken = QUuid::createUuid();
        fixture.engine->listener(stale);
        QVERIFY(module.session()->snapshot().trackPreferences.isEmpty());
        fixture.engine->listener(choice);
        QTRY_VERIFY(fixture.store->checkpoints.load() >= 1);
        auto saved = std::get<std::optional<VodProgress>>(fixture.store->read(ref, requestFor(fixture.source)));
        QVERIFY(saved); QCOMPARE(saved->trackPreferences, preferences);
        module.session()->stop();
        PlaybackPreferences options; options.fromBeginning = fromBeginning;
        module.controller()->play(ref, options);
        QTRY_COMPARE(fixture.engine->loads, 2);
        QCOMPARE(fixture.engine->request.trackPreferences, preferences);
        fixture.engine->emitState(EngineState::Loaded);
        QCOMPARE(fixture.engine->seeks, fromBeginning ? 0 : 1);
        if (!fromBeginning) fixture.engine->emitSeekCompleted(55000);
        module.session()->stop();
        auto other = ref; other.providerItemId = QStringLiteral("other");
        module.controller()->play(other);
        QTRY_COMPARE(fixture.engine->loads, 3);
        QVERIFY(fixture.engine->request.trackPreferences.isEmpty());
    }
    void preplayTrackPreferencesOverrideSavedChoice()
    {
        Fixture fixture;
        VodModule module({true}, [&]() { return fixture.composition(); });
        const QJsonObject selected{
            {QStringLiteral("audio"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("track")},
                 {QStringLiteral("ordinal"), 1}, {QStringLiteral("lang"), QStringLiteral("eng")},
                 {QStringLiteral("codec"), QStringLiteral("aac")}}},
            {QStringLiteral("sub"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("off")}}}};
        PlaybackPreferences options;
        options.trackPreferences = selected;
        module.controller()->play(refFor(fixture.source), options);
        QTRY_VERIFY(fixture.engine && fixture.engine->loads == 1);
        QCOMPARE(fixture.engine->request.trackPreferences, selected);
        QVERIFY(!fixture.engine->request.startPaused);
    }
    void categoryCacheAvoidsNetworkAndFailedRefreshPreservesSnapshot()
    {
        Fixture fixture;
        VodModule module({true}, [&]() { return fixture.composition(); });
        QList<VodEvent> events;
        module.controller()->completed = [&](const VodEvent &event) { events.append(event); };
        const auto scope = scopeFor(fixture.source);
        const CategorySnapshot old{scope, {{scope, QStringLiteral("007"), QStringLiteral("Saved category"), {}}}, QDateTime::currentDateTimeUtc().addDays(-2)};
        const auto request = requestFor(fixture.source);
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginCategoryRefresh(scope, request)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->storeCategories(old, request)));
        module.controller()->categories(scope);
        QTRY_COMPARE(events.size(), 1);
        QCOMPARE(fixture.provider->calls.load(), 0);
        QCOMPARE(std::get<CategorySnapshot>(std::get<PublicValue>(events.last().result)).refreshedAtUtc, old.refreshedAtUtc);
        fixture.provider->categories = [](const CatalogScope &) -> Result<QList<VodCategory>> { return Error{ErrorCode::ProviderUnavailable, {}}; };
        module.controller()->categories(scope, true);
        QTRY_COMPARE(events.size(), 2);
        QVERIFY(std::holds_alternative<Error>(events.last().result));
        module.controller()->categories(scope);
        QTRY_COMPARE(events.size(), 3);
        QCOMPARE(std::get<CategorySnapshot>(std::get<PublicValue>(events.last().result)).categories.first().name, QStringLiteral("Saved category"));
        QCOMPARE(fixture.provider->calls.load(), 1);
        fixture.provider->categories = {};
        module.controller()->categories(scope, true);
        QTRY_COMPARE(events.size(), 4);
        QVERIFY(std::get<CategorySnapshot>(std::get<PublicValue>(events.last().result)).categories.isEmpty());
        module.controller()->categories(scope);
        QTRY_COMPARE(events.size(), 5);
        QVERIFY(std::get<CategorySnapshot>(std::get<PublicValue>(events.last().result)).categories.isEmpty());
        QCOMPARE(fixture.provider->calls.load(), 2);
        QVERIFY(!fixture.provider->calledOnGui);
    }
    void recordingStartingDuringHandoffRejectsVod()
    {
        bool recording = false;
        LegacyPlaybackAdapter::Acknowledgement released;
        LegacyPlaybackAdapter legacy({[&]() { return LegacyResources{false, recording, false}; },
            [&](const QUuid &, LegacyPlaybackAdapter::Acknowledgement ack) { released = std::move(ack); }, []() {}});
        std::optional<Outcome> result;
        legacy.release(QUuid::createUuid(), [&](Outcome outcome) { result = std::move(outcome); });
        QVERIFY(!result);
        recording = true;
        released(Success{});
        QVERIFY(result);
        QCOMPARE(std::get<Error>(*result).code, ErrorCode::PlaybackConflict);
        QVERIFY(recording);
    }
    void explicitLiveCancelsPendingResolver()
    {
        Fixture fixture;
        QSemaphore entered, release;
        fixture.provider->resolve = [&](const SourceContext &source, const ContentRef &ref) -> Result<PlaybackDescriptor> {
            entered.release();
            release.acquire();
            return descriptorFor(source, ref);
        };
        VodModule module({true}, [&]() { return fixture.composition(); });
        QList<VodEvent> events;
        module.controller()->completed = [&](const VodEvent &event) { events.append(event); };
        module.controller()->play(refFor(fixture.source));
        const auto unblock = qScopeGuard([&]() { release.release(); });
        QTRY_VERIFY(entered.available() > 0);
        QCOMPARE(module.coordinator()->owner(), PlaybackOwner::Legacy);
        module.controller()->cancelPendingPlayback();
        module.coordinator()->requestLive();
        release.release();
        QTRY_VERIFY(!events.isEmpty());
        QVERIFY(std::holds_alternative<Error>(events.last().result));
        QCOMPARE(std::get<Error>(events.last().result).code, ErrorCode::Cancelled);
        QVERIFY(!fixture.engine);
        QCOMPARE(fixture.liveActivations, 1);
    }
    void sourceEditWaitsForStopAndDurableCheckpoint()
    {
        Fixture fixture;
        VodModule module({true}, [&]() { return fixture.composition(); });
        const auto ref = refFor(fixture.source);
        module.controller()->play(ref);
        QTRY_VERIFY(fixture.engine && fixture.engine->loads == 1);
        fixture.engine->emitState(EngineState::Playing, 73000);
        fixture.engine->acknowledgeStop = false;
        std::optional<Outcome> prepared;
        module.prepareSourceChange(ref.profileId, [&](Outcome result) { prepared = result; });
        QCOMPARE(fixture.engine->stops, 1);
        QVERIFY(!prepared);
        module.controller()->play(ref); // cannot reopen while credentials are changing
        fixture.engine->emitEnd(fixture.engine->token, EndReason::UserStop);
        QTRY_VERIFY(prepared.has_value());
        QVERIFY(std::holds_alternative<Success>(*prepared));
        QCOMPARE(fixture.engine->loads, 1);
        auto saved = std::get<std::optional<VodProgress>>(fixture.store->read(ref, requestFor(fixture.source)));
        QVERIFY(saved);
        QCOMPARE(saved->positionMs, 73000);
        fixture.store->editCredentials();
        module.finishSourceChange(ref.profileId);
        module.controller()->play(ref);
        QTRY_COMPARE(fixture.engine->loads, 2);
        fixture.engine->acknowledgeStop = true;
    }
    void recoveryIsBoundedAndPreservesPauseAndPosition()
    {
        const auto source = makeSource();
        auto engine = std::make_unique<Engine>(); auto *backend = engine.get();
        VodPlaybackSession session(std::move(engine));
        int resolutions = 0;
        session.recover = [&](const ContentRef &ref, VodPlaybackSession::RecoveryCompletion done) {
            ++resolutions; done(descriptorFor(source, ref));
        };
        session.open(descriptorFor(source, refFor(source)), QUuid::createUuid(), {});
        backend->emitState(EngineState::Playing, 50000);
        const QJsonObject preferences{{QStringLiteral("sub"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("off")}}}};
        PlaybackEvent choice; choice.loadToken = backend->token; choice.state = EngineState::Playing;
        choice.positionMs = 50000; choice.trackPreferences = preferences;
        backend->listener(choice);
        session.pause();
        backend->emitState(EngineState::Paused, 50000);
        for (int index = 0; index < 2; ++index) {
            QElapsedTimer retryWait; retryWait.start();
            const auto retired = backend->token;
            backend->listener({retired, EngineState::Stopped, -1, {}, false, false, EndReason::Error, true});
            QCOMPARE(session.snapshot().state, SessionState::Recovering);
            backend->emitEnd(retired, EndReason::Error); // duplicate old event
            QTRY_COMPARE_WITH_TIMEOUT(backend->loads, index + 2, 7000);
            QVERIFY(retryWait.elapsed() >= (index == 0 ? 1800 : 4500));
            QVERIFY(backend->request.startPaused);
            QCOMPARE(backend->request.trackPreferences, preferences);
            backend->emitState(EngineState::Loaded);
            QCOMPARE(backend->seekPosition, 50000);
            backend->emitSeekCompleted(50000);
            QCOMPARE(session.snapshot().state, SessionState::Paused);
        }
        backend->listener({backend->token, EngineState::Stopped, -1, {}, false, false, EndReason::Error, true});
        QCOMPARE(session.snapshot().state, SessionState::Failed);
        QCOMPARE(resolutions, 2);
        QCOMPARE(backend->loads, 3);
    }
    void stopDuringRecoveryRejectsLateDescriptor()
    {
        const auto source = makeSource();
        auto engine = std::make_unique<Engine>(); auto *backend = engine.get();
        VodPlaybackSession session(std::move(engine));
        VodPlaybackSession::RecoveryCompletion completion;
        session.recover = [&](const ContentRef &, VodPlaybackSession::RecoveryCompletion done) { completion = std::move(done); };
        session.open(descriptorFor(source, refFor(source)), QUuid::createUuid(), {});
        backend->listener({backend->token, EngineState::Stopped, -1, {}, false, false, EndReason::Error, true});
        QTRY_VERIFY(bool(completion));
        session.stop();
        QCOMPARE(session.snapshot().state, SessionState::Stopped);
        completion(descriptorFor(source, refFor(source)));
        QCOMPARE(backend->loads, 1);
    }
    void changedDurationDisablesResumeAndPendingSeekCannotEraseBookmark()
    {
        const auto source = makeSource();
        auto engine = std::make_unique<Engine>(); auto *backend = engine.get();
        VodPlaybackSession session(std::move(engine));
        VodProgress progress; progress.positionMs = 60000; progress.durationMs = 120000;
        session.open(descriptorFor(source, refFor(source)), QUuid::createUuid(), progress);
        backend->emitState(EngineState::Loaded, 0, 600000);
        QCOMPARE(backend->seeks, 0);
        session.stop();
        session.open(descriptorFor(source, refFor(source)), QUuid::createUuid(), progress);
        backend->emitState(EngineState::Loaded);
        backend->emitState(EngineState::Playing, 0);
        QVERIFY(!session.snapshot().positionValid);
        QCOMPARE(session.snapshot().state, SessionState::SeekingResume);
        backend->emitSeekCompleted(55000);
        QVERIFY(session.snapshot().positionValid);
        QCOMPARE(session.snapshot().positionMs, 55000);
    }
    void newSessionCheckpointsEvenAtIdenticalPosition()
    {
        Fixture fixture;
        VodProgressService progress(fixture.dependencies());
        SessionSnapshot snapshot;
        snapshot.ref = refFor(fixture.source);
        snapshot.sessionToken = QUuid::createUuid();
        snapshot.positionValid = true;
        snapshot.positionMs = 45000;
        snapshot.durationMs = 120000;
        progress.observe(snapshot, true);
        QTRY_COMPARE(fixture.store->checkpoints.load(), 1);
        snapshot.ref.providerItemId = QStringLiteral("second");
        snapshot.sessionToken = QUuid::createUuid();
        snapshot.positionValid = false;
        progress.observe(snapshot, false);
        progress.flush();
        snapshot.positionValid = true;
        progress.observe(snapshot, false);
        progress.flush();
        QTRY_COMPARE(fixture.store->checkpoints.load(), 2);
        const auto saved = std::get<std::optional<VodProgress>>(
            fixture.store->read(snapshot.ref, requestFor(fixture.source)));
        QVERIFY(saved);
        QCOMPARE(saved->positionMs, 45000);
        QCOMPARE(saved->sequence, quint64(1));
        QCOMPARE(saved->sessionToken, snapshot.sessionToken);
    }
    void duplicateReleaseAckCannotRetireActiveOwner()
    {
        Fixture fixture; fixture.deferRelease = true;
        VodModule module({true}, [&]() { return fixture.composition(); });
        module.controller()->play(refFor(fixture.source));
        QTRY_VERIFY(bool(fixture.releaseAck));
        const auto ack = fixture.releaseAck;
        ack(Success{});
        QCOMPARE(module.coordinator()->owner(), PlaybackOwner::Vod);
        ack(Success{});
        QCOMPARE(module.coordinator()->owner(), PlaybackOwner::Vod);
        QCOMPARE(fixture.engine->loads, 1);
        fixture.engine->acknowledgeStop = false;
        module.coordinator()->requestLive();
        QCOMPARE(fixture.liveActivations, 0);
        fixture.engine->emitEnd(fixture.engine->token, EndReason::UserStop);
        QCOMPARE(fixture.liveActivations, 1);
    }
    void malformedPlaybackHeadersAreRejected()
    {
        Fixture fixture;
        const QList<QPair<QByteArray, QByteArray>> invalid = {
            {"Bad Header", "value"}, {"Bad\tHeader", "value"},
            {"X-Test", QByteArray("a\0b", 3)}, {"X-Test", QByteArray(1, '\x7f')}
        };
        for (const auto &header : invalid) {
            fixture.provider->resolve = [header](const SourceContext &source, const ContentRef &ref) -> Result<PlaybackDescriptor> {
                auto descriptor = descriptorFor(source, ref);
                descriptor.allowedHeaders.insert(header.first, header.second);
                return descriptor;
            };
            const auto result = VodPlaybackResolver::resolve(fixture.dependencies(), fixture.source,
                refFor(fixture.source), {}, requestFor(fixture.source));
            QVERIFY(std::holds_alternative<Error>(result));
            QCOMPARE(std::get<Error>(result).code, ErrorCode::InvalidResponse);
        }
    }
    void disabledModuleIsInert()
    {
        Fixture fixture;
        int creations = 0;
        VodModule module({}, [&]() { ++creations; return fixture.composition(); });
        QVERIFY(!module.enabled()); QVERIFY(!module.controller());
        QCOMPARE(creations, 0); QCOMPARE(fixture.store->migrations.load(), 0);
        QCOMPARE(fixture.provider->calls.load(), 0); QVERIFY(!fixture.engine);
        VodModule unavailable({true});
        QVERIFY(!unavailable.enabled());
        QCOMPARE(unavailable.initializationError()->code, ErrorCode::UnsupportedCapability);
    }
    void facadeCatalogToPlaybackWithoutGuiNetworkOrMpv()
    {
        Fixture fixture;
        const auto ref = refFor(fixture.source);
        fixture.provider->fetch = [ref](const CatalogScope &scope, const RequestContext &) -> Result<CatalogBatch> {
            MovieSummary movie; movie.ref = ref; movie.title = QStringLiteral("Synthetic film");
            return CatalogBatch{scope, {movie}, true, {}};
        };
        VodModule module({true}, [&]() { return fixture.composition(); });
        QList<VodEvent> events;
        module.controller()->completed = [&](const VodEvent &event) { events.append(event); };
        const auto refresh = module.controller()->refresh(scopeFor(fixture.source));
        QTRY_COMPARE(events.size(), 1);
        QCOMPARE(events.last().operationId, refresh);
        QVERIFY(std::holds_alternative<PublicValue>(events.last().result));
        CatalogQuery query; query.scope = scopeFor(fixture.source);
        module.controller()->query(query);
        QTRY_COMPARE(events.size(), 2);
        const auto page = std::get<CatalogPage>(std::get<PublicValue>(events.last().result));
        QCOMPARE(page.items.size(), 1);
        QVERIFY(!fixture.engine);
        const auto catalogRef = std::get<MovieSummary>(page.items.first()).ref;
        module.controller()->play(catalogRef);
        QTRY_COMPARE(events.size(), 3);
        QCOMPARE(fixture.engine->loads, 1); QCOMPARE(fixture.releases, 1);
        QCOMPARE(fixture.engine->request.transport, TransportPolicy::NativeMedia);
        QCOMPARE(fixture.engine->request.validatedEngineOptions.value(QStringLiteral("keep-open")), QStringLiteral("no"));
        QVERIFY(!fixture.provider->calledOnGui.load());
        fixture.engine->emitState(EngineState::Playing, 45000);
        fixture.engine->emitState(EngineState::Paused, 45000);
        QTRY_VERIFY(fixture.store->checkpoints.load() > 0);
        auto saved = fixture.store->read(ref, requestFor(fixture.source));
        QCOMPARE(std::get<std::optional<VodProgress>>(saved)->positionMs, 45000);
        module.shutdown();
    }
    void credentialsAndDomainEditsKeepIdentityAndHistory()
    {
        Fixture fixture;
        auto request = requestFor(fixture.source);
        const auto ref = refFor(fixture.source);
        VodProgress progress; progress.sessionToken = QUuid::createUuid(); progress.sequence = 1; progress.positionMs = 80000;
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(ref, progress.sessionToken, request)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(ref, progress, request)));
        auto import = std::get<ImportToken>(fixture.store->beginRefresh(scopeFor(fixture.source), request));
        fixture.store->stageBatch(import, {scopeFor(fixture.source), {}, true, {}});
        fixture.store->editCredentials();
        const auto edited = std::get<SourceContext>(fixture.store->snapshot(fixture.source.revision.profileId));
        QCOMPARE(edited.revision.catalogNamespace, fixture.source.revision.catalogNamespace);
        QCOMPARE(refFor(edited).key(), ref.key());
        QVERIFY(!fixture.store->isCurrent(fixture.source.revision));
        QVERIFY(std::holds_alternative<Error>(fixture.store->publishIfCurrent(import, true)));
        auto saved = std::get<std::optional<VodProgress>>(fixture.store->read(ref, requestFor(edited)));
        QVERIFY(saved); QCOMPARE(saved->positionMs, 80000);
    }
    void importPublicationBarrierAndCacheIsolation()
    {
        Fixture fixture;
        const auto scope = scopeFor(fixture.source);
        auto request = requestFor(fixture.source);
        auto old = std::get<ImportToken>(fixture.store->beginRefresh(scope, request));
        fixture.store->stageBatch(old, {scope, {}, true, {}});
        auto current = std::get<ImportToken>(fixture.store->beginRefresh(scope, request));
        QVERIFY(std::holds_alternative<Error>(fixture.store->publishIfCurrent(old, true)));
        fixture.store->stageBatch(current, {scope, {}, false, {ProviderCursor{QStringLiteral("next")}}});
        QVERIFY(std::holds_alternative<Error>(fixture.store->publishIfCurrent(current, true)));
        fixture.store->stageBatch(current, {scope, {}, true, {}});
        request.cancelled->store(true);
        QVERIFY(std::holds_alternative<Error>(fixture.store->publishIfCurrent(current, true)));
        QCOMPARE(fixture.store->publications.load(), 0);
        request = requestFor(fixture.source);
        const auto empty = std::get<ImportToken>(fixture.store->beginRefresh(scope, request));
        fixture.store->stageBatch(empty, {scope, {}, true, {}});
        QVERIFY(std::holds_alternative<quint64>(fixture.store->publishIfCurrent(empty, true)));
        const auto ref = refFor(fixture.source);
        VodProgress progress; progress.sessionToken = QUuid::createUuid(); progress.sequence = 1; progress.positionMs = 50000;
        fixture.store->beginSession(ref, progress.sessionToken, request);
        fixture.store->checkpoint(ref, progress, request);
        fixture.store->evictCache(scope, request);
        QVERIFY(std::get<std::optional<VodProgress>>(fixture.store->read(ref, request)).has_value());
        auto pending = std::get<ImportToken>(fixture.store->beginRefresh(scope, request));
        fixture.store->stageBatch(pending, {scope, {}, true, {}});
        fixture.store->prepareRemoval(scope.profileId);
        fixture.store->removeSourceState(scope.profileId);
        QVERIFY(std::holds_alternative<Error>(fixture.store->publishIfCurrent(pending, true)));
        QVERIFY(std::holds_alternative<Error>(fixture.store->checkpoint(ref, progress, request)));
    }
    void backwardSeekWinsOverLateCheckpoint()
    {
        Fixture fixture;
        const auto request = requestFor(fixture.source);
        const auto ref = refFor(fixture.source);
        VodProgress progress; progress.sessionToken = QUuid::createUuid(); progress.sequence = 1; progress.positionMs = 90000;
        fixture.store->beginSession(ref, progress.sessionToken, request);
        fixture.store->checkpoint(ref, progress, request);
        auto backwards = progress; backwards.sequence = 2; backwards.positionMs = 10000;
        fixture.store->checkpoint(ref, backwards, request);
        QVERIFY(std::holds_alternative<Error>(fixture.store->checkpoint(ref, progress, request)));
        QCOMPARE(std::get<std::optional<VodProgress>>(fixture.store->read(ref, request))->positionMs, 10000);
        fixture.store->beginSession(ref, QUuid::createUuid(), request);
        backwards.sequence = 999;
        QVERIFY(std::holds_alternative<Error>(fixture.store->checkpoint(ref, backwards, request)));
    }
    void stablePagesAndCategoryScopedReplacement()
    {
        Fixture fixture;
        auto request = requestFor(fixture.source);
        auto scope = scopeFor(fixture.source); scope.categoryId = QStringLiteral("a");
        const auto publish = [&](const CatalogScope &range, QList<CatalogItem> items) {
            const auto token = std::get<ImportToken>(fixture.store->beginRefresh(range, request));
            fixture.store->stageBatch(token, {range, items, true, {}});
            return fixture.store->publishIfCurrent(token, true);
        };
        MovieSummary one; one.ref = refFor(fixture.source, QStringLiteral("1")); one.title = QStringLiteral("Same");
        auto two = one; two.ref.providerItemId = QStringLiteral("2");
        publish(scope, {two, one});
        auto other = scope; other.categoryId = QStringLiteral("b"); publish(other, {one});
        CatalogQuery query; query.scope = scope; query.pageSize = 1;
        auto first = std::get<CatalogPage>(fixture.store->query(query, request));
        QVERIFY(first.next); QCOMPARE(first.items.size(), 1);
        query.page = first.next;
        auto second = std::get<CatalogPage>(fixture.store->query(query, request));
        QVERIFY(std::get<MovieSummary>(first.items.first()).ref != std::get<MovieSummary>(second.items.first()).ref);
        publish(scope, {});
        QVERIFY(std::holds_alternative<Error>(fixture.store->query(query, request)));
        query.scope = other; query.page.reset();
        QCOMPARE(std::get<CatalogPage>(fixture.store->query(query, request)).items.size(), 1);
    }
    void refreshErrorsNeverPublishEmptyCatalog()
    {
        Fixture fixture;
        fixture.provider->fetch = [](const CatalogScope &, const RequestContext &request) -> Result<CatalogBatch> {
            return Error{ErrorCode::Unauthorized, request.operationId};
        };
        auto result = VodCatalogService::refresh(fixture.dependencies(), fixture.source, scopeFor(fixture.source), requestFor(fixture.source));
        QCOMPARE(std::get<Error>(result).code, ErrorCode::Unauthorized);
        QCOMPARE(fixture.store->publications.load(), 0);
        fixture.provider->fetch = [](const CatalogScope &scope, const RequestContext &) -> Result<CatalogBatch> {
            return CatalogBatch{scope, {}, false, ProviderCursor{QStringLiteral("repeated")}};
        };
        result = VodCatalogService::refresh(fixture.dependencies(), fixture.source, scopeFor(fixture.source), requestFor(fixture.source));
        QCOMPARE(std::get<Error>(result).code, ErrorCode::InvalidResponse);
        QCOMPARE(fixture.store->publications.load(), 0);
    }
    void cancellationAtTransportAndPublicationBoundaries()
    {
        Fixture fixture;
        std::atomic_bool entered{false};
        fixture.provider->fetch = [&](const CatalogScope &scope, const RequestContext &context) -> Result<CatalogBatch> {
            entered = true;
            while (!context.interruption()) QThread::msleep(1);
            // Simulate a provider returning data despite the cancellation.
            return CatalogBatch{scope, {}, true, {}};
        };
        VodModule module({true}, [&]() { return fixture.composition(); });
        QList<VodEvent> events;
        module.controller()->completed = [&](const VodEvent &event) { events.append(event); };
        const auto id = module.controller()->refresh(scopeFor(fixture.source));
        QTRY_VERIFY(entered.load());
        module.controller()->cancel(id);
        QTRY_COMPARE(events.size(), 1);
        QCOMPARE(std::get<Error>(events.last().result).code, ErrorCode::Cancelled);
        QCOMPARE(fixture.store->publications.load(), 0);
    }
    void seriesAndUnsafeUrlsCannotOpenEngine()
    {
        Fixture fixture;
        VodModule module({true}, [&]() { return fixture.composition(); });
        QList<VodEvent> events;
        module.controller()->completed = [&](const VodEvent &event) { events.append(event); };
        auto series = refFor(fixture.source); series.kind = ContentKind::Series;
        module.controller()->play(series);
        QTRY_COMPARE(events.size(), 1);
        QCOMPARE(std::get<Error>(events.last().result).code, ErrorCode::ContentUnavailable);
        fixture.provider->resolve = [](const SourceContext &source, const ContentRef &ref) -> Result<PlaybackDescriptor> {
            auto value = descriptorFor(source, ref); value.mediaUri = QUrl(QStringLiteral("file:///tmp/not-allowed")); return value;
        };
        module.controller()->play(refFor(fixture.source));
        QTRY_COMPARE(events.size(), 2);
        QCOMPARE(std::get<Error>(events.last().result).code, ErrorCode::InvalidResponse);
        QVERIFY(!fixture.engine); QCOMPARE(fixture.releases, 0);
        QVERIFY(!std::get<Error>(events.last().result).message().contains(QStringLiteral("synthetic-secret")));
    }
    void sourcePolicyChangeDuringLegacyReleaseCannotStart()
    {
        Fixture fixture; fixture.deferRelease = true;
        const auto allowed = std::make_shared<std::atomic_bool>(true);
        fixture.source.policyCurrent = [allowed]() { return allowed->load(); };
        fixture.store = std::make_shared<MemoryStore>(fixture.source);
        VodModule module({true}, [&]() { return fixture.composition(); });
        QList<VodEvent> events;
        connect(module.controller(), &VodController::eventCompleted, module.controller(), [&](const VodEvent &event) { events.append(event); });
        module.controller()->play(refFor(fixture.source));
        QTRY_VERIFY(bool(fixture.releaseAck));
        allowed->store(false); fixture.releaseAck(Success{});
        QTRY_COMPARE(events.size(), 1);
        QCOMPARE(std::get<Error>(events.first().result).code, ErrorCode::Cancelled);
        QVERIFY(!fixture.engine);
    }
    void handoffWaitsForLegacyAndStopAcknowledgement()
    {
        Fixture fixture; fixture.deferRelease = true;
        VodModule module({true}, [&]() { return fixture.composition(); });
        module.controller()->play(refFor(fixture.source));
        QTRY_VERIFY(bool(fixture.releaseAck));
        QVERIFY(!fixture.engine);
        fixture.releaseAck(Success{});
        QCOMPARE(fixture.engine->loads, 1);
        fixture.engine->acknowledgeStop = false;
        module.coordinator()->requestLive();
        QCOMPARE(fixture.liveActivations, 0);
        fixture.engine->emitEnd(fixture.engine->token, EndReason::UserStop);
        QCOMPARE(fixture.liveActivations, 1);
        QCOMPARE(module.coordinator()->owner(), PlaybackOwner::Legacy);
    }
    void recordingsConflictWithoutBeingStopped()
    {
        Fixture fixture; fixture.resources.engineBoundRecording = true;
        VodModule module({true}, [&]() { return fixture.composition(); });
        QList<VodEvent> events;
        module.controller()->completed = [&](const VodEvent &event) { events.append(event); };
        module.controller()->play(refFor(fixture.source));
        QTRY_COMPARE(events.size(), 1);
        QCOMPARE(std::get<Error>(events.last().result).code, ErrorCode::PlaybackConflict);
        QVERIFY(!fixture.engine); QCOMPARE(fixture.releases, 0);
        QVERIFY(fixture.resources.engineBoundRecording);
    }
    void credentialsInvalidatePendingDescriptor()
    {
        Fixture fixture; fixture.deferRelease = true;
        VodModule module({true}, [&]() { return fixture.composition(); });
        QList<VodEvent> events;
        module.controller()->completed = [&](const VodEvent &event) { events.append(event); };
        module.controller()->play(refFor(fixture.source));
        QTRY_VERIFY(bool(fixture.releaseAck));
        fixture.store->editCredentials();
        fixture.releaseAck(Success{});
        QVERIFY(!fixture.engine);
        QCOMPARE(std::get<Error>(events.last().result).code, ErrorCode::Cancelled);
    }
    void naturalEndAndStaleEventsNeverReconnectOrFinishReplacement()
    {
        Fixture fixture;
        VodModule module({true}, [&]() { return fixture.composition(); });
        module.controller()->play(refFor(fixture.source));
        QTRY_VERIFY(fixture.engine != nullptr);
        QTRY_COMPARE(fixture.engine->loads, 1);
        const auto old = fixture.engine->token;
        fixture.engine->emitState(EngineState::Playing, 45000);
        module.controller()->play(refFor(fixture.source, QStringLiteral("2")));
        QTRY_COMPARE(fixture.engine->loads, 2);
        fixture.engine->emitEnd(old, EndReason::NaturalEnd);
        QCOMPARE(module.session()->snapshot().state, SessionState::Opening);
        fixture.engine->emitState(EngineState::Playing, 120000);
        fixture.engine->emitEnd(fixture.engine->token, EndReason::NaturalEnd);
        QCOMPARE(module.session()->snapshot().state, SessionState::Ended);
        QCOMPARE(module.coordinator()->owner(), PlaybackOwner::None);
        fixture.engine->emitEnd(fixture.engine->token, EndReason::NaturalEnd);
        QCOMPARE(fixture.engine->loads, 2); QCOMPARE(fixture.liveActivations, 0);
    }
    void resumeRequiresOneMinute_data()
    {
        QTest::addColumn<qint64>("position");
        QTest::addColumn<int>("seeks");
        QTest::newRow("under-minute") << qint64(59999) << 0;
        QTest::newRow("one-minute") << qint64(60000) << 1;
    }
    void resumeRequiresOneMinute()
    {
        QFETCH(qint64, position); QFETCH(int, seeks);
        const auto source = makeSource();
        auto engine = std::make_unique<Engine>(); auto *backend = engine.get();
        VodPlaybackSession session(std::move(engine));
        VodProgress progress; progress.positionMs = position; progress.durationMs = 120000;
        QVERIFY(std::holds_alternative<Success>(session.open(descriptorFor(source, refFor(source)), QUuid::createUuid(), progress)));
        backend->emitState(EngineState::Loaded);
        QCOMPARE(backend->seeks, seeks);
        if (seeks) QCOMPARE(backend->seekPosition, position - 5000);
        else QCOMPARE(session.snapshot().positionMs, qint64(0));
    }
    void resumeOnlyAfterLoadedWithKnownDurationAndSeek()
    {
        auto source = makeSource();
        auto engine = std::make_unique<Engine>(); auto *backend = engine.get();
        VodPlaybackSession session(std::move(engine));
        VodProgress resume; resume.positionMs = 65000;
        session.open(descriptorFor(source, refFor(source)), QUuid::createUuid(), resume);
        QCOMPARE(backend->seeks, 0);
        backend->emitState(EngineState::Loaded);
        QCOMPARE(backend->seekPosition, 60000);
        backend->emitState(EngineState::Loaded);
        QCOMPARE(backend->seeks, 1);
        QTest::qWait(3200); // A remote seek must not fail at the former 3s limit.
        QCOMPARE(session.snapshot().state, SessionState::SeekingResume);
        backend->emitSeekCompleted(60000);
        backend->emitState(EngineState::Playing, 46000);
        backend->emitEnd(backend->token, EndReason::Error);
        QCOMPARE(session.snapshot().state, SessionState::Failed);
        QCOMPARE(session.snapshot().positionMs, 46000);
        session.open(descriptorFor(source, refFor(source)), QUuid::createUuid(), resume);
        backend->emitState(EngineState::Loaded, 0, {}, true);
        QCOMPARE(backend->seeks, 1);
        session.stop();
        session.open(descriptorFor(source, refFor(source)), QUuid::createUuid(), resume);
        backend->emitState(EngineState::Loaded, 0, 120000, false);
        QCOMPARE(backend->seeks, 1);
    }
    void sourceRemovalCancelsImportAndPlayback()
    {
        Fixture fixture;
        VodModule module({true}, [&]() { return fixture.composition(); });
        module.controller()->play(refFor(fixture.source));
        QTRY_VERIFY(fixture.engine != nullptr);
        QTRY_COMPARE(fixture.engine->loads, 1);
        std::atomic_bool entered{false};
        fixture.provider->fetch = [&](const CatalogScope &scope, const RequestContext &request) -> Result<CatalogBatch> {
            entered = true;
            while (!request.interruption()) QThread::msleep(1);
            return CatalogBatch{scope, {}, true, {}};
        };
        module.controller()->refresh(scopeFor(fixture.source));
        QTRY_VERIFY(entered.load());
        QList<VodEvent> events;
        module.controller()->completed = [&](const VodEvent &event) { events.append(event); };
        const auto removal = module.controller()->removeSource(fixture.source.revision.profileId);
        QTRY_VERIFY(std::any_of(events.cbegin(), events.cend(), [&](const auto &event) { return event.operationId == removal; }));
        QVERIFY(fixture.engine->stops > 0); QCOMPARE(fixture.store->publications.load(), 0);
        QVERIFY(!fixture.store->isCurrent(fixture.source.revision));
    }
    void removalCompletionWaitsForStop()
    {
        Fixture fixture;
        VodModule module({true}, [&]() { return fixture.composition(); });
        module.controller()->play(refFor(fixture.source));
        QTRY_VERIFY(fixture.engine != nullptr);
        fixture.engine->acknowledgeStop = false;
        QList<VodEvent> events;
        module.controller()->completed = [&](const VodEvent &event) { events.append(event); };
        const auto id = module.controller()->removeSource(fixture.source.revision.profileId);
        QTRY_COMPARE(fixture.engine->stops, 1);
        QVERIFY(events.isEmpty());
        QVERIFY(!fixture.store->isCurrent(fixture.source.revision));
        fixture.engine->emitEnd(fixture.engine->token, EndReason::UserStop);
        QTRY_COMPARE(events.size(), 1);
        QCOMPARE(events.last().operationId, id);
    }
    void cancelledPendingPlayNeverLoadsAfterRelease()
    {
        Fixture fixture; fixture.deferRelease = true;
        VodModule module({true}, [&]() { return fixture.composition(); });
        QList<VodEvent> events;
        module.controller()->completed = [&](const VodEvent &event) { events.append(event); };
        const auto id = module.controller()->play(refFor(fixture.source));
        QTRY_VERIFY(bool(fixture.releaseAck));
        module.controller()->cancel(id);
        fixture.releaseAck(Success{});
        QVERIFY(!fixture.engine);
        QCOMPARE(events.size(), 1);
        QCOMPARE(std::get<Error>(events.first().result).code, ErrorCode::Cancelled);
    }
    void failedOpeningDoesNotEraseResumePosition()
    {
        Fixture fixture;
        const auto ref = refFor(fixture.source);
        const auto request = requestFor(fixture.source);
        VodProgress progress; progress.sessionToken = QUuid::createUuid(); progress.sequence = 1; progress.positionMs = 65000;
        fixture.store->beginSession(ref, progress.sessionToken, request);
        fixture.store->checkpoint(ref, progress, request);
        VodModule module({true}, [&]() { return fixture.composition(); });
        module.controller()->play(ref);
        QTRY_VERIFY(fixture.engine != nullptr);
        fixture.engine->emitEnd(fixture.engine->token, EndReason::Error);
        module.shutdown();
        const auto saved = std::get<std::optional<VodProgress>>(fixture.store->read(ref, request));
        QVERIFY(saved); QCOMPARE(saved->positionMs, 65000);
    }
    void progressLaneOrdersCheckpointsWithoutWaitingForTimer()
    {
        Fixture fixture;
        VodProgressService progress(fixture.dependencies());
        SessionSnapshot snapshot;
        snapshot.ref = refFor(fixture.source); snapshot.sessionToken = QUuid::createUuid();
        snapshot.positionValid = true; snapshot.state = SessionState::Playing; snapshot.positionMs = 80000;
        progress.observe(snapshot, false);
        progress.flush();
        snapshot.positionMs = 10000;
        progress.observe(snapshot, true);
        QTRY_COMPARE(fixture.store->checkpoints.load(), 2);
        const auto saved = std::get<std::optional<VodProgress>>(fixture.store->read(snapshot.ref, requestFor(fixture.source)));
        QVERIFY(saved); QCOMPARE(saved->positionMs, 10000); QCOMPARE(saved->sequence, quint64(2));
    }
    void progressInitializationRetriesBeforeCheckpoint()
    {
        Fixture fixture;
        fixture.store->beginFailures = 2;
        VodProgressService progress(fixture.dependencies());
        int failures = 0;
        progress.failed = [&](const Error &) { ++failures; };
        SessionSnapshot snapshot;
        snapshot.ref = refFor(fixture.source); snapshot.sessionToken = QUuid::createUuid();
        snapshot.positionValid = true; snapshot.state = SessionState::Playing; snapshot.positionMs = 80000;
        progress.observe(snapshot, true);
        QTRY_COMPARE(failures, 2);
        QCOMPARE(fixture.store->checkpoints.load(), 0);
        snapshot.positionMs = 90000;
        progress.observe(snapshot, true);
        QTRY_COMPARE(fixture.store->checkpoints.load(), 1);
        QCOMPARE(fixture.store->beginAttempts.load(), 3);
        const auto saved = std::get<std::optional<VodProgress>>(fixture.store->read(snapshot.ref, requestFor(fixture.source)));
        QVERIFY(saved); QCOMPARE(saved->positionMs, 90000);
        snapshot.positionMs = 100000;
        progress.observe(snapshot, true);
        QTRY_COMPARE(fixture.store->checkpoints.load(), 2);
        QCOMPARE(fixture.store->beginAttempts.load(), 3); // ownership is not reset on every checkpoint
    }
    void runnerBoundsGlobalAndPerSourceConcurrency()
    {
        VodJobRunner runner;
        const auto one = QUuid::createUuid();
        const auto two = QUuid::createUuid();
        std::atomic_bool release{false};
        std::atomic_int active{0}, activeOne{0}, activeTwo{0}, entered{0};
        int completions = 0;
        for (int index = 0; index < 8; ++index) {
            RequestContext request; request.deadline = QDeadlineTimer(3000);
            const auto source = index % 2 == 0 ? one : two;
            runner.submit(source, request, [&, source](RequestContext context) -> Result<JobReply> {
                auto &perSource = source == one ? activeOne : activeTwo;
                ++active; ++perSource; ++entered;
                while (!release && !context.interruption()) QThread::msleep(1);
                --active; --perSource;
                return JobReply{};
            }, [&](Result<JobReply>) { ++completions; });
        }
        QTRY_COMPARE(entered.load(), 4);
        QCOMPARE(active.load(), 4); QCOMPARE(activeOne.load(), 2); QCOMPARE(activeTwo.load(), 2);
        release = true;
        QTRY_COMPARE(completions, 8);
    }
    void destroyedRunnerDropsDeliveryAndOwnsWorkerInputs()
    {
        auto runner = std::make_unique<VodJobRunner>();
        auto entered = std::make_shared<std::atomic_bool>(false);
        auto left = std::make_shared<std::atomic_bool>(false);
        bool delivered = false;
        RequestContext request; request.deadline = QDeadlineTimer(3000);
        runner->submit(QUuid::createUuid(), request, [entered, left](RequestContext context) -> Result<JobReply> {
            entered->store(true);
            while (!context.interruption()) QThread::msleep(1);
            left->store(true);
            return JobReply{};
        }, [&](Result<JobReply>) { delivered = true; });
        QTRY_VERIFY(entered->load());
        runner.reset();
        QTRY_VERIFY(left->load());
        QVERIFY(!delivered);
    }
    void shutdownCancelsWorkersAndFlushesFinalPosition()
    {
        Fixture fixture;
        VodModule module({true}, [&]() { return fixture.composition(); });
        module.controller()->play(refFor(fixture.source));
        QTRY_VERIFY(fixture.engine != nullptr);
        QTRY_COMPARE(fixture.engine->loads, 1);
        fixture.engine->emitState(EngineState::Playing, 55000);
        module.shutdown();
        const auto saved = std::get<std::optional<VodProgress>>(fixture.store->read(refFor(fixture.source), requestFor(fixture.source)));
        QVERIFY(saved); QCOMPARE(saved->positionMs, 55000); QCOMPARE(saved->status, WatchStatus::InProgress);
        QCOMPARE(fixture.engine->stops, 1);
    }
};
QTEST_GUILESS_MAIN(VodContractTests)
#include "tst_vod_contracts.moc"
