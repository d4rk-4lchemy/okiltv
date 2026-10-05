#include <QtTest>
#include "app/playback/playbackbuffering.h"
#include "app/playback/playbackrecovery.h"
#include "app/playback/catchupplaybacksession.h"
#include "app/playback/catchupstandbytransition.h"
#include "player/mpvplayer.h"

using namespace OKILTV::App::Playback;

class PlaybackTests final : public QObject {
    Q_OBJECT
private slots:
    void reserveCommandsAndCancellation();
    void refillPreservesPauseAndEof();
    void liveCapacityTimeoutAndMissingTelemetry();
    void bitrateWindowAndRetune();
    void startupPolicies();
    void reconnectStopLoadLimitsAndCancellation();
    void reconnectReserveAndStabilization();
    void playbackBufferUsesPlaybackClockAndContinuousMedia();
    void playbackBufferAlignsMpegTsTimestampEpochs();
    void reconnectAcceptsHighPlateauAndDeliveryBursts();
    void reconnectDrainsReserveAndRejectsStaleOrFrozenPlayback();
    void watchdogsRespectContext();
    void catchupTimelineAndProgress();
    void seekQueuePublicationAndRetry();
    void rollbackDefersUntilCacheCoversTarget();
    void standbyRetryAndCancellation();
    void independentPlayers();
    void cutoverPreservesBufferedMedia();
    void alignmentPrefersVerifiedPeriod();
    void startupCancellationAndFallback();
};

void PlaybackTests::reserveCommandsAndCancellation()
{
    PlaybackBuffering buffer;
    QList<PauseCommand> commands;
    const auto record = [&](ReserveDecision decision) {
        if (decision.pause != PauseCommand::None) commands << decision.pause;
        return decision.pending;
    };
    QVERIFY(!record(buffer.beginLiveReserve(0.11, false, 3.0, 100)));
    QVERIFY(record(buffer.beginLiveReserve(0.1, false, 3.0, 101)));
    QVERIFY(record(buffer.advanceLiveReserve(2.9, false, false, 1000, 1101)));
    QVERIFY(!record(buffer.advanceLiveReserve(3.0, false, false, 1001, 1102)));
    QCOMPARE(commands, (QList<PauseCommand>{PauseCommand::Pause, PauseCommand::Resume}));
    QVERIFY(record(buffer.beginLiveReserve(0.0, false, 3.0, 1103)));
    QVERIFY(!record(buffer.resetLiveReserve(true, true)));
    QCOMPARE(commands.last(), PauseCommand::Pause);
    QVERIFY(!record(buffer.advanceLiveReserve(50.0, false, false, 30000, 31103)));
    QCOMPARE(commands.size(), 3);
}

void PlaybackTests::refillPreservesPauseAndEof()
{
    PlaybackBuffering buffer;
    QCOMPARE(buffer.catchupRefill(0.0, true, false, false, true, 0).pause, PauseCommand::Pause);
    QVERIFY(buffer.catchupRefill(0.5, true, false, false, false, 30001).pending);
    QCOMPARE(buffer.catchupRefill(2.0, true, false, false, false, 30002).pause, PauseCommand::Resume);
    QCOMPARE(buffer.catchupRefill(0.0, true, false, false, true, 31000).pause, PauseCommand::Pause);
    QCOMPARE(buffer.catchupRefill(0.0, true, true, true, false, 31001).pause, PauseCommand::None);
    QVERIFY(!buffer.catchupRefilling());
    QCOMPARE(buffer.catchupRefill(0.0, true, false, false, true, 32000).pause, PauseCommand::Pause);
    QCOMPARE(buffer.catchupRefill(0.0, true, true, false, false, 32001).pause, PauseCommand::Resume);
}

void PlaybackTests::liveCapacityTimeoutAndMissingTelemetry()
{
    PlaybackBuffering buffer;
    QVERIFY(!buffer.beginLiveReserve(std::nullopt, false, 3.0, 0).pending);
    QVERIFY(buffer.beginLiveReserve(0.0, false, 8.0, 1).pending);
    QVERIFY(buffer.advanceLiveReserve(2.0, true, false, 999, 1000).pending);
    QVERIFY(!buffer.advanceLiveReserve(2.0, true, false, 1000, 1001).pending);
    QVERIFY(!buffer.beginLiveReserve(0.0, false, 8.0, 11000).pending);
    QVERIFY(buffer.beginLiveReserve(0.0, false, 8.0, 11001).pending);
    QVERIFY(!buffer.advanceLiveReserve(std::nullopt, false, false, 18000, 29001).pending);
    buffer.resetAdaptation();
    QVERIFY(buffer.beginLiveReserve(0.0, false, 3.0, 30000).pending);
    QVERIFY(!buffer.advanceLiveReserve(0.2, false, true, 1, 30001).pending);
}

