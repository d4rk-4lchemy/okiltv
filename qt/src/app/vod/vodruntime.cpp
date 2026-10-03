#include "app/playback/trackpresentation.h"
#include "vodruntime.h"
#include "vodmediaprobe.h"
#include "app/multiviewcontroller.h"
#include "app/dvrcontroller.h"
#include "app/timeshiftcontroller.h"
#include "core/settingsmanager.h"
#include "core/sourcestore.h"
#include "core/appdatapaths.h"
#include "core/vod/providers/xtreamvodprovider.h"
#include "player/mpvplaybackengine.h"
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QScopedValueRollback>
#include <QTimer>
#include <QtConcurrentRun>
#include <cmath>

namespace OKILTV::Vod {
namespace {
using Completion = PlaybackCoordinator::Completion;
Outcome waitFor(const std::function<void(Completion)> &start)
{
    // Source forms have a synchronous save contract. Keep painting/backend acks
    // responsive while excluding input; SettingsManager rejects reentrant edits.
    QEventLoop loop;
    QPointer<QEventLoop> waiter(&loop);
    auto result = std::make_shared<std::optional<Outcome>>();
    const Completion completion = [result, waiter](Outcome outcome) { *result = outcome; if (waiter) waiter->quit(); };
    start(completion);
    // The callback copy belongs to the asynchronous continuation, including
    // after timeout; std::function releases its shared state on completion.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    if (!*result) {
        QTimer::singleShot(12000, &loop, &QEventLoop::quit);
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    return result->value_or(Outcome{Error{ErrorCode::Timeout, {}}});
}
QFuture<Outcome> work(QObject *owner, std::function<Outcome()> operation, Completion completion)
{
    auto *watcher = new QFutureWatcher<Outcome>(owner);
    QObject::connect(watcher, &QFutureWatcher<Outcome>::finished, owner, [watcher, completion = std::move(completion)]() {
        const auto result = watcher->result(); watcher->deleteLater(); completion(result);
    });
    const auto future = QtConcurrent::run([operation = std::move(operation)]() -> Outcome {
        try { return operation(); } catch (...) { return Error{ErrorCode::StorageUnavailable, {}}; }
    });
    watcher->setFuture(future);
    return future;
}
}
VodRuntime::VodRuntime(Core::SettingsManager *settings, App::MultiViewController *multiview,
    App::DvrController *dvr, App::TimeshiftController *timeshift, QObject *parent, EngineFactory engineFactory)
    : QObject(parent), m_settings(settings), m_multiview(multiview), m_dvr(dvr), m_timeshift(timeshift)
{
    m_module = std::make_unique<VodModule>();
    if (settings->current().vodEnabled) initialize(std::move(engineFactory));
    else m_deferredFactory = std::move(engineFactory);
}
void VodRuntime::enableForSession(const QUuid &profile)
{
    if (m_stopped) return;
    { QMutexLocker lock(&m_sessionSources->mutex); m_sessionSources->enabled.insert(profile); }
    if (!m_module->enabled()) initialize(std::move(m_deferredFactory));
}
void VodRuntime::initialize(EngineFactory engineFactory)
{
    const auto directory = QFileInfo(m_settings->settingsFilePath()).dir();
    m_artwork = std::make_shared<VodArtworkCache>(Core::AppDataPaths::vodArtworkDirectory(directory.absolutePath()));

    const Core::SourceStore sources(directory.filePath(QStringLiteral("source-summaries.json")), directory.filePath(QStringLiteral("sources")));
    m_store = std::make_shared<SqliteVodStore>(directory.filePath(QStringLiteral("iptv.db")), [sources, session = m_sessionSources](const QUuid &id) -> Result<SourceContext> {
        try {
            const auto summaries = sources.loadSummaries();
            if (std::none_of(summaries.cbegin(), summaries.cend(), [&](const Core::SourceSummary &summary) { return summary.id == id; }))
                return Error{ErrorCode::ContentUnavailable, {}};
            const auto profile = sources.loadDetail(id);
            if (!profile) return Error{ErrorCode::ContentUnavailable, {}};
            SourceContext source;
            source.revision = {id, {}, profile->vodCredentialRevision};
            bool enabledForSession = false;
            { QMutexLocker lock(&session->mutex); enabledForSession = session->enabled.contains(id); }
            source.enabled = (profile->vodEnabled || enabledForSession) && profile->type == Core::ProfileType::Xtream;
            source.provider = QStringLiteral("xtream");
            source.endpoint = QUrl(profile->xtreamBaseUrl);
            source.username = profile->xtreamUsername; source.password = profile->xtreamPassword;
            return source;
        } catch (...) { return Error{ErrorCode::SecretUnavailable, {}}; }
    });
    auto legacy = std::make_shared<LegacyPlaybackAdapter>(LegacyPlaybackAdapter::Hooks{
        [this]() { return LegacyResources{m_multiview->hasRecordingForHandoff(), m_dvr->hasRecordingDemand(), !m_settings->current().dvrStopVodBeforeRecording}; },
        [this](const QUuid &id, LegacyPlaybackAdapter::Acknowledgement done) {
            m_timeshift->handleUserStopRequest();
            m_multiview->releasePlaybackForHandoff(id, [done = std::move(done)](bool success) {
                if (success) done(Success{}); else done(Error{ErrorCode::PlaybackConflict, {}});
            });
        },
        [this]() {
            auto action = std::move(m_liveAction); m_liveAction = {};
            QScopedValueRollback approved(m_approvedLive, true);
            if (action) action();
            emit stateChanged();
        }});
    m_module = std::make_unique<VodModule>(VodOptions{true}, [this, legacy, engineFactory = std::move(engineFactory)]() {
        VodComposition composition;
        auto provider = std::make_shared<XtreamVodProvider>(std::make_shared<QtHttpTransport>(), m_settings->current().vodSeriesEnabled);
        const auto artwork = m_artwork;
        provider->registerArtwork = [artwork](const SourceContext &source, const QUrl &url, const RequestContext &context) {
            return artwork->remember(source, url, context);
        };
        composition.dependencies = {provider, m_store, m_store, m_store, m_store,
            [artwork](const QUuid &profile, const ArtworkRef &ref, const RequestContext &context) { return artwork->resolve(profile, ref, context); },
            [](const PlaybackDescriptor &descriptor, const RequestContext &context) { return probeVodMedia(descriptor, context); },
            [artwork](const QUuid &profile, const ArtworkRef &ref, const RequestContext &context) { return artwork->cached(profile, ref, context); }, m_store};
        composition.legacy = legacy;
        composition.createEngine = [this]() {
            auto backend = std::make_unique<Player::MpvPlayer>();
            const auto &settings = m_settings->current();
            backend->configureLibraryPath(settings.mpvDllPath);
            backend->configureOptions(settings.mpvOptions);
            backend->configurePlaybackTuning(settings.playerWaitForStreamSeconds, settings.playerDeinterlaceEnabled, settings.playerBufferSeconds);
            backend->configureUserAgent(settings.playerUserAgent);
            backend->configureImageSmoothing(settings.playerImageSmoothingEnabled);
            backend->configurePicturePreset(settings.playerPicturePreset);
            backend->setVolume(static_cast<int>(volume()));
            return std::make_unique<Player::MpvPlaybackEngine>(std::move(backend), true);
        };
        if (engineFactory) composition.createEngine = engineFactory;
        return composition;
    });
    m_module->changed = [this](const SessionSnapshot &snapshot) {
        emit stateChanged();
        updatePlaybackMetadata(snapshot);
        if (snapshot.state == SessionState::Failed) {
            const auto message = tr("The movie could not be played. Press V to return to the library and try again.");
            emit errorOccurred(message);
            emit notification(message);
        }
    };
    m_module->progressFailed = [this](const Error &error) { emit errorOccurred(error.message()); };
    m_module->controller()->completed = [this](const VodEvent &event) {
        if (m_removals.contains(event.operationId)) {
            auto done = m_removals.take(event.operationId);
            if (const auto *error = std::get_if<Error>(&event.result)) done(*error); else done(Success{});
        }
    };
    m_dvr->prepareRecordingStart = [this](std::function<void(bool)> done) { prepareRecording(std::move(done)); };
    m_dvr->automaticPlaybackHandoffAllowed = [this]() {
        return !active() && m_module->coordinator()->owner() == PlaybackOwner::Legacy;
    };
    bindPlayer(m_multiview->primaryController());
    connect(m_multiview, &App::MultiViewController::playbackSessionCreated, this, &VodRuntime::bindPlayer);
    connect(m_multiview, &App::MultiViewController::primaryControllerChanged, this, [this]() { bindPlayer(m_multiview->primaryController()); });
    m_multiview->playbackStartGate = [this](std::function<void()> action) { return gate(m_multiview, std::move(action)); };
    m_settings->prepareProfileMutation = [this](const QUuid &id, const Core::ServerProfile *replacement, QString *error) {
        return prepareMutation(id, replacement, error);
    };
    m_settings->profileMutationFinished = [this](const QUuid &id, bool) { finishMutation(id); };
    reconcileSources();
}
void VodRuntime::retryInitialization()
{
    if (!m_stopped && m_module->enabled() && !m_ready && !m_reconcileWork.isRunning()) reconcileSources();
}
void VodRuntime::reconcileSources()
{
    QList<QUuid> existing;
    for (const auto &source : m_settings->sourceSummaries()) { existing.append(source.id); m_module->controller()->blockSource(source.id); }
    auto *reconcile = new QFutureWatcher<Result<QList<QUuid>>>(this);
    connect(reconcile, &QFutureWatcher<Result<QList<QUuid>>>::finished, this, [this, reconcile, existing]() {
        const auto result = reconcile->result(); reconcile->deleteLater();
        if (m_stopped) return;
        if (const auto *error = std::get_if<Error>(&result)) { emit errorOccurred(error->message()); return; }
        QScopedValueRollback reconciling(m_reconciling, true);
        for (const auto &id : std::get<QList<QUuid>>(result)) {
            if (m_settings->profileById(id) && !m_settings->removeProfile(id)) {
                emit errorOccurred(m_settings->lastSaveError()); return;
            }
        }
        for (const auto &id : existing) m_module->controller()->unblockSource(id);
        m_ready = true;
        emit sourcesReconciled(); emit stateChanged();
    });
    const auto store = m_store;
    const auto artwork = m_artwork;
    m_reconcileWork = QtConcurrent::run([store, artwork, existing]() -> Result<QList<QUuid>> {
        try {
            artwork->retainSources(existing);
            return store->reconcileRemovedSources(existing);
        } catch (...) { return Error{ErrorCode::StorageUnavailable, {}}; }
    });
    reconcile->setFuture(m_reconcileWork);
}
VodRuntime::~VodRuntime() { shutdown(); }
bool VodRuntime::active() const { return m_module->enabled() && m_module->session()->snapshot().ref.playable() && !m_module->session()->snapshot().end; }
bool VodRuntime::isPlaying() const { return active() && m_module->session()->snapshot().state == SessionState::Playing; }
bool VodRuntime::isPaused() const { return active() && m_module->session()->snapshot().pauseRequested; }
bool VodRuntime::isLoading() const
{
    if (!active()) return false;
    const auto &snapshot = m_module->session()->snapshot();
    return snapshot.state == SessionState::Opening || snapshot.state == SessionState::SeekingResume
        || snapshot.state == SessionState::Recovering || (snapshot.buffering && !snapshot.pauseRequested);
}
QObject *VodRuntime::playerObject() const { return active() && m_module->session()->engine() ? static_cast<Player::MpvPlayer *>(m_module->session()->engine()->renderHandle()) : nullptr; }
double VodRuntime::volume() const { return m_multiview->primaryController()->volume(); }
double VodRuntime::positionSeconds() const { return active() ? static_cast<double>(m_module->session()->snapshot().positionMs) / 1000.0 : 0; }
double VodRuntime::durationSeconds() const
{
    if (!active()) return -1;
    const auto duration = m_module->session()->snapshot().durationMs;
    return duration ? static_cast<double>(*duration) / 1000.0 : -1;
}
bool VodRuntime::seekable() const { return active() && m_module->session()->snapshot().seekable; }
QVariantList VodRuntime::audioTracks() const
{
    return App::Playback::audioTracks(qobject_cast<Player::MpvPlayer *>(playerObject()));
}
QVariantList VodRuntime::subtitleTracks() const
{
    return App::Playback::subtitleTracks(qobject_cast<Player::MpvPlayer *>(playerObject()));
}
void VodRuntime::selectAudioTrack(int id)
{
    if (auto *backend = qobject_cast<Player::MpvPlayer *>(playerObject())) backend->selectAudioTrack(id, true);
}
void VodRuntime::selectSubtitleTrack(int id)
{
    if (auto *backend = qobject_cast<Player::MpvPlayer *>(playerObject())) backend->selectSubtitleTrack(id, true);
}
std::optional<VodMediaProbe> VodRuntime::playbackMetadata(const ContentRef &ref) const
{
    return ref == m_metadataRef ? m_metadata : std::nullopt;
}
void VodRuntime::updatePlaybackMetadata(const SessionSnapshot &snapshot)
{
    if (snapshot.end || snapshot.loadToken.isNull() || snapshot.loadToken == m_metadataLoad
        || !snapshot.videoWidth || !snapshot.videoHeight || snapshot.tracks.isEmpty()) return;
    VodMediaProbe metadata;
    metadata.videoWidth = snapshot.videoWidth;
    metadata.videoHeight = snapshot.videoHeight;
    metadata.observedAtUtc = QDateTime::currentDateTimeUtc();
    for (const auto &entry : snapshot.tracks) {
        const auto track = entry.toMap();
        const auto type = track.value(QStringLiteral("type")).toString();
        if (type != QLatin1String("audio") && type != QLatin1String("sub")) continue;
        auto &tracks = type == QLatin1String("audio") ? metadata.audioTracks : metadata.subtitleTracks;
        VodMediaTrack value;
        value.streamIndex = track.value(QStringLiteral("id")).toInt();
        value.ordinal = static_cast<int>(tracks.size());
        value.type = type;
        value.codec = track.value(QStringLiteral("codec")).toString();
        value.title = track.value(QStringLiteral("title")).toString();
        value.language = track.value(QStringLiteral("lang")).toString();
        value.isDefault = track.value(QStringLiteral("default")).toBool();
        value.forced = track.value(QStringLiteral("forced")).toBool();
        tracks.append(value);
    }
    m_metadataLoad = snapshot.loadToken;
    m_metadataRef = snapshot.ref;
    m_metadata = metadata;
    m_module->controller()->cachePlaybackMetadata(snapshot.ref, metadata, snapshot.durationMs);
    emit playbackMetadataChanged(snapshot.ref);
}
QVariantMap VodRuntime::debugOverlaySnapshot() const
{
    const auto *backend = qobject_cast<Player::MpvPlayer *>(playerObject());
    if (!backend) return {};
    using Presentation = App::PlayerController;
    const auto &ref = m_module->session()->snapshot().ref;
    const auto profile = m_settings->profileById(ref.profileId);
    const auto width = backend->videoWidth();
    const auto height = backend->videoHeight();
    const auto interlaced = backend->isInterlaced();
    const auto fps = backend->deinterlaceEnabled() && !interlaced.value_or(true)
        ? backend->sourceFrameRateFps() : backend->estimatedFrameRateFps();
    const auto cache = backend->demuxerCacheDurationSeconds().value_or(-1.0);
    const auto volume = backend->volumePercent();
    double bitrate = 0;
    bool hasBitrate = false;
    for (const auto value : {backend->videoBitrateBitsPerSecond(), backend->audioBitrateBitsPerSecond()}) {
        if (value && std::isfinite(*value) && *value >= 0) { bitrate += *value; hasBitrate = true; }
    }
    int videoCount = 0, audioCount = 0, subtitleCount = 0;
    for (const auto &track : backend->trackList()) {
        const auto type = track.toMap().value(QStringLiteral("type")).toString();
        if (type == QLatin1String("video")) ++videoCount;
        else if (type == QLatin1String("audio")) ++audioCount;
        else if (type == QLatin1String("sub")) ++subtitleCount;
    }
    return {
        {QStringLiteral("streamHost"), profile ? Presentation::debugStreamHostFromUrl(profile->xtreamBaseUrl) : QStringLiteral("N/A")},
        {QStringLiteral("streamId"), ref.providerItemId},
        {QStringLiteral("streamsText"), QStringLiteral("v(%1) a(%2) s(%3)").arg(videoCount).arg(audioCount).arg(subtitleCount)},
        {QStringLiteral("sourceResolution"), width && height ? QStringLiteral("%1x%2").arg(*width).arg(*height) : QStringLiteral("N/A")},
        {QStringLiteral("scanningText"), interlaced ? (*interlaced ? QStringLiteral("Interlaced") : QStringLiteral("Progressive")) : QStringLiteral("N/A")},
        {QStringLiteral("volumeText"), volume && std::isfinite(*volume) ? QStringLiteral("%1%").arg(std::round(std::clamp(*volume, 0.0, 100.0)), 0, 'f', 0) : QStringLiteral("N/A")},
        {QStringLiteral("videoCodec"), backend->videoCodec().value_or(QStringLiteral("N/A"))},
        {QStringLiteral("audioCodec"), backend->audioCodec().value_or(QStringLiteral("N/A"))},
        {QStringLiteral("frameRateText"), QStringLiteral("%1 (dropped frames: %2)").arg(Presentation::formatDebugFramerate(fps.value_or(-1.0))).arg(std::max(0, backend->droppedFrameCount().value_or(0)))},
        {QStringLiteral("bitrateText"), Presentation::formatDebugBitrate(hasBitrate ? bitrate : -1.0)},
        {QStringLiteral("bitrateValueKbps"), hasBitrate ? bitrate / 1000.0 : -1.0},
        {QStringLiteral("bufferDurationSeconds"), cache},
        {QStringLiteral("bufferDurationText"), Presentation::formatDebugBufferDuration(cache)},
        {QStringLiteral("bufferDurationSourceText"), QStringLiteral("mpv cache")},
        {QStringLiteral("mpvBufferDurationText"), Presentation::formatDebugBufferDuration(cache)},
        {QStringLiteral("minBufferNeededSeconds"), backend->bufferTargetSeconds()},
        {QStringLiteral("timeshiftMode"), QStringLiteral("Off (VOD)")},
        {QStringLiteral("timestamp"), Presentation::debugTimestampNowLocal()}
    };
}
void VodRuntime::togglePause() { if (active()) { if (isPaused()) m_module->session()->resume(); else m_module->session()->pause(); } }
void VodRuntime::stop() { if (active()) m_module->session()->stop(); }
void VodRuntime::seekRelative(double seconds) { if (active() && std::isfinite(seconds)) m_module->session()->seek(static_cast<qint64>(std::max(0.0, positionSeconds() + std::clamp(seconds, -86400.0, 86400.0)) * 1000.0)); }
QString VodRuntime::title() const
{
    return active() && m_module->session()->snapshot().ref == m_titleRef ? m_title : QString{};
}
QString VodRuntime::playbackYear() const
{
    return active() && m_module->session()->snapshot().ref == m_titleRef ? m_year : QString{};
}
void VodRuntime::setPlaybackTitle(const ContentRef &ref, const QString &title, const QString &year)
{
    if (!active() || m_module->session()->snapshot().ref != ref) return;
    m_titleRef = ref;
    m_title = title;
    m_year = year;
    emit stateChanged();
}
void VodRuntime::seekTo(double seconds)
{
    const double duration = durationSeconds();
    if (!seekable() || !std::isfinite(seconds) || duration <= 0) return;
    m_module->session()->seek(static_cast<qint64>(std::clamp(seconds, 0.0, duration) * 1000.0));
}
void VodRuntime::setVolume(double volume) { m_multiview->primaryController()->setVolume(volume); }
void VodRuntime::toggleMute() { m_multiview->primaryController()->toggleMute(); }
void VodRuntime::applyPictureSettings()
{
    const auto *session = m_module->session();
    if (!session || !session->engine()) return;
    auto *backend = static_cast<Player::MpvPlayer *>(session->engine()->renderHandle());
    if (!backend) return;
    backend->configurePicturePreset(m_settings->current().playerPicturePreset);
    backend->configureImageSmoothing(m_settings->current().playerImageSmoothingEnabled);
}
void VodRuntime::bindPlayer(App::PlayerController *player)
{
    if (!player || m_players.contains(player)) return;
    m_players.append(player);
    player->playbackStartGate = [this, player](std::function<void()> action) { return gate(player, std::move(action)); };
    connect(player, &App::PlayerController::volumeChanged, this, [this]() {
        if (m_module->session()->engine()) m_module->session()->setVolume(volume());
        emit stateChanged();
    });
}
bool VodRuntime::gate(QObject *owner, std::function<void()> action)
{
    if (m_approvedLive) return false;
    if (m_stopped) return true;
    if (!m_module->enabled()) return false;
    m_module->controller()->cancelPendingPlayback();
    if (m_module->coordinator()->owner() == PlaybackOwner::Legacy) return false;
    QPointer<QObject> receiver(owner);
    m_liveAction = [receiver, action = std::move(action)]() { if (receiver) action(); };
    m_module->coordinator()->requestLive();
    return true;
}
bool VodRuntime::prepareMutation(const QUuid &id, const Core::ServerProfile *replacement, QString *error)
{
    if (m_reconciling) return true;
    if (!m_ready || m_stopped) { if (error) *error = QStringLiteral("VOD storage is not ready. Source changes were not saved."); return false; }
    if (m_preparingEdits.contains(id) || m_finishingEdits.contains(id)) {
        if (error) *error = QStringLiteral("VOD source change is still completing. Please retry.");
        return false;
    }
    emit sourceInvalidated(id);
    if (replacement) m_preparingEdits.insert(id);
    const auto revision = replacement ? replacement->vodCredentialRevision : 0;
    const auto outcome = waitFor([this, id, revision, replacement](Completion done) {
        if (!replacement) {
            m_module->controller()->blockSource(id);
            const auto operation = m_module->controller()->removeSource(id);
            m_removals.insert(operation, [this, id, done](Outcome result) {
                if (std::holds_alternative<Success>(result)) {
                    const auto artwork = m_artwork;
                    m_sourceWork.addFuture(work(this, [artwork, id]() -> Outcome { artwork->removeSource(id); return Success{}; }, [](Outcome) {}));
                }
                done(result);
                QTimer::singleShot(0, this, [this, id]() { emit sourceUpdated(id); });
            });
            return;
        }
        auto preparedEdit = [this, id, done = std::move(done)](Outcome result) {
            m_preparingEdits.remove(id);
            done(result);
            // A synchronous form may already have timed out. Do not reopen the
            // source until this worker and its durable barrier have completed.
            if (m_editCompletionRequested.contains(id)) finishMutation(id);
        };
        m_module->prepareSourceChange(id, [this, id, revision, done = std::move(preparedEdit)](Outcome prepared) {
            if (const auto *error = std::get_if<Error>(&prepared)) { done(*error); return; }
            const auto store = m_store;
            m_sourceWork.addFuture(work(this, [store, id, revision]() { return store->advanceCredentialRevision(id, revision); }, done));
        });
    });
    if (const auto *failure = std::get_if<Error>(&outcome)) { if (error) *error = failure->message(); return false; }
    return true;
}
void VodRuntime::prepareRecording(std::function<void(bool)> done)
{
    if (m_stopped) { done(false); return; }
    if (!m_settings->current().dvrStopVodBeforeRecording) { done(true); return; }
    m_module->controller()->cancelPendingPlayback();
    if (m_recordingRetryPending && !m_settings->profileById(m_module->session()->snapshot().ref.profileId))
        m_recordingRetryPending = false; // explicit source removal also removes its history
    if (!active() && !m_recordingRetryPending && m_recordingWaiters.isEmpty()) { done(true); return; }
    const bool preparing = !m_recordingWaiters.isEmpty();
    m_recordingWaiters.append(std::move(done));
    if (preparing) return;
    const bool stoppedVod = active();
    m_module->prepareRecording([this, stoppedVod](Outcome result) {
        m_recordingRetryPending = std::holds_alternative<Error>(result);
        auto waiters = std::move(m_recordingWaiters);
        m_recordingWaiters.clear();
        if (!m_stopped) {
            if (stoppedVod && !active()) emit notification(tr("VOD playback stopped because a DVR recording is starting."));
            if (const auto *error = std::get_if<Error>(&result)) emit errorOccurred(error->message());
        }
        for (const auto &waiter : waiters) waiter(!m_stopped && std::holds_alternative<Success>(result));
    });
}
void VodRuntime::finishMutation(const QUuid &id)
{
    if (m_stopped || m_reconciling || !m_ready) return;
    m_editCompletionRequested.insert(id);
    if (m_preparingEdits.contains(id) || m_finishingEdits.contains(id)) return;
    m_editCompletionRequested.remove(id);
    m_finishingEdits.insert(id);
    const auto store = m_store;
    m_sourceWork.addFuture(work(this, [store, id]() { return store->finishCredentialChange(id); }, [this, id](Outcome result) {
        m_finishingEdits.remove(id);
        if (m_stopped) return;
        if (const auto *error = std::get_if<Error>(&result)) { emit errorOccurred(error->message()); return; }
        m_module->finishSourceChange(id);
        emit sourceUpdated(id);
    }));
}
void VodRuntime::shutdown()
{
    if (m_stopped) return;
    m_stopped = true;
    if (m_module->enabled()) {
        m_dvr->prepareRecordingStart = {};
        m_dvr->automaticPlaybackHandoffAllowed = {};
        auto waiters = std::move(m_recordingWaiters);
        m_recordingWaiters.clear();
        for (const auto &waiter : waiters) waiter(false);
        m_settings->prepareProfileMutation = {};
        m_settings->profileMutationFinished = {};
        m_multiview->playbackStartGate = {};
        for (const auto &player : m_players) if (player) player->playbackStartGate = {};
    }
    m_module->shutdown();
    // Local storage work owns Qt SQL connections. Join it while the application
    // and its drivers still exist; workers never wait for the GUI thread.
    m_sourceWork.waitForFinished();
    if (m_reconcileWork.isStarted()) m_reconcileWork.waitForFinished();
}
}
