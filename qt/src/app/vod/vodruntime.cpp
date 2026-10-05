#include "app/playback/trackpresentation.h"
#include "vodruntime.h"
#include "vodepisodesmodel.h"
#include "vodmediaprobe.h"
#include "app/multiviewcontroller.h"
#include "app/dvrcontroller.h"
#include "app/timeshiftcontroller.h"
#include "core/settingsmanager.h"
#include "core/sourcestore.h"
#include "core/sourcegrouppreferences.h"
#include <stdexcept>
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
    m_subtitlePool.setMaxThreadCount(1);
    connect(this, &VodRuntime::sourceInvalidated, this, [this](const QUuid &profile) {
        if (m_uploadRef.profileId == profile) { m_uploadRef = {}; m_uploadPausedSession = QUuid{}; }
        if (const auto cancel = m_subtitleCancellation.take(profile)) cancel->store(true);
        m_subtitlePending.clear();
        m_subtitleIntents.clear();
        m_subtitleStates.clear();
    });
    m_module = std::make_unique<VodModule>();
    connect(this, &VodRuntime::sourceSyncChanged, this, &VodRuntime::sourceSyncInProgressChanged);
    m_episodes = std::make_unique<VodEpisodesModel>(this);
    connect(m_episodes.get(), &VodEpisodesModel::playRequested, this, [this](const ContentRef &ref,bool fromBeginning) { startEpisode(ref,fromBeginning); });
    connect(m_episodes.get(), &VodEpisodesModel::changed, this, [this]() {
        if (!m_pendingEpisodeEnd || m_episodes->busy()) return;
        const auto pending=std::exchange(m_pendingEpisodeEnd,{});
        completeEpisode(pending->first,pending->second);
    });
    connect(m_episodes.get(), &VodEpisodesModel::loaded, this, [this]() {
        if (!activeSeries()) return;
        const auto ref=m_module->session()->snapshot().ref;
        const auto &details=m_episodes->details();
        for (const auto &episode : details.episodes) if (episode.ref==ref) {
            const auto label=m_episodes->selectedEpisode().value(QStringLiteral("label")).toString();
            if (!details.title.isEmpty()) m_seriesTitle=details.title;
            const auto seriesTitle=m_seriesTitle;
            setPlaybackTitle(ref,seriesTitle + QStringLiteral(" · ")+label+QStringLiteral(" · ")+episode.title);
        }
        emit stateChanged();
    });
    m_deferredFactory = std::move(engineFactory);
    m_settings->vodPolicyChanged = [this](const QUuid &id) { policyChanged(id); };
    bool enabled = settings->current().vodEnabled;
    for (const auto &summary : settings->sourceSummaries()) {
        updatePolicy(summary.id);
        const auto profile = settings->profileById(summary.id);
        enabled = enabled || (profile && profile->type == Core::ProfileType::Xtream && profile->vodEnabled);
    }
    if (enabled) initialize(std::move(m_deferredFactory));
}
void VodRuntime::ensureForSource(const QUuid &profile)
{
    if (m_stopped) return;
    const auto saved = m_settings->profileById(profile);
    if (!saved || saved->type != Core::ProfileType::Xtream || !saved->vodEnabled) return;
    if (!m_module->enabled()) initialize(std::move(m_deferredFactory));
}
void VodRuntime::updatePolicy(const QUuid &id, std::optional<QStringList> categories, CatalogKind kind)
{
    const auto profile = m_settings->profileById(id);
    const auto key = id.toString(QUuid::WithoutBraces) + QStringLiteral("|movies");
    std::lock_guard lock(*m_policies->mutex);
    auto &policy = m_policies->values[id];
    ++policy.generation;
    policy.enabled = profile && profile->type == Core::ProfileType::Xtream && profile->vodEnabled;
    policy.hidden = m_settings->current().hiddenGroupsByProfile.value(key);
    policy.configured = m_settings->current().groupOrderByProfile.contains(key);
    const auto seriesKey = id.toString(QUuid::WithoutBraces) + QStringLiteral("|series");
    policy.seriesHidden = m_settings->current().hiddenGroupsByProfile.value(seriesKey);
    policy.seriesConfigured = m_settings->current().groupOrderByProfile.contains(seriesKey);
    if (categories && kind == CatalogKind::Series) policy.seriesCategories = *categories;
    else if (policy.seriesCategories.isEmpty()) policy.seriesCategories = m_settings->current().groupOrderByProfile.value(seriesKey);
    if (categories && kind == CatalogKind::Movies) policy.categories = *categories;
    else if (policy.categories.isEmpty()) policy.categories = m_settings->current().groupOrderByProfile.value(key);
}
void VodRuntime::policyChanged(const QUuid &id)
{
    finishLibraryBrowsing(false);
    cancelLiveNavigation();
    cancelEpisodeTransition();
    updatePolicy(id);
    if (m_module->controller()) m_module->controller()->sourceChanged(id);
    emit sourceInvalidated(id);
    emit sourceUpdated(id);
    const auto saved = m_settings->profileById(id);
    if (saved && saved->type == Core::ProfileType::Xtream && saved->vodEnabled)
        synchronizeSource(id.toString(QUuid::WithoutBraces));
    else {
        m_queuedSync.remove(id);
        const auto requests = m_syncRequests.keys();
        for (const auto &request : requests) if (m_syncRequests.value(request).profile == id) m_syncRequests.remove(request);
        finishSync(id);
    }
}
void VodRuntime::synchronizeSource(const QString &profileId)
{
    const QUuid id(profileId);
    if (m_stopped || id.isNull()) return;
    const auto saved = m_settings->profileById(id);
    if (!saved || saved->type != Core::ProfileType::Xtream || !saved->vodEnabled) {
        if (syncing(id)) finishSync(id);
        return;
    }
    ensureForSource(id);
    if (!m_ready) {
        m_queuedSync.insert(id);
        m_syncErrors.remove(id);
        emit sourceSyncChanged(id);
        retryInitialization();
        return;
    }
    updatePolicy(id);
    m_module->controller()->sourceChanged(id);
    const auto requests = m_syncRequests.keys();
    for (const auto &request : requests) if (m_syncRequests.value(request).profile == id) m_syncRequests.remove(request);
    m_queuedSync.remove(id);
    m_syncing.insert(id); m_syncErrors.remove(id);
    m_syncRequests.insert(m_module->controller()->scope(id), {id, SyncStage::Scope, {}});
    emit sourceSyncChanged(id);
}
bool VodRuntime::reconcileCategories(const CategorySnapshot &snapshot)
{
    auto &settings = m_settings->current();
    const auto before = settings;
    const auto id = snapshot.scope.profileId;
    const auto key = id.toString(QUuid::WithoutBraces) + (snapshot.scope.kind == CatalogKind::Movies ? QStringLiteral("|movies") : QStringLiteral("|series"));
    QStringList discovered;
    for (const auto &category : snapshot.categories) if (!discovered.contains(category.id)) discovered.append(category.id);
    const auto reconciled = Core::reconcileSourceGroups(discovered, {settings.hiddenGroupsByProfile.value(key), settings.groupOrderByProfile.value(key)});
    settings.hiddenGroupsByProfile[key] = reconciled.hiddenGroups;
    settings.groupOrderByProfile[key] = reconciled.groupOrder;
    m_settings->save();
    if (!m_settings->lastSaveError().isEmpty()) {
        settings = before; m_syncErrors[id] = m_settings->lastSaveError(); return false;
    }
    updatePolicy(id, discovered, snapshot.scope.kind);
    emit categoriesUpdated(id);
    return true;
}
void VodRuntime::receiveSync(const VodEvent &event)
{
    const auto it = m_syncRequests.find(event.operationId);
    if (it == m_syncRequests.end()) return;
    auto request = it.value(); m_syncRequests.erase(it);
    if (const auto *error = std::get_if<Error>(&event.result)) {
        m_syncErrors[request.profile] = error->message();
        // Series categories fail independently; movie synchronization still runs.
        if (request.stage == SyncStage::Scope) { finishSync(request.profile); return; }
    } else {
        const auto &value = std::get<PublicValue>(event.result);
        if (request.stage == SyncStage::Scope) request.scope = std::get<CatalogScope>(value);
        if (request.stage == SyncStage::Movies || request.stage == SyncStage::Series) {
            reconcileCategories(std::get<CategorySnapshot>(value));
        }
    }
    auto *controller = m_module->controller();
    QUuid next;
    switch (request.stage) {
    case SyncStage::Scope:
        request.stage = SyncStage::Movies; next = controller->categories(request.scope, true); break;
    case SyncStage::Movies: {
        request.stage = SyncStage::Series;
        auto series = request.scope; series.kind = CatalogKind::Series;
        next = controller->categories(series, true); break;
    }
    case SyncStage::Series:
        request.stage = SyncStage::Catalog; next = controller->refresh(request.scope); break;
    case SyncStage::Catalog:
        request.stage = SyncStage::SeriesCatalog; request.scope.kind = CatalogKind::Series; next = controller->refresh(request.scope); break;
    case SyncStage::SeriesCatalog:
        finishSync(request.profile); return;
    }
    m_syncRequests.insert(next, request);
    emit sourceSyncChanged(request.profile);
}
void VodRuntime::finishSync(const QUuid &id)
{
    m_queuedSync.remove(id);
    m_syncing.remove(id);
    const auto requests = m_syncRequests.keys();
    for (const auto &request : requests)
        if (m_syncRequests.value(request).profile == id) m_syncRequests.remove(request);
    emit sourceSyncChanged(id);
    if (!m_stopped) emit sourceSyncFinished(id);
}
void VodRuntime::failQueuedSync(const QString &error)
{
    const auto queued = m_queuedSync;
    for (const auto &id : queued) {
        m_syncErrors[id] = error;
        finishSync(id);
    }
}
QList<Core::ChannelCategory> VodRuntime::sourceCategories(const QUuid &id, CatalogKind kind) const
{
    std::shared_ptr<SqliteVodStore> store;
    { QMutexLocker lock(&m_storeMutex); store = m_store; }
    if (!store) return {};
    const auto source = store->snapshot(id);
    if (const auto *error = std::get_if<Error>(&source)) throw std::runtime_error(error->message().toStdString());
    RequestContext context; context.source = std::get<SourceContext>(source).revision;
    const CatalogScope scope{id, context.source.catalogNamespace, kind, {}};
    const auto result = store->readCategories(scope, context);
    if (const auto *error = std::get_if<Error>(&result)) throw std::runtime_error(error->message().toStdString());
    QList<Core::ChannelCategory> groups;
    if (const auto cached = std::get<std::optional<CategorySnapshot>>(result))
        for (const auto &category : cached->categories) groups.append({category.id, category.name, 0});
    return groups;
}
void VodRuntime::initialize(EngineFactory engineFactory)
{
    const auto directory = QFileInfo(m_settings->settingsFilePath()).dir();
    m_artwork = std::make_shared<VodArtworkCache>(Core::AppDataPaths::vodArtworkDirectory(directory.absolutePath()));

    const Core::SourceStore sources(directory.filePath(QStringLiteral("source-summaries.json")), directory.filePath(QStringLiteral("sources")));
    auto store = std::make_shared<SqliteVodStore>(directory.filePath(QStringLiteral("iptv.db")), [sources, policies = m_policies](const QUuid &id) -> Result<SourceContext> {
        try {
            const auto summaries = sources.loadSummaries();
            if (std::none_of(summaries.cbegin(), summaries.cend(), [&](const Core::SourceSummary &summary) { return summary.id == id; }))
                return Error{ErrorCode::ContentUnavailable, {}};
            const auto profile = sources.loadDetail(id);
            if (!profile) return Error{ErrorCode::ContentUnavailable, {}};
            SourceContext source;
            source.revision = {id, {}, profile->vodCredentialRevision};
            source.enabled = profile->vodEnabled && profile->type == Core::ProfileType::Xtream;
            {
                std::lock_guard lock(*policies->mutex);
                const auto policy = policies->values.value(id);
                QStringList allowed;
                for (const auto &category : policy.categories) if (!policy.hidden.contains(category)) allowed.append(category);
                if (policy.configured) source.allowedMovieCategories = allowed;
                source.movieCategoryCount = static_cast<int>(policy.categories.size());
                QStringList seriesAllowed;
                for (const auto &category : policy.seriesCategories) if (!policy.seriesHidden.contains(category)) seriesAllowed.append(category);
                if (policy.seriesConfigured) source.allowedSeriesCategories = seriesAllowed;
                source.seriesCategoryCount = static_cast<int>(policy.seriesCategories.size());
                source.policyMutex = policies->mutex;
                source.policyCurrent = [policies, id, generation = policy.generation]() {
                    std::lock_guard policyLock(*policies->mutex);
                    const auto current = policies->values.value(id);
                    return current.enabled && current.generation == generation;
                };
            }
            source.provider = QStringLiteral("xtream");
            source.endpoint = QUrl(profile->xtreamBaseUrl);
            source.username = profile->xtreamUsername; source.password = profile->xtreamPassword;
            return source;
        } catch (...) { return Error{ErrorCode::SecretUnavailable, {}}; }
    });
    { QMutexLocker lock(&m_storeMutex); m_store = std::move(store); }
    m_subtitles = std::make_shared<VodSubtitleCache>(Core::AppDataPaths::vodSubtitlesDirectory(directory.absolutePath()), m_store);
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
        auto provider = std::make_shared<XtreamVodProvider>(std::make_shared<QtHttpTransport>(), true);
        const auto artwork = m_artwork;
        provider->registerArtwork = [artwork](const SourceContext &source, const QUrl &url, const RequestContext &context) {
            return artwork->remember(source, url, context);
        };
        composition.dependencies = {provider, m_store, m_store, m_store, m_store,
            [artwork](const QUuid &profile, const ArtworkRef &ref, const RequestContext &context) { return artwork->resolve(profile, ref, context); },
            [](const PlaybackDescriptor &descriptor, const RequestContext &context) { return probeVodMedia(descriptor, context); },
            [artwork](const QUuid &profile, const ArtworkRef &ref, const RequestContext &context) { return artwork->cached(profile, ref, context); }, m_store};
        const auto subtitles = m_subtitles;
        composition.dependencies.subtitles = [subtitles](const ContentRef &ref, const RequestContext &context) { return subtitles->read(ref, context); };
        composition.dependencies.subtitleFiles = [subtitles](const ContentRef &ref, const QJsonObject &state) { return subtitles->playbackFiles(ref, state); };
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
        if (snapshot.end || snapshot.sessionToken != m_libraryPausedSession) m_libraryPausedSession = QUuid{};
        emit stateChanged();
        observeSubtitles(snapshot);
        updatePlaybackMetadata(snapshot);
        observeEpisode(snapshot);
        if (snapshot.state == SessionState::Failed) {
            const auto message = snapshot.ref.kind==ContentKind::Episode
                ? tr("The episode could not be played. Press B to return to the library and try again.")
                : tr("The movie could not be played. Press V to return to the library and try again.");
            emit errorOccurred(message);
            emit notification(message);
        }
    };
    m_module->playbackInterrupted = [this]() { m_uploadPausedSession = QUuid{}; finishLibraryBrowsing(false); cancelEpisodeTransition(); };
    m_module->progressFailed = [this](const Error &error) { emit errorOccurred(error.message()); };
    connect(m_module->controller(), &VodController::playbackRequested, this, &VodRuntime::cancelEpisodeTransition);
    connect(m_module->controller(), &VodController::playbackRequested, this, [this]() {
        finishLibraryBrowsing(false);
        cancelLiveNavigation();
    });
    connect(m_module->controller(), &VodController::eventCompleted, this, [this](const VodEvent &event) {
        if (event.operationId!=m_episodePlay) return;
        m_episodePlay=QUuid{}; m_episodeTransition=false;
        if (const auto *error=std::get_if<Error>(&event.result)) emit errorOccurred(error->message());
        else emit episodeStarted();
        emit stateChanged();
    });
    connect(m_module->controller(), &VodController::eventCompleted, this, &VodRuntime::receiveSync);
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
        if (const auto *error = std::get_if<Error>(&result)) {
            failQueuedSync(error->message());
            emit errorOccurred(error->message());
            return;
        }
        QScopedValueRollback reconciling(m_reconciling, true);
        for (const auto &id : std::get<QList<QUuid>>(result)) {
            if (m_settings->profileById(id) && !m_settings->removeProfile(id)) {
                failQueuedSync(m_settings->lastSaveError());
                emit errorOccurred(m_settings->lastSaveError()); return;
            }
        }
        for (const auto &id : existing) m_module->controller()->unblockSource(id);
        m_ready = true;
        const auto failedSources = m_syncErrors.keys();
        m_syncErrors.clear(); // Initialization errors no longer block catalogue loading.
        for (const auto &id : failedSources) emit sourceSyncChanged(id);
        emit sourcesReconciled(); emit stateChanged();
        const auto queued = m_queuedSync;
        for (const auto &id : queued) synchronizeSource(id.toString(QUuid::WithoutBraces));
    });
    const auto store = m_store;
    const auto artwork = m_artwork;
    const auto subtitles = m_subtitles;
    m_reconcileWork = QtConcurrent::run([store, artwork, subtitles, existing]() -> Result<QList<QUuid>> {
        try {
            artwork->retainSources(existing);
            subtitles->reconcile(existing);
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
    if (m_episodeTransition) return true;
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
    auto rows = App::Playback::subtitleTracks(qobject_cast<Player::MpvPlayer *>(playerObject()));
    const auto *backend = qobject_cast<Player::MpvPlayer *>(playerObject());
    const auto tracks = backend ? backend->trackList() : QVariantList{};
    for (auto &value : rows) {
        auto row = value.toMap();
        for (const auto &entry : tracks) {
            const auto track = entry.toMap();
            if (track.value(QStringLiteral("type")).toString() == QLatin1String("sub") && track.value(QStringLiteral("id")) == row.value(QStringLiteral("id"))) {
                row.insert(QStringLiteral("externalId"), track.value(QStringLiteral("externalId")));
                break;
            }
        }
        value = row;
    }
    if (active()) {
        const auto state = subtitleState(m_module->session()->snapshot().ref);
        int synthetic = -3;
        for (const auto &value : state.value(QStringLiteral("files")).toArray()) {
            const auto file = value.toObject();
            const auto id = file.value(QStringLiteral("id")).toString();
            bool loaded = false;
            for (auto &entry : rows) {
                auto row = entry.toMap();
                if (row.value(QStringLiteral("externalId")).toString() == id) {
                    loaded = true; row.insert(QStringLiteral("name"), file.value(QStringLiteral("name")).toString()); entry = row;
                }
            }
            if (!loaded) rows.append(QVariantMap{{QStringLiteral("id"), synthetic--}, {QStringLiteral("externalId"), id},
                {QStringLiteral("action"), QStringLiteral("external")}, {QStringLiteral("name"), file.value(QStringLiteral("name")).toString()},
                {QStringLiteral("subtitle"), QStringLiteral("Uploaded subtitles")}, {QStringLiteral("selected"), false}});
        }
    }
    if (active()) rows.append(QVariantMap{{QStringLiteral("id"), -2}, {QStringLiteral("action"), QStringLiteral("upload")},
        {QStringLiteral("name"), QStringLiteral("Upload subtitles...")}, {QStringLiteral("subtitle"), QString{}}, {QStringLiteral("selected"), false}});
    return rows;
}
void VodRuntime::selectAudioTrack(int id)
{
    if (auto *backend = qobject_cast<Player::MpvPlayer *>(playerObject())) backend->selectAudioTrack(id, true);
}
void VodRuntime::selectSubtitleTrack(int id)
{
    if (!active()) return;
    m_subtitleIntents.remove(m_module->session()->snapshot().ref.key());
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
        if (track.value(QStringLiteral("external")).toBool()) continue;
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
    const auto buffer = backend->playbackBufferSnapshot();
    const auto cache = buffer.seconds.value_or(-1.0);
    const auto cacheText = (buffer.seconds && buffer.estimated ? QStringLiteral("≈") : QString {})
        + Presentation::formatDebugBufferDuration(cache);
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
        {QStringLiteral("bufferDurationText"), cacheText},
        {QStringLiteral("bufferDurationSourceText"), QStringLiteral("mpv cache")},
        {QStringLiteral("mpvBufferDurationText"), cacheText},
        {QStringLiteral("minBufferNeededSeconds"), backend->bufferTargetSeconds()},
        {QStringLiteral("timeshiftMode"), QStringLiteral("Off (VOD)")},
        {QStringLiteral("timestamp"), Presentation::debugTimestampNowLocal()}
    };
}
void VodRuntime::beginLibraryBrowsing()
{
    if (!active() || isPaused() || m_liveTransition) return;
    m_libraryPausedSession = m_module->session()->snapshot().sessionToken;
    m_module->session()->pause();
    emit stateChanged();
}
void VodRuntime::finishLibraryBrowsing(bool resumePlayback)
{
    const auto session = std::exchange(m_libraryPausedSession, {});
    if (resumePlayback && !session.isNull() && active() && isPaused()
        && m_module->session()->snapshot().sessionToken == session) {
        m_module->session()->resume();
        emit stateChanged();
    }
}
void VodRuntime::cancelLiveNavigation()
{
    ++m_liveGeneration;
    if (!m_liveTransition) return;
    m_liveTransition = false;
    emit stateChanged();
}
void VodRuntime::returnToLive()
{
    if (m_stopped || m_liveTransition) return;
    finishLibraryBrowsing(false);
    if (!m_module->enabled()) { emit liveRequested(); return; }
    m_liveTransition = true;
    const auto generation = ++m_liveGeneration;
    emit stateChanged();
    // Reuse the acknowledged stop and serial progress flush barrier. This
    // does not start or stop any recording; it only releases the VOD session.
    m_module->prepareRecording([this, generation](Outcome result) {
        if (m_stopped || generation != m_liveGeneration) return;
        m_liveTransition = false;
        emit stateChanged();
        if (const auto *error = std::get_if<Error>(&result)) {
            emit errorOccurred(error->message());
            emit notification(error->message());
            return;
        }
        const auto activate = [this]() { emit liveRequested(); };
        if (!gate(this, activate)) activate();
    });
}
void VodRuntime::togglePause() { m_uploadPausedSession = QUuid{}; finishLibraryBrowsing(false); if (active()) { if (isPaused()) m_module->session()->resume(); else m_module->session()->pause(); } }
void VodRuntime::stop() { finishLibraryBrowsing(false); cancelLiveNavigation(); cancelEpisodeTransition(); if (m_module->controller()) m_module->controller()->cancelPendingPlayback(); if (active()) m_module->session()->stop(); }
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
    cancelEpisodeTransition();
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
    finishLibraryBrowsing(false);
    cancelLiveNavigation();
    cancelEpisodeTransition();
    if (m_reconciling) return true;
    if (!m_ready || m_stopped) { if (error) *error = QStringLiteral("VOD storage is not ready. Source changes were not saved."); return false; }
    if (m_preparingEdits.contains(id) || m_finishingEdits.contains(id)) {
        if (error) *error = QStringLiteral("VOD source change is still completing. Please retry.");
        return false;
    }
    emit sourceInvalidated(id);
    if (syncing(id)) finishSync(id);
    if (replacement) m_preparingEdits.insert(id);
    const auto revision = replacement ? replacement->vodCredentialRevision : 0;
    const auto outcome = waitFor([this, id, revision, replacement](Completion done) {
        if (!replacement) {
            m_module->controller()->blockSource(id);
            const auto operation = m_module->controller()->removeSource(id);
            m_removals.insert(operation, [this, id, done](Outcome result) {
                if (std::holds_alternative<Success>(result)) {
                    const auto artwork = m_artwork;
                    const auto subtitles = m_subtitles;
                    m_sourceWork.addFuture(work(this, [artwork, subtitles, id]() -> Outcome { artwork->removeSource(id); subtitles->removeSource(id); return Success{}; }, [](Outcome) {}));
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
    cancelEpisodeTransition();
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
    finishLibraryBrowsing(false);
    cancelLiveNavigation();
    cancelEpisodeTransition();
    if (m_stopped) return;
    m_stopped = true;
    for (const auto &cancel : m_subtitleCancellation) cancel->store(true);
    const auto syncingSources = m_syncing | m_queuedSync;
    for (const auto &id : syncingSources) finishSync(id);
    m_settings->vodPolicyChanged = {};
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
    m_subtitlePool.waitForDone();
    m_sourceWork.waitForFinished();
    if (m_reconcileWork.isStarted()) m_reconcileWork.waitForFinished();
}
}

namespace OKILTV::Vod {
QObject *VodRuntime::episodesObject() const { return m_episodes.get(); }
bool VodRuntime::activeSeries() const { return m_module->session() && m_module->session()->snapshot().ref.kind==ContentKind::Episode && (active() || m_episodeTransition); }
bool VodRuntime::hasPreviousEpisode() const { return activeSeries() && adjacentEpisode(m_episodes->details(),m_module->session()->snapshot().ref,-1).has_value(); }
bool VodRuntime::hasNextEpisode() const { return activeSeries() && adjacentEpisode(m_episodes->details(),m_module->session()->snapshot().ref,1).has_value(); }
void VodRuntime::cancelEpisodeTransition() {
    const bool changed=m_episodeTransition || m_returnSeries.valid();
    ++m_episodeGeneration; m_pendingEpisodeEnd.reset();
    if (!m_episodePlay.isNull() && m_module->controller()) m_module->controller()->cancel(m_episodePlay);
    m_episodePlay=QUuid{}; m_episodeTransition=false; m_returnSeries={}; m_returnAfterStop=false;
    if (changed) emit stateChanged();
}
void VodRuntime::startEpisode(const ContentRef &ref,bool fromBeginning) {
    if (m_stopped || !m_ready) return;
    PlaybackPreferences preferences; preferences.fromBeginning=fromBeginning;
    const auto &snapshot=m_module->session()->snapshot();
    if (parentSeries(snapshot.ref)==parentSeries(ref)) preferences.inheritedTracks=snapshot.trackPreferences;
    m_episodePlay=m_module->controller()->play(ref,preferences);
    m_episodeTransition=true; emit stateChanged();
}
void VodRuntime::navigateEpisode(int direction) {
    if (!activeSeries() || m_episodeTransition) return;
    const auto episode=adjacentEpisode(m_episodes->details(),m_module->session()->snapshot().ref,direction);
    if (episode) startEpisode(episode->ref,true);
}
void VodRuntime::previousEpisode() { navigateEpisode(-1); }
void VodRuntime::nextEpisode() { navigateEpisode(1); }
void VodRuntime::observeEpisode(const SessionSnapshot &snapshot) {
    if (snapshot.ref.kind!=ContentKind::Episode) return;
    if (!snapshot.end && snapshot.sessionToken!=m_episodeSession) {
        m_episodeSession=snapshot.sessionToken;
        m_episodes->load(parentSeries(snapshot.ref),snapshot.ref);
    }
    if (!snapshot.end) return;
    if (snapshot.state!=SessionState::Ended || snapshot.end!=Player::EndReason::NaturalEnd) {
        if (m_returnAfterStop && m_returnSeries==parentSeries(snapshot.ref) && snapshot.end==Player::EndReason::UserStop) {
            finishSeriesReturn(); return;
        }
        // The coordinator acknowledges replacement by stopping the old episode.
        // The owned next request survives this acknowledgement; explicit Stop cancels it first.
        if (!m_episodePlay.isNull() && (snapshot.end==Player::EndReason::UserStop || snapshot.end==Player::EndReason::Replaced)) return;
        cancelEpisodeTransition(); return;
    }
    if (m_handledEnd==snapshot.sessionToken) return;
    m_handledEnd=snapshot.sessionToken;
    const auto generation=++m_episodeGeneration;
    m_episodeTransition=true; emit stateChanged();
    m_module->progressService()->flushSource(snapshot.ref.profileId,[this,snapshot,generation](Outcome result) {
        if (m_stopped || generation!=m_episodeGeneration || m_module->session()->snapshot().sessionToken!=snapshot.sessionToken) return;
        if (const auto *error=std::get_if<Error>(&result)) { m_episodeTransition=false; emit errorOccurred(error->message()); emit stateChanged(); return; }
        completeEpisode(snapshot,generation);
    });
}
void VodRuntime::completeEpisode(const SessionSnapshot &snapshot,quint64 generation) {
    if (m_stopped || generation!=m_episodeGeneration || m_module->session()->snapshot().sessionToken!=snapshot.sessionToken) return;
    if (m_episodes->busy()) { m_pendingEpisodeEnd=std::pair{snapshot,generation}; return; }
    if (!m_episodes->errorText().isEmpty() || m_episodes->details().ref!=parentSeries(snapshot.ref)) {
        m_episodeTransition=false; emit errorOccurred(tr("Episode information could not be loaded.")); emit stateChanged(); return;
    }
    // An early EOF is not a confirmed completed episode. Unknown duration never marks Watched.
    if (snapshot.durationMs && !watchedByPosition(snapshot.positionMs,snapshot.durationMs)) { m_episodeTransition=false; emit stateChanged(); return; }
    const auto next=adjacentEpisode(m_episodes->details(),snapshot.ref,1);
    if (next) startEpisode(next->ref,true);
    else { m_episodeTransition=false; m_returnSeries=parentSeries(snapshot.ref); emit stateChanged(); }
}
void VodRuntime::returnToSeriesLibrary() {
    if (!m_module->session()) return;
    const auto series=parentSeries(m_module->session()->snapshot().ref);
    stop(); m_returnSeries=series; m_returnAfterStop=true;
    if (!active()) finishSeriesReturn();
    emit stateChanged();
}
void VodRuntime::finishSeriesReturn() {
    const auto series=m_returnSeries; const auto generation=m_episodeGeneration;
    m_module->progressService()->flushSource(series.profileId,[this,series,generation](Outcome result) {
        if (m_stopped || m_returnSeries!=series || generation!=m_episodeGeneration) return;
        m_returnAfterStop=false;
        if (const auto *error=std::get_if<Error>(&result)) emit errorOccurred(error->message());
        emit stateChanged();
    });
}
void VodRuntime::deliverLibraryReturn() {
    if (!libraryReturnPending()) return;
    const auto series=m_returnSeries;m_returnSeries={};
    emit seriesReturnRequested(series); emit libraryRequested(); emit stateChanged();
}
}