void PlaybackTests::bitrateWindowAndRetune()
{
    PlaybackBuffering buffer;
    QCOMPARE(buffer.observeBitrate(8000.0, 0).value(), 8000.0);
    QCOMPARE(buffer.observeBitrate(16000.0, 5000).value(), 12000.0);
    QCOMPARE(buffer.observeBitrate(24000.0, 5001).value(), 20000.0);
    QVERIFY(!buffer.observeBitrate(std::nullopt, 5002));
    const auto initial = buffer.retune(false, 3.0, 20.0, true, 6000);
    QVERIFY(initial);
    QCOMPARE(initial->refillSeconds.value(), 0.0);
    QVERIFY(!buffer.retune(false, 30.0, 20.0, true, 7999));
    QVERIFY(buffer.retune(false, 30.0, 20.0, true, 8000));
    buffer.observeBitrate(1e30, 8001);
    const auto archive = buffer.activeCatchupPolicy(8001);
    QCOMPARE(archive.maxBytes, 96LL * 1024 * 1024);
    QCOMPARE(archive.maxBackBytes, 32LL * 1024 * 1024);
}

void PlaybackTests::startupPolicies()
{
    using Policy = PlaybackBuffering::StartupPolicy;
    QCOMPARE(PlaybackBuffering::startupPolicy(false, true, false, false, false, false), Policy::FastLive);
    QCOMPARE(PlaybackBuffering::startupPolicy(true, true, false, false, false, false), Policy::BestEffort);
    QCOMPARE(PlaybackBuffering::startupPolicy(false, true, true, false, false, false), Policy::StrictBuffered);
    QCOMPARE(PlaybackBuffering::startupPolicy(false, true, false, true, false, false), Policy::StrictBuffered);
    QCOMPARE(PlaybackBuffering::startupPolicy(false, true, false, false, false, true), Policy::StrictBuffered);
}

void PlaybackTests::reconnectStopLoadLimitsAndCancellation()
{
    PlaybackRecovery recovery;
    using Command = PlaybackRecovery::Command;
    QCOMPARE(recovery.nextAttempt(0), Command::None);
    recovery.start();
    for (int attempt = 0; attempt < 5; ++attempt) {
        const qint64 now = attempt * 10000;
        QCOMPARE(recovery.nextAttempt(now), Command::Stop);
        QCOMPARE(recovery.nextAttempt(now + 449), Command::None);
        QCOMPARE(recovery.nextAttempt(now + 450), Command::Load);
        recovery.beginLoad(false, 3.0, now + 450);
        QCOMPARE(recovery.nextAttempt(now + 451), Command::None);
        QVERIFY(recovery.attemptTimeout(5000, 1000, now + 5449).isEmpty());
        QCOMPARE(recovery.attemptTimeout(5000, 1000, now + 5450), QStringLiteral("attempt-timeout"));
        recovery.clearAttempt();
    }
    QCOMPARE(recovery.nextAttempt(60000), Command::Exhausted);
    recovery.stop(true, 60000);
    QCOMPARE(recovery.nextAttempt(70000), Command::None);
    recovery.start();
    QCOMPARE(recovery.nextAttempt(70000), Command::Stop);
    recovery.stop(false, 70001);
    QCOMPARE(recovery.nextAttempt(80000), Command::None);
}

