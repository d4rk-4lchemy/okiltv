#include "vodcontroller.h"
#include <QTimer>
#include <QPointer>
#include <utility>

namespace OKILTV::Vod {
namespace {
template<class T> Result<JobReply> reply(Result<T> result, const SourceRevision &source)
{
    if (const auto *error = std::get_if<Error>(&result)) return *error;
    return JobReply{source, std::get<T>(std::move(result)), {}};
}
bool permitted(const VodDependencies &deps, const SourceContext &source, const ContentRef &ref, const RequestContext &context)
{
    auto admitted = ref.kind == ContentKind::Episode ? parentSeries(ref) : ref;
    const auto allowed = admitted.kind == ContentKind::Movie ? source.allowedMovieCategories : source.allowedSeriesCategories;
    if (!allowed) return true;
    // Identity is checked through a filtered local query; no provider request.
    CatalogQuery query{{ref.profileId, ref.catalogNamespace, admitted.kind == ContentKind::Movie ? CatalogKind::Movies : CatalogKind::Series, {}}, {}, CatalogSort::TitleAscending, 1000, {}};
    query.identity = admitted.key();
    query.allowedCategories = allowed;
    do {
        const auto result = deps.catalog->query(query, context);
        if (!std::holds_alternative<CatalogPage>(result)) return false;
        const auto &page = std::get<CatalogPage>(result);
        for (const auto &item : page.items) if (std::visit([&](const auto &summary) { return summary.ref == admitted; }, item)) return true;
        query.page = page.next;
    } while (query.page);
    return false;
}
bool matches(const SourceContext &source, const CatalogScope &scope)
{ return source.revision.profileId == scope.profileId && source.revision.catalogNamespace == scope.catalogNamespace; }
Outcome cacheArtwork(const VodDependencies &deps, const QUuid &profile, QList<ArtworkRef> &artwork, const RequestContext &context)
{
    for (auto &art : artwork) {
        art.cachedPath.reset();
        if (const auto error = context.interruption()) return *error;
        if (!deps.cachedArtwork) continue;
        const auto result = deps.cachedArtwork(profile, art, context);
        if (const auto *error = std::get_if<Error>(&result)) {
            if (error->code == ErrorCode::Cancelled || error->code == ErrorCode::Timeout) return *error;
            continue; // An artwork miss/error must not hide the catalogue or details.
        }
        if (const auto cached = std::get<std::optional<ArtworkRef>>(result)) art.cachedPath = cached->cachedPath;
    }
    return Success{};
}
}
VodController::VodController(VodDependencies deps, PlaybackCoordinator &coordinator, QObject *parent)
    : QObject(parent), m_deps(std::move(deps)), m_coordinator(coordinator)
{
    m_probeCooldown.setSingleShot(true);
    m_probeCooldown.setInterval(2000);
    m_probeCooldown.setTimerType(Qt::PreciseTimer);
    connect(&m_probeCooldown, &QTimer::timeout, this, [this]() {
        auto pending = std::exchange(m_deferredPlay, {});
        if (pending) pending->start();
    });
}
QUuid VodController::submit(const QUuid &profile, Operation operation, OperationKind kind)
{
    RequestContext request;
    if (kind == OperationKind::Refresh) request.deadline = QDeadlineTimer(90000);
    const auto id = request.operationId;
    const bool play = kind == OperationKind::Play;
    if (play) {
        emit playbackRequested();
        cancelPendingPlayback();
        m_playRequest = id;
        m_playProfile = profile;
    }
    if (m_stopped || m_blockedSources.contains(profile)
        || (kind == OperationKind::Probe && (!m_playRequest.isNull() || m_probeHandoff || m_probeCooldown.isActive()))) {
        QTimer::singleShot(0, this, [this, id, profile, play]() { deliver(id, profile, Error{ErrorCode::Cancelled, id}, play); });
        return id;
    }
    if (kind == OperationKind::Probe) m_probes.insert(id);
    const auto deps = m_deps;
    auto start = [this, deps, profile, request, id, play, operation = std::move(operation)]() {
        m_jobs.submit(profile, request, [deps, profile, operation](RequestContext context) -> Result<JobReply> {
            auto source = deps.sources->snapshot(profile);
            if (const auto *error = std::get_if<Error>(&source)) return *error;
            const auto snapshot = std::get<SourceContext>(source);
            context.source = snapshot.revision;
            context.policyCurrent = snapshot.policyCurrent;
            context.policyMutex = snapshot.policyMutex;
            if (!snapshot.enabled) return Error{ErrorCode::UnsupportedCapability, context.operationId};
            auto migrated = deps.migrations->prepare(context);
            if (const auto *error = std::get_if<Error>(&migrated)) return *error;
            if (const auto interrupted = context.interruption()) return *interrupted;
            auto result = operation(deps, snapshot, context);
            if (auto *reply = std::get_if<JobReply>(&result)) reply->policyCurrent = context.policyCurrent;
            return result;
        }, [this, id, profile, play](Result<JobReply> result) { deliver(id, profile, std::move(result), play); });
    };
    if (play && (!m_probes.isEmpty() || m_probeCooldown.isActive())) {
        m_deferredPlay = DeferredPlay{id, profile, std::move(start)};
        if (!m_probes.isEmpty()) {
            m_probeHandoff = true;
            for (const auto &probe : m_probes) m_jobs.cancel(probe);
        }
    } else start();
    return id;
}
void VodController::deliver(const QUuid &id, const QUuid &profile, Result<JobReply> result, bool play)
{
    // Job completion is delivered only after the worker has reaped ffprobe.
    // Cancellation alone is not an acknowledgement of process termination.
    if (m_probes.remove(id) && m_probes.isEmpty() && m_probeHandoff) {
        m_probeHandoff = false;
        m_probeCooldown.start();
    }
    if (m_blockedSources.contains(profile)) result = Error{ErrorCode::Cancelled, id};
    if (play && id != m_playRequest) result = Error{ErrorCode::Cancelled, id};
    if (const auto *value = std::get_if<JobReply>(&result); value && (!m_deps.sources->isCurrent(value->source) || (value->policyCurrent && !value->policyCurrent())))
        result = Error{ErrorCode::Cancelled, id};
    if (auto *error = std::get_if<Error>(&result)) {
        if (play && m_playRequest == id) m_playRequest = QUuid{};
        error->operationId = id;
        if (completed) completed({id, profile, *error});
        emit eventCompleted({id, profile, *error});
        return;
    }
    auto value = std::get<JobReply>(std::move(result));
    if (play && value.playback) {
        const auto progress = std::get<std::optional<VodProgress>>(value.value);
        value.playback->admissionCurrent = value.policyCurrent;
        m_coordinator.play(id, *value.playback, QUuid::createUuid(), progress,
            [this, id, profile](Outcome outcome) {
                if (m_playRequest == id) m_playRequest = QUuid{};
                if (auto *error = std::get_if<Error>(&outcome)) {
                    error->operationId = id;
                    if (completed) completed({id, profile, *error});
                    emit eventCompleted({id, profile, *error});
                } else {
                    if (completed) completed({id, profile, PublicValue{Success{}}});
                    emit eventCompleted({id, profile, PublicValue{Success{}}});
                }
            });
    } else {
        if (completed) completed({id, profile, value.value});
        emit eventCompleted({id, profile, std::move(value.value)});
    }
}
QUuid VodController::scope(const QUuid &profile)
{
    return submit(profile, [](const VodDependencies &, const SourceContext &source, const RequestContext &) -> Result<JobReply> {
        return JobReply{source.revision, CatalogScope{source.revision.profileId, source.revision.catalogNamespace, CatalogKind::Movies, {}}, {}};
    });
}
QUuid VodController::artwork(const QUuid &profile, const ArtworkRef &art)
{
    return submit(profile, [art](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
        if (!deps.artwork) return Error{ErrorCode::UnsupportedCapability, context.operationId};
        return reply(deps.artwork(source.revision.profileId, art, context), source.revision);
    });
}
QUuid VodController::capabilities(const QUuid &profile)
{
    return submit(profile, [](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) {
        return reply(deps.provider->capabilities(source, context), source.revision);
    });
}
QUuid VodController::categories(const CatalogScope &scope, bool refresh)
{
    return submit(scope.profileId, [scope, refresh](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
        if (!matches(source, scope) || scope.categoryId) return Error{ErrorCode::ContentUnavailable, context.operationId};
        if (!refresh) {
            auto stored = deps.catalog->readCategories(scope, context);
            if (const auto *error = std::get_if<Error>(&stored)) return *error;
            if (const auto cached = std::get<std::optional<CategorySnapshot>>(stored))
                return JobReply{source.revision, *cached, {}};
        }
        const auto begun = deps.catalog->beginCategoryRefresh(scope, context);
        if (const auto *error = std::get_if<Error>(&begun)) return *error;
        auto response = deps.provider->listCategories(source, scope, context);
        if (const auto *error = std::get_if<Error>(&response)) return *error;
        CategorySnapshot snapshot{scope, std::get<QList<VodCategory>>(std::move(response)), QDateTime::currentDateTimeUtc()};
        auto saved = deps.catalog->storeCategories(snapshot, context);
        if (const auto *error = std::get_if<Error>(&saved)) return *error;
        return JobReply{source.revision, std::move(snapshot), {}};
    }, OperationKind::Refresh);
}
QUuid VodController::query(const CatalogQuery &query)
{
    return submit(query.scope.profileId, [query](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
        if (!matches(source, query.scope) || query.pageSize < 1 || query.pageSize > 1000)
            return Error{ErrorCode::InvalidResponse, context.operationId};
        auto filtered = query;
        filtered.allowedCategories = query.scope.kind == CatalogKind::Movies ? source.allowedMovieCategories : source.allowedSeriesCategories;
        auto result = deps.catalog->query(filtered, context);
        if (const auto *error = std::get_if<Error>(&result)) return *error;
        auto page = std::get<CatalogPage>(std::move(result));
        for (auto &item : page.items) {
            const auto cached = std::visit([&](auto &summary) { return cacheArtwork(deps, source.revision.profileId, summary.artwork, context); }, item);
            if (const auto *error = std::get_if<Error>(&cached)) return *error;
        }
        return JobReply{source.revision, std::move(page), {}};
    });
}
QUuid VodController::details(const ContentRef &ref, bool refresh)
{
    return submit(ref.profileId, [ref, refresh](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
        if (!permitted(deps, source, ref, context)) return Error{ErrorCode::ContentUnavailable, context.operationId};
        if (!ref.valid() || ref.catalogNamespace != source.revision.catalogNamespace)
            return Error{ErrorCode::ContentUnavailable, context.operationId};
        auto stored = deps.catalog->readDetails(ref, context);
        if (const auto *error = std::get_if<Error>(&stored)) return *error;
        if (auto cached = std::get<std::optional<VodDetails>>(std::move(stored)); cached && !refresh) {
            for (auto &episode : cached->episodes) cacheArtwork(deps,ref.profileId,episode.artwork,context);
            const auto artwork = cacheArtwork(deps, ref.profileId, cached->artwork, context);
            if (const auto *error = std::get_if<Error>(&artwork)) return *error;
            return JobReply{source.revision, std::move(*cached), {}};
        }
        auto response = deps.provider->fetchDetails(source, ref, context);
        if (const auto *error = std::get_if<Error>(&response)) return *error;
        auto value = std::get<VodDetails>(std::move(response));
        if (value.ref != ref) return Error{ErrorCode::InvalidResponse, context.operationId};
        auto saved = deps.catalog->storeDetails(value, context);
        if (const auto *error = std::get_if<Error>(&saved)) return *error;
        for (auto &episode : value.episodes) cacheArtwork(deps,ref.profileId,episode.artwork,context);
        const auto artwork = cacheArtwork(deps, ref.profileId, value.artwork, context);
        if (const auto *error = std::get_if<Error>(&artwork)) return *error;
        return JobReply{source.revision, std::move(value), {}};
    });
}
QUuid VodController::cachedSeasonMetadata(const ContentRef &series, const QString &seasonId)
{
    return submit(series.profileId, [series, seasonId](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
        if (!series.valid() || series.kind != ContentKind::Series || series.catalogNamespace != source.revision.catalogNamespace
            || !permitted(deps, source, series, context)) return Error{ErrorCode::ContentUnavailable, context.operationId};
        return reply(deps.catalog->readSeasonMediaMetadata(series, seasonId, context), source.revision);
    });
}
QUuid VodController::probe(const ContentRef &ref)
{
    return submit(ref.profileId, [ref](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
        if (!permitted(deps, source, ref, context)) return Error{ErrorCode::ContentUnavailable, context.operationId};
        if (!ref.playable() || ref.catalogNamespace != source.revision.catalogNamespace || !deps.mediaProbe)
            return Error{ErrorCode::UnsupportedCapability, context.operationId};
        auto stored = deps.catalog->readDetails(ref, context);
        if (const auto *error = std::get_if<Error>(&stored)) return *error;
        auto details = std::get<std::optional<VodDetails>>(std::move(stored));
        if (details && details->mediaProbe
            && details->mediaProbe->observedAtUtc >= QDateTime::currentDateTimeUtc().addDays(-7)) {
            return JobReply{source.revision, *details->mediaProbe, {}};
        }
        auto descriptor = VodPlaybackResolver::resolve(deps, source, ref, {}, context);
        if (const auto *error = std::get_if<Error>(&descriptor)) return *error;
        auto result = deps.mediaProbe(std::get<PlaybackDescriptor>(descriptor), context);
        if (const auto *error = std::get_if<Error>(&result)) return *error;
        auto probe = std::get<VodMediaProbe>(std::move(result));
        if (details) {
            details->mediaProbe = probe;
            const auto saved = deps.catalog->storeDetails(*details, context);
            if (const auto *error = std::get_if<Error>(&saved)) return *error;
        }
        return JobReply{source.revision, std::move(probe), {}};
    }, OperationKind::Probe);
}
QUuid VodController::refresh(const CatalogScope &scope)
{
    return submit(scope.profileId, [scope](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
        if (!matches(source, scope)) return Error{ErrorCode::ContentUnavailable, context.operationId};
        return reply(VodCatalogService::refresh(deps, source, scope, context), source.revision);
    }, OperationKind::Refresh);
}
QUuid VodController::cachePlaybackMetadata(const ContentRef &ref, const VodMediaProbe &metadata, std::optional<qint64> durationMs)
{
    return submit(ref.profileId, [ref, metadata, durationMs](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
        if (!permitted(deps, source, ref, context)) return Error{ErrorCode::ContentUnavailable, context.operationId};
        if (!ref.playable() || ref.catalogNamespace != source.revision.catalogNamespace)
            return Error{ErrorCode::ContentUnavailable, context.operationId};
        auto stored = deps.catalog->readDetails(ref, context);
        if (const auto *error = std::get_if<Error>(&stored)) return *error;
        auto details = std::get<std::optional<VodDetails>>(std::move(stored));
        // Only enrich existing details: this path never fetches provider metadata or media.
        if (!details) return JobReply{source.revision, metadata, {}};
        details->mediaProbe = metadata;
        if (durationMs && *durationMs > 0) details->declaredDurationMs = durationMs;
        const auto saved = deps.catalog->storeDetails(*details, context);
        if (const auto *error = std::get_if<Error>(&saved)) return *error;
        return JobReply{source.revision, *details, {}};
    });
}
QUuid VodController::play(const ContentRef &ref, const PlaybackPreferences &preferences)
{
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks) -- submit transfers the owning std::function to the deferred start or job; both release their captures.
    return submit(ref.profileId, [ref, preferences](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
        if (!permitted(deps, source, ref, context)) return Error{ErrorCode::ContentUnavailable, context.operationId};
        auto playable = ref;
        const auto owningSeries=parentSeries(ref);
        if (owningSeries.valid()) {
            // Persist the lazy episode set before loading media. Even a short
            // episode's first Watched checkpoint can then assess Series To Watch.
            auto stored = deps.catalog->readDetails(owningSeries, context);
            if (const auto *error=std::get_if<Error>(&stored)) return *error;
            auto details=std::get<std::optional<VodDetails>>(stored);
            if (!details) {
                auto fetched=deps.provider->fetchDetails(source,owningSeries,context);
                if (const auto *error=std::get_if<Error>(&fetched)) return *error;
                details=std::get<VodDetails>(fetched);
                auto written=deps.catalog->storeDetails(*details,context);
                if (const auto *error=std::get_if<Error>(&written)) return *error;
            }
            if (ref.kind == ContentKind::Series) {
                const auto history=deps.progress->readSeriesProgress(ref,context);
                if (const auto *error=std::get_if<Error>(&history)) return *error;
                auto episode=continueEpisode(*details,std::get<SeriesProgress>(history));
                if (!episode) { const auto first=orderedEpisodes(*details); if (!first.isEmpty()) episode=first.first(); }
                if (!episode) return Error{ErrorCode::ContentUnavailable,context.operationId};
                playable=episode->ref;
            }
        }
        auto descriptor = VodPlaybackResolver::resolve(deps, source, playable, preferences, context);
        if (const auto *error = std::get_if<Error>(&descriptor)) return *error;
        auto progress = deps.progress->read(playable, context);
        if (const auto *error = std::get_if<Error>(&progress)) return *error;
        auto saved = std::get<std::optional<VodProgress>>(progress);
        if (playable.kind == ContentKind::Episode && (!saved || saved->trackPreferences.isEmpty())) {
            // Current backend confirmation is newer than a queued checkpoint.
            // Saved preferences of the target still have first priority.
            if (preferences.inheritedTracks) {
                if (!saved) saved=VodProgress{};
                saved->trackPreferences=*preferences.inheritedTracks;
            } else {
                const auto history=deps.progress->readSeriesProgress(parentSeries(playable),context);
                if (const auto *error=std::get_if<Error>(&history)) return *error;
                const auto &series=std::get<SeriesProgress>(history);
                if (series.lastEpisode) {
                    if (!saved) saved=VodProgress{};
                    saved->trackPreferences=series.episodes.value(series.lastEpisode->key()).trackPreferences;
                }
            }
        }
        if (saved && saved->status == WatchStatus::Watched) saved->positionMs=0;
        if (preferences.trackPreferences) {
            if (!saved) saved = VodProgress{};
            saved->trackPreferences = *preferences.trackPreferences;
        }
        if (preferences.fromBeginning && saved) saved->positionMs = 0;
        return JobReply{source.revision, saved, std::get<PlaybackDescriptor>(descriptor)};
    }, OperationKind::Play);
}
QUuid VodController::movieLists(const ContentRef &ref)
{
    // submit transfers the callable into the owned asynchronous job queue.
    return submit(ref.profileId, [ref](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) { // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
        return reply(deps.lists->readMovieLists(ref, context), source.revision);
    });
}
QUuid VodController::progress(const ContentRef &ref)
{
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks) -- submit transfers the owning std::function to the job runner, which releases its captures on completion.
    return submit(ref.profileId, [ref](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
        if (!permitted(deps, source, ref, context)) return Error{ErrorCode::ContentUnavailable, context.operationId};
        if (ref.kind==ContentKind::Series) {
            const auto history=deps.progress->readSeriesProgress(ref,context);
            if (const auto *error=std::get_if<Error>(&history)) return *error;
            const auto &saved=std::get<SeriesProgress>(history);
            auto projected=saved.continuationEpisode ? std::optional<VodProgress>(saved.episodes.value(saved.continuationEpisode->key())) : std::nullopt;
            if (projected && (projected->status==WatchStatus::Watched || watchedByPosition(projected->positionMs,projected->durationMs))) {
                projected->positionMs=0;projected->status=WatchStatus::InProgress;
            }
            return JobReply{source.revision,projected,{}};
        }
        if (!ref.playable() || ref.catalogNamespace != source.revision.catalogNamespace)
            return Error{ErrorCode::ContentUnavailable, context.operationId};
        return reply(deps.progress->read(ref, context), source.revision);
    });
}
QUuid VodController::removeSource(const QUuid &profile)
{
    RequestContext context;
    const auto id = context.operationId;
    const auto deps = m_deps;
    // Durable intent and the publication barrier are established on a worker.
    // No source-store/database I/O is performed on the owner (GUI) thread.
    m_jobs.submit(profile, context, [deps, profile](const RequestContext &request) -> Result<JobReply> {
        if (const auto interrupted = request.interruption()) return *interrupted;
        const auto result = deps.sources->prepareRemoval(profile);
        if (const auto *error = std::get_if<Error>(&result)) return *error;
        return JobReply{{}, Success{}, {}};
    }, [this, id, profile, deps](Result<JobReply> barrier) {
        if (const auto *error = std::get_if<Error>(&barrier)) {
            if (completed) completed({id, profile, *error});
            return;
        }
        sourceChanged(profile);
        QPointer<VodController> self(this);
        m_coordinator.removeSource(profile, [self, id, profile, deps](Outcome stopped) {
            if (!self) return;
            if (const auto *error = std::get_if<Error>(&stopped)) {
                if (self->completed) self->completed({id, profile, *error});
                return; // Keep intent; next startup must finish interrupted removal.
            }
            RequestContext cleanup;
            cleanup.operationId = id;
            self->m_jobs.submit(profile, cleanup, [deps, profile](const RequestContext &request) -> Result<JobReply> {
                const auto catalog = deps.catalog->removeSourceState(profile);
                if (const auto *error = std::get_if<Error>(&catalog)) return *error;
                if (const auto interrupted = request.interruption()) return *interrupted;
                const auto progress = deps.progress->removeSourceState(profile);
                if (const auto *error = std::get_if<Error>(&progress)) return *error;
                const auto finished = deps.sources->finishRemoval(profile);
                if (const auto *error = std::get_if<Error>(&finished)) return *error;
                return JobReply{{}, Success{}, {}};
            }, [self, id, profile](Result<JobReply> result) {
                if (!self || !self->completed) return;
                if (auto *error = std::get_if<Error>(&result)) {
                    error->operationId = id;
                    self->completed({id, profile, *error});
                } else self->completed({id, profile, PublicValue{Success{}}});
            });
        });
    });
    return id;
}
void VodController::cancel(const QUuid &operation)
{
    if (m_playRequest == operation) m_playRequest = QUuid{};
    if (m_deferredPlay && m_deferredPlay->id == operation) {
        const auto profile = m_deferredPlay->profile;
        m_deferredPlay.reset();
        QTimer::singleShot(0, this, [this, operation, profile]() {
            deliver(operation, profile, Error{ErrorCode::Cancelled, operation}, true);
        });
    }
    m_jobs.cancel(operation);
    m_coordinator.cancel(operation);
}
void VodController::cancelPendingPlayback()
{
    const auto operation = m_playRequest;
    m_playRequest = QUuid{};
    if (!operation.isNull()) cancel(operation);
}
void VodController::sourceChanged(const QUuid &profile)
{
    if (m_playProfile == profile && !m_playRequest.isNull()) cancel(m_playRequest);
    if (m_deferredPlay && m_deferredPlay->profile == profile) {
        const auto id = m_deferredPlay->id;
        cancel(id);
    }
    m_jobs.cancelSource(profile);
}
void VodController::blockSource(const QUuid &profile) { m_blockedSources.insert(profile); m_recoveryJobs.cancelSource(profile); sourceChanged(profile); }
void VodController::unblockSource(const QUuid &profile) { m_blockedSources.remove(profile); }
void VodController::resolveForRecovery(const ContentRef &ref, VodPlaybackSession::RecoveryCompletion completion)
{
    if (m_stopped || m_blockedSources.contains(ref.profileId)) { completion(Error{ErrorCode::Cancelled, {}}); return; }
    const auto session = m_coordinator.sessionSnapshot();
    if (session.ref != ref || session.sessionToken.isNull() || session.end || session.state == SessionState::Idle) { completion(Error{ErrorCode::Cancelled, {}}); return; }
    const auto deps = m_deps;
    m_recoveryJobs.submit(ref.profileId, {}, [deps, ref](RequestContext context) -> Result<JobReply> {
        const auto source = deps.sources->snapshot(ref.profileId);
        if (const auto *error = std::get_if<Error>(&source)) return *error;
        auto snapshot = std::get<SourceContext>(source);
        context.source = snapshot.revision;
        snapshot.enabled = true; // Recovery is admitted only for the owning session below.
        auto result = VodPlaybackResolver::resolve(deps, snapshot, ref, {}, context);
        if (const auto *error = std::get_if<Error>(&result)) return *error;
        return JobReply{snapshot.revision, Success{}, std::get<PlaybackDescriptor>(result)};
    }, [this, deps, session, completion = std::move(completion)](Result<JobReply> result) {
        if (m_coordinator.sessionSnapshot().sessionToken != session.sessionToken || m_coordinator.sessionSnapshot().ref != session.ref || m_coordinator.sessionSnapshot().end) { completion(Error{ErrorCode::Cancelled, {}}); return; }
        if (const auto *error = std::get_if<Error>(&result)) { completion(*error); return; }
        const auto reply = std::get<JobReply>(result);
        if (!deps.sources->isCurrent(reply.source) || !reply.playback) { completion(Error{ErrorCode::Cancelled, {}}); return; }
        completion(*reply.playback);
    });
}
void VodController::shutdown()
{
    m_stopped = true;
    m_deferredPlay.reset();
    m_probeCooldown.stop();
    m_jobs.shutdown();
    m_recoveryJobs.shutdown();
}
}

namespace OKILTV::Vod {
QUuid VodController::seriesProgress(const ContentRef &ref) {
    // submit transfers the std::function to the bounded job queue; its RAII owner
    // releases it after completion/cancellation, beyond the analyzer call path.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    return submit(ref.profileId, [ref](const VodDependencies &deps,const SourceContext &source,const RequestContext &context) -> Result<JobReply> {
        if (!permitted(deps,source,ref,context)) return Error{ErrorCode::ContentUnavailable,context.operationId};
        return reply(deps.progress->readSeriesProgress(ref,context),source.revision);
    });
}
}