void PlaybackTests::reconnectReserveAndStabilization()
{
    PlaybackRecovery recovery;
    recovery.start();
    recovery.beginLoad(true, 7.5, 0);
    recovery.loaded(100);
    using Action = PlaybackRecovery::ReserveResult::Action;
    for (int i = 1; i <= 7; ++i) {
        QVERIFY(recovery.reserve(static_cast<double>(i), false, false, false, 5.0, i * 1000).holdSample);
        QCOMPARE(recovery.attempts(), 1);
    }
    QCOMPARE(recovery.reserve(7.5, false, false, false, 5.0, 8000).action, Action::Resume);
    RecoveryContext context;
    context.hasChannel = true;
    StreamHealth health{7.5, 5000.0, 3.0};
    QVERIFY(recovery.observeRecovery(health, context, true, true, 8001));
    QVERIFY(recovery.stabilizing());
    QVERIFY(recovery.attemptTimeout(5000, 1000, 20000).isEmpty());
    for (int i = 1; i <= 12; ++i) {
        health.cacheDurationSeconds = i % 2 == 0 ? 7.5 : 7.0;
        recovery.observeRecovery(health, context, true, true, 8001 + i * 1000);
    }
    QVERIFY(recovery.recovered());
    QCOMPARE(recovery.reserve(0.5, false, false, true, 5.0, 22000).action, Action::Pause);
    QVERIFY(!recovery.stabilizing());
    QCOMPARE(recovery.reserve(0.5, false, true, true, 5.0, 22001).action, Action::Stop);
    QVERIFY(!recovery.attemptInFlight());
}

void PlaybackTests::playbackBufferUsesPlaybackClockAndContinuousMedia()
{
    using Player = OKILTV::Player::MpvPlayer;
    QVariantMap state {{QStringLiteral("cache-duration"), 11.008},
        {QStringLiteral("reader-pts"), 1.365333}, {QStringLiteral("cache-end"), 12.373333},
        {QStringLiteral("raw-input-rate"), 1000000.0}};
    auto sample = Player::playbackBufferForCacheState(1.021333, state);
    QVERIFY(sample.seconds);
    QVERIFY(std::abs(*sample.seconds - 11.352) < 0.000001);
    QCOMPARE(sample.endSeconds.value(), 12.373333);
    const auto track = [](const QString &type, double end) {
        return QVariantMap {{QStringLiteral("type"), type}, {QStringLiteral("reader-pts"), 1.365333},
            {QStringLiteral("cache-end"), end}, {QStringLiteral("cache-duration"), end - 1.365333}};
    };
    state.insert(QStringLiteral("ts-per-stream"), QVariantList {
        track(QStringLiteral("video"), 15.0), track(QStringLiteral("audio"), 12.373333),
        track(QStringLiteral("sub"), 2.0)});
    sample = Player::playbackBufferForCacheState(1.021333, state);
    QVERIFY(!sample.estimated);
    QVERIFY(std::abs(*sample.seconds - 11.352) < 0.000001);
    // A stationary cache end drains as actual playback advances.
    QCOMPARE(Player::playbackBufferForCacheState(5.0, state).seconds.value(), 7.373333);
    // No hard ceiling: a larger observed cache is published as-is.
    state.insert(QStringLiteral("cache-duration"), 24.0 - 1.365333);
    state.insert(QStringLiteral("cache-end"), 24.0);
    state.insert(QStringLiteral("ts-per-stream"), QVariantList {track(QStringLiteral("video"), 25.0), track(QStringLiteral("audio"), 24.0)});
    QCOMPARE(Player::playbackBufferForCacheState(1.0, state).seconds.value(), 23.0);
    const auto range = [](double start, double end) {
        return QVariantMap {{QStringLiteral("start"), start}, {QStringLiteral("end"), end}};
    };
    state.insert(QStringLiteral("seekable-ranges"), QVariantList {range(8.0, 24.0), range(0.0, 4.0)});
    QCOMPARE(Player::playbackBufferForCacheState(1.0, state).seconds.value(), 3.0);
    QVERIFY(!Player::playbackBufferForCacheState(6.0, state).seconds);
    state.insert(QStringLiteral("seeking"), true);
    QVERIFY(!Player::playbackBufferForCacheState(1.0, state).seconds);
    const QVariantMap fallback {{QStringLiteral("cache-duration"), 11.008}};
    sample = Player::playbackBufferForCacheState(1.0, fallback);
    QVERIFY(sample.estimated);
    QCOMPARE(sample.seconds.value(), 11.008);
    QVERIFY(!Player::playbackBufferForCacheState(1.0, {}).seconds);
    state.remove(QStringLiteral("seeking"));
    state.insert(QStringLiteral("cache-duration"), 4096.0);
    QVERIFY(!Player::playbackBufferForCacheState(1.0, state).seconds);
}

void PlaybackTests::playbackBufferAlignsMpegTsTimestampEpochs()
{
    using Player = OKILTV::Player::MpvPlayer;
    const auto track = [](const QString &type, double reader, double end) {
        return QVariantMap {{QStringLiteral("type"), type}, {QStringLiteral("reader-pts"), reader},
            {QStringLiteral("cache-end"), end}, {QStringLiteral("cache-duration"), end - reader}};
    };
    // Real mpv cache states apply ts_offset only to aggregate clocks/ranges.
    // These represent the same reserve despite very different raw MPEG-TS PTS.
    for (const double epoch : {0.0, 0.02, 40000.0, 95440.0, -40000.0}) {
        QVariantMap state {{QStringLiteral("cache-duration"), 11.008},
            {QStringLiteral("reader-pts"), 1.365333}, {QStringLiteral("cache-end"), 12.373333},
            {QStringLiteral("seekable-ranges"), QVariantList {QVariantMap {
                {QStringLiteral("start"), 0.0}, {QStringLiteral("end"), 11.5}}}},
            {QStringLiteral("ts-per-stream"), QVariantList {
                track(QStringLiteral("video"), epoch + 1.4, epoch + 13.0),
                track(QStringLiteral("audio"), epoch + 1.365333, epoch + 12.373333)}}};
        auto sample = Player::playbackBufferForCacheState(1.021333, state);
        QVERIFY(sample.seconds);
        QVERIFY(!sample.estimated);
        QVERIFY(std::abs(*sample.seconds - 11.352) < 0.000001);
        QVERIFY(std::abs(*sample.endSeconds - 12.373333) < 0.000001);

        // Known delivery stops: the corrected end stays fixed while playback
        // consumes the reserve, rather than preserving a huge raw PTS offset.
        sample = Player::playbackBufferForCacheState(12.35, state);
        QVERIFY(sample.seconds);
        QVERIFY(*sample.seconds < 0.05);

        PlaybackRecovery recovery;
        recovery.start(); recovery.beginLoad(false, 3.0, 0); recovery.loaded(0);
        StreamHealth health;
        RecoveryContext context;
        health.bufferTargetSeconds = 3.0;
        health.cacheDurationSeconds = sample.seconds;
        health.cacheEndSeconds = sample.endSeconds;
        for (int i = 0; i <= 12; ++i) recovery.observeRecovery(health, context, true, true, i * 1000);
        QVERIFY(!recovery.stabilizing());
        QVERIFY(!recovery.recovered());
        QVERIFY(!recovery.attemptTimeout(10000, 1000, 20000).isEmpty());

        // Sparse subtitles may supply the aggregate clock but must not bound A/V.
        state.insert(QStringLiteral("reader-pts"), 2.0);
        state.insert(QStringLiteral("cache-end"), 2.5);
        state.insert(QStringLiteral("cache-duration"), 0.5);
        auto streams = state.value(QStringLiteral("ts-per-stream")).toList();
        streams.append(track(QStringLiteral("subtitle"), epoch + 2.0, epoch + 2.5));
        state.insert(QStringLiteral("ts-per-stream"), streams);
        sample = Player::playbackBufferForCacheState(1.021333, state);
        QVERIFY(sample.seconds);
        QVERIFY(!sample.estimated);
        QVERIFY(std::abs(*sample.seconds - 11.352) < 0.000001);
    }

    // Ambiguous or missing stream correspondence uses the already rebased
    // aggregate clock, explicitly estimated; it never guesses a huge reserve.
    QVariantMap ambiguous {{QStringLiteral("cache-duration"), 11.0},
        {QStringLiteral("reader-pts"), 1.0}, {QStringLiteral("cache-end"), 12.0},
        {QStringLiteral("ts-per-stream"), QVariantList {
            track(QStringLiteral("video"), 40001.0, 40012.0),
            track(QStringLiteral("audio"), 50001.0, 50012.0)}}};
    auto sample = Player::playbackBufferForCacheState(0.5, ambiguous);
    QVERIFY(sample.estimated);
    QCOMPARE(sample.seconds.value(), 11.5);
    ambiguous.insert(QStringLiteral("ts-per-stream"), QVariantList {
        track(QStringLiteral("video"), 40001.0, 40015.0)});
    sample = Player::playbackBufferForCacheState(0.5, ambiguous);
    QVERIFY(sample.estimated);
    QCOMPARE(sample.seconds.value(), 11.5);
}

void PlaybackTests::reconnectAcceptsHighPlateauAndDeliveryBursts()
{
    for (const bool bursts : {false, true}) {
        PlaybackRecovery recovery;
        recovery.start();
        recovery.beginLoad(false, 3.0, 0);
        RecoveryContext context;
        context.hasChannel = true;
        StreamHealth health {11.008, 0.0, 3.0};
        for (int i = 0; i <= 12; ++i) {
            if (bursts) {
                health.cacheDurationSeconds = 3.1 - (i % 3) * 0.7;
                health.cacheSpeedBytesPerSecond = i % 3 == 0 ? 1000000.0 : 0.0;
            }
            recovery.observeRecovery(health, context, true, true, i * 1000);
            QVERIFY(recovery.attemptTimeout(10000, 1000, i * 1000).isEmpty());
            QCOMPARE(recovery.recovered(), i == 12);
        }
    }
}

void PlaybackTests::reconnectDrainsReserveAndRejectsStaleOrFrozenPlayback()
{
    PlaybackRecovery recovery;
    recovery.start();
    recovery.beginLoad(false, 3.0, 0);
    RecoveryContext context;
    context.hasChannel = true;
    StreamHealth health {40.0, 0.0, 3.0};
    health.cacheEndSeconds = 100.0;
    for (int i = 0; i <= 20; ++i) {
        health.cacheDurationSeconds = 40.0 - i;
        recovery.observeRecovery(health, context, true, true, i * 1000);
        QVERIFY(!recovery.recovered());
        QVERIFY(recovery.attemptTimeout(10000, 1000, i * 1000).isEmpty());
    }
    health.sampleFresh = false;
    recovery.observeRecovery(health, context, true, true, 21000);
    QVERIFY(!recovery.recovered());
    QCOMPARE(recovery.attemptTimeout(10000, 1000, 21000), QStringLiteral("stabilization-timeout"));
    health.sampleFresh = true;
    for (int i = 22; i <= 24; ++i) recovery.observeRecovery(health, context, false, false, i * 1000);
    QVERIFY(recovery.unstable());

    recovery.clearAttempt();
    recovery.beginLoad(false, 3.0, 25000);
    health.cacheDurationSeconds = 11.008;
    recovery.observeRecovery(health, context, true, true, 25000);
    context.manuallyPaused = true;
    recovery.observeRecovery(health, context, false, false, 26000);
    context.manuallyPaused = false;
    recovery.observeRecovery(health, context, true, true, 60000);
    QVERIFY(!recovery.recovered());
    for (int i = 1; i <= 12; ++i) {
        health.cacheEndSeconds = 100.0 + i;
        recovery.observeRecovery(health, context, true, true, 60000 + i * 1000);
    }
    QVERIFY(recovery.recovered());
}

void PlaybackTests::watchdogsRespectContext()
{
    PlaybackRecovery recovery;
    RecoveryContext context;
    context.hasChannel = true;
    context.buffering = true;
    context.manuallyPaused = true;
    StreamHealth health{0.0, 0.0, 3.0};
    for (int i = 0; i < 10; ++i) QVERIFY(recovery.noRefill(health, context, false, false, i * 1000).isEmpty());
    context.manuallyPaused = false;
    QVERIFY(recovery.noRefill(health, context, false, false, 11000).isEmpty());
    QVERIFY(recovery.noRefill(health, context, false, false, 12000).isEmpty());
    QCOMPARE(recovery.noRefill(health, context, false, false, 13000), QStringLiteral("no-refill-watchdog"));
    context.catchup = true;
    QVERIFY(recovery.noRefill(health, context, false, false, 14000).isEmpty());
    context.catchup = false;
    health.cacheDurationSeconds = 10.0;
    for (int i = 0; i < 10; ++i) QVERIFY(recovery.videoFreeze(health, context, false, std::nullopt, 15000 + i).isEmpty());
}

void PlaybackTests::catchupTimelineAndProgress()
{
    CatchupPlaybackSession session;
    const auto start = QDateTime::fromSecsSinceEpoch(1000000, QTimeZone::UTC);
    session.setProgramStartUtc(start);
    session.setProgramStopUtc(start.addSecs(3600));
    session.setEndless(true);
    session.setTimelinePositionSeconds(200.0);
    session.setStreamBaseOffsetSeconds(120.0);
    QVERIFY(session.syncTimeline(true, start.addSecs(1000)));
    QCOMPARE(session.timelineAvailableSeconds(), 820.0);
    QVERIFY(!session.observeProgress(80.0, start.addSecs(1000)));
    session.setProgressTransportReady(true);
    session.setProgressSeekTargetSeconds(600.0);
    QVERIFY(!session.observeProgress(80.0, start.addSecs(1000)));
    QCOMPARE(session.observeProgress(480.0, start.addSecs(1000)).value(), start.addSecs(600));
    OKILTV::Core::EpgEntry program;
    program.start = start.addSecs(300);
    program.stop = start.addSecs(900);
    program.title = QStringLiteral("Next programme");
    QVERIFY(session.updateProgramme(true, program));
    QCOMPARE(session.streamBaseOffsetSeconds(), 120.0);
    QCOMPARE(session.catchupTimelineStartEpochMs(), program.start.toMSecsSinceEpoch());
}

void PlaybackTests::seekQueuePublicationAndRetry()
{
    CatchupPlaybackSession session;
    QVERIFY(session.beginReload(120.0, QStringLiteral("first"), 120.0));
    QVERIFY(!session.beginReload(240.0, QStringLiteral("second"), 240.0));
    QVERIFY(!session.beginReload(300.0, QStringLiteral("last"), 300.0));
    QCOMPARE(session.reloadUrl(), QStringLiteral("first"));
    QCOMPARE(session.queuedSeek().value(), 300.0);
    session.finishReload();
    QVERIFY(!session.reloadInFlight());
    QCOMPARE(session.queuedSeek().value(), 300.0);
    session.cancelReload();
    QVERIFY(!session.queuedSeek());
    QVERIFY(!session.reloadAckTimer().isActive());
    session.waitForPublication(100);
    QVERIFY(!session.publicationDue(5099));
    QVERIFY(session.publicationDue(5100));
    QVERIFY(session.allowContinuation(600));
    QVERIFY(session.allowContinuation(600));
    QVERIFY(session.allowContinuation(600));
    QVERIFY(!session.allowContinuation(600));
    QVERIFY(session.allowContinuation(602));
    session.resetContinuation();
    QVERIFY(!session.publicationWaiting());
}

void PlaybackTests::rollbackDefersUntilCacheCoversTarget()
{
    CatchupPlaybackSession session;
    session.resetRollbackGuard(true, 0);
    QVERIFY(!session.correctRollback(120.0, true, true, QPair<double,double>{0.0, 121.0}, 100));
    QVERIFY(!session.correctRollback(0.0, true, true, QPair<double,double>{0.0, 20.0}, 200));
    QVERIFY(session.rollbackDeferred());
    QCOMPARE(session.correctRollback(1.0, true, true, QPair<double,double>{0.0, 130.0}, 300).value(), 120.0);
    QVERIFY(!session.rollbackDeferred());
    session.clearRollbackGuard();
    QVERIFY(!session.correctRollback(0.0, true, true, std::nullopt, 400));
}

void PlaybackTests::standbyRetryAndCancellation()
{
    CatchupStandbyTransition standby;
    standby.arm(QStringLiteral("archive"), 120.0);
    const auto first = standby.generation();
    QVERIFY(standby.allowAttempt(0));
    standby.beginStop();
    QVERIFY(standby.stopPending());
    standby.acknowledgeStop();
    standby.loaded(100);
    standby.failed(200);
    QVERIFY(!standby.allowAttempt(449));
    QVERIFY(standby.allowAttempt(450));
    QCOMPARE(standby.retryBudget(), 1);
    standby.loaded(450);
    standby.failed(500);
    QVERIFY(standby.allowAttempt(750));
    standby.loaded(750);
    standby.failed(800);
    QVERIFY(!standby.allowAttempt(1050));
    standby.cancel();
    QVERIFY(!standby.accepts(first));
    QVERIFY(!standby.pending());
    QVERIFY(!standby.retryTimer().isActive());
    standby.arm(QStringLiteral("second"), 240.0);
    QVERIFY(!standby.accepts(first));
    QVERIFY(standby.allowAttempt(5750));
    standby.loaded(5750);
    standby.markReady();
    QVERIFY(standby.ready());
    QVERIFY(!standby.videoReady());
    standby.markVideoReady();
    QVERIFY(standby.videoReady());
}

void PlaybackTests::independentPlayers()
{
    PlaybackBuffering primary, pip;
    primary.beginLiveReserve(0.0, false, 3.0, 0);
    QVERIFY(!pip.liveReservePending());
    PlaybackRecovery first, second;
    first.start();
    QVERIFY(!second.active());
    CatchupPlaybackSession archive, other;
    archive.beginReload(0.0, QStringLiteral("url"), 0.0);
    QVERIFY(!other.reloadInFlight());
}
void PlaybackTests::cutoverPreservesBufferedMedia()
{
    CatchupStandbyTransition standby;
    standby.arm(QStringLiteral("url"), 120.0);
    standby.loaded(0);
    standby.markReady();
    standby.markVideoReady();
    using Action = CatchupStandbyTransition::CutoverDecision::Action;
    CatchupStandbyTransition::CutoverSample sample;
    sample.activeCache = 15.0;
    sample.remainingSeconds = -100.0;
    QCOMPARE(standby.evaluateCutover(sample).action, Action::Wait);
    sample.activeCache = 0.3;
    sample.alignToWatched = true;
    sample.watchedSeconds = 180.0;
    sample.standbyPosition = 0.0;
    sample.standbyRange = QPair<double,double>{0.0, 50.0};
    QCOMPARE(standby.evaluateCutover(sample).action, Action::Wait);
    sample.standbyExhausted = true;
    QCOMPARE(standby.evaluateCutover(sample).action, Action::Reject);
    sample.standbyRange = QPair<double,double>{0.0, 80.0};
    const auto seek = standby.evaluateCutover(sample);
    QCOMPARE(seek.action, Action::Seek);
    QCOMPARE(seek.target, 60.0);
    QCOMPARE(standby.evaluateCutover(sample).action, Action::Wait);
    sample.standbyPosition = 60.0;
    QCOMPARE(standby.evaluateCutover(sample).action, Action::Commit);
    standby.cancel();
    QCOMPARE(standby.evaluateCutover(sample).action, Action::Wait);
}

void PlaybackTests::alignmentPrefersVerifiedPeriod()
{
    CatchupPlaybackSession session;
    session.setStreamBaseOffsetSeconds(120.0);
    session.setReconnectResumeStreamRelativeSeconds(60.0);
    session.setAlignmentActive(true);
    using Action = CatchupPlaybackSession::AlignmentDecision::Action;
    QCOMPARE(session.alignRecovery(10.0, QPair<double,double>{0.0, 20.0}, false, true, 181.0, 0).action,
             Action::NextPeriod);
    session.setAlignmentActive(false);
    QCOMPARE(session.alignRecovery(10.0, std::nullopt, true, true, std::nullopt, 1).action, Action::Inactive);
    session.setAlignmentActive(true);
    QCOMPARE(session.alignRecovery(0.0, QPair<double,double>{0.0, 61.0}, false, false, std::nullopt, 2).action,
             Action::Seek);
    QCOMPARE(session.alignRecovery(60.0, QPair<double,double>{0.0, 61.0}, false, false, std::nullopt, 3).action,
             Action::Complete);
    QVERIFY(!session.reconnectResumeStreamRelativeSeconds());
}

void PlaybackTests::startupCancellationAndFallback()
{
    PlaybackBuffering buffer;
    buffer.setStartupPending(true);
    buffer.armStartup(3.0, 2);
    QCOMPARE(buffer.startupFallbackTimer().interval(), 6000);
    QVERIFY(!buffer.startupReady(std::nullopt));
    QVERIFY(!buffer.startupReady(2.9));
    QVERIFY(buffer.startupReady(3.0));
    buffer.applyStartupFallback();
    QVERIFY(!buffer.startupPending());
    QVERIFY(buffer.startupFallbackApplied());
    buffer.resetStartupWatchdog(true);
    QVERIFY(!buffer.startupFallbackTimer().isActive());
    QVERIFY(!buffer.startupFallbackApplied());
}

QTEST_GUILESS_MAIN(PlaybackTests)
#include "tst_playback_components.moc"
