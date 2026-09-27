#include "vodcontroller.h"
#include <QTimer>
#include <QPointer>

namespace OKILTV::Vod {
namespace {
template<class T> Result<JobReply> reply(Result<T> result, const SourceRevision &source)
{
    if (const auto *error = std::get_if<Error>(&result)) return *error;
    return JobReply{source, std::get<T>(std::move(result)), {}};
}
bool matches(const SourceContext &source, const CatalogScope &scope)
{ return source.revision.profileId == scope.profileId && source.revision.catalogNamespace == scope.catalogNamespace; }
}
VodController::VodController(VodDependencies deps, PlaybackCoordinator &coordinator, QObject *parent)
    : QObject(parent), m_deps(std::move(deps)), m_coordinator(coordinator) {}
QUuid VodController::submit(const QUuid &profile, Operation operation, bool play)
{
    RequestContext request;
    const auto id = request.operationId;
    if (play) {
        if (!m_playRequest.isNull()) cancel(m_playRequest);
        m_playRequest = id;
    }
    if (m_stopped || m_blockedSources.contains(profile)) {
        QTimer::singleShot(0, this, [this, id, profile]() { deliver(id, profile, Error{ErrorCode::Cancelled, id}, false); });
        return id;
    }
    const auto deps = m_deps;
    m_jobs.submit(profile, request, [deps, profile, operation = std::move(operation)](RequestContext context) -> Result<JobReply> {
        auto source = deps.sources->snapshot(profile);
        if (const auto *error = std::get_if<Error>(&source)) return *error;
        const auto snapshot = std::get<SourceContext>(source);
        context.source = snapshot.revision;
        if (!snapshot.enabled) return Error{ErrorCode::UnsupportedCapability, context.operationId};
        auto migrated = deps.migrations->prepare(context);
        if (const auto *error = std::get_if<Error>(&migrated)) return *error;
        if (const auto interrupted = context.interruption()) return *interrupted;
        return operation(deps, snapshot, context);
    }, [this, id, profile, play](Result<JobReply> result) { deliver(id, profile, std::move(result), play); });
    return id;
}
void VodController::deliver(const QUuid &id, const QUuid &profile, Result<JobReply> result, bool play)
{
    if (m_blockedSources.contains(profile)) result = Error{ErrorCode::Cancelled, id};
    if (play && id != m_playRequest) result = Error{ErrorCode::Cancelled, id};
    if (const auto *value = std::get_if<JobReply>(&result); value && !m_deps.sources->isCurrent(value->source))
        result = Error{ErrorCode::Cancelled, id};
    if (auto *error = std::get_if<Error>(&result)) {
        error->operationId = id;
        if (completed) completed({id, profile, *error});
        emit eventCompleted({id, profile, *error});
        return;
    }
    auto value = std::get<JobReply>(std::move(result));
    if (play && value.playback) {
        const auto progress = std::get<std::optional<VodProgress>>(value.value);
        m_coordinator.play(id, *value.playback, QUuid::createUuid(), progress,
            [this, id, profile](Outcome outcome) {
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
    });
}
QUuid VodController::query(const CatalogQuery &query)
{
    return submit(query.scope.profileId, [query](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
        if (!matches(source, query.scope) || query.pageSize < 1 || query.pageSize > 1000)
            return Error{ErrorCode::InvalidResponse, context.operationId};
        return reply(deps.catalog->query(query, context), source.revision);
    });
}
QUuid VodController::details(const ContentRef &ref)
{
    return submit(ref.profileId, [ref](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
        if (!ref.valid() || ref.catalogNamespace != source.revision.catalogNamespace)
            return Error{ErrorCode::ContentUnavailable, context.operationId};
        auto stored = deps.catalog->readDetails(ref, context);
        if (const auto *error = std::get_if<Error>(&stored)) return *error;
        if (const auto cached = std::get<std::optional<VodDetails>>(stored)) return JobReply{source.revision, *cached, {}};
        auto response = deps.provider->fetchDetails(source, ref, context);
        if (const auto *error = std::get_if<Error>(&response)) return *error;
        const auto value = std::get<VodDetails>(response);
        if (value.ref != ref) return Error{ErrorCode::InvalidResponse, context.operationId};
        auto saved = deps.catalog->storeDetails(value, context);
        if (const auto *error = std::get_if<Error>(&saved)) return *error;
        return JobReply{source.revision, value, {}};
    });
}
QUuid VodController::probe(const ContentRef &ref)
{
    return submit(ref.profileId, [ref](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
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
    });
}
QUuid VodController::refresh(const CatalogScope &scope)
{
    return submit(scope.profileId, [scope](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
        if (!matches(source, scope)) return Error{ErrorCode::ContentUnavailable, context.operationId};
        return reply(VodCatalogService::refresh(deps, source, scope, context), source.revision);
    });
}
QUuid VodController::play(const ContentRef &ref, const PlaybackPreferences &preferences)
{
    return submit(ref.profileId, [ref, preferences](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
        auto descriptor = VodPlaybackResolver::resolve(deps, source, ref, preferences, context);
        if (const auto *error = std::get_if<Error>(&descriptor)) return *error;
        auto progress = deps.progress->read(ref, context);
        if (const auto *error = std::get_if<Error>(&progress)) return *error;
        auto saved = std::get<std::optional<VodProgress>>(progress);
        if (preferences.trackPreferences) {
            if (!saved) saved = VodProgress{};
            saved->trackPreferences = *preferences.trackPreferences;
        }
        if (preferences.fromBeginning && saved) saved->positionMs = 0;
        return JobReply{source.revision, saved, std::get<PlaybackDescriptor>(descriptor)};
    }, true);
}
QUuid VodController::progress(const ContentRef &ref)
{
    return submit(ref.profileId, [ref](const VodDependencies &deps, const SourceContext &source, const RequestContext &context) -> Result<JobReply> {
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
        m_jobs.cancelSource(profile);
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
void VodController::cancel(const QUuid &operation) { m_jobs.cancel(operation); m_coordinator.cancel(operation); }
void VodController::cancelPendingPlayback()
{
    const auto operation = m_playRequest;
    m_playRequest = QUuid{};
    if (!operation.isNull()) cancel(operation);
}
void VodController::sourceChanged(const QUuid &profile) { m_jobs.cancelSource(profile); }
void VodController::blockSource(const QUuid &profile) { m_blockedSources.insert(profile); sourceChanged(profile); }
void VodController::unblockSource(const QUuid &profile) { m_blockedSources.remove(profile); }
void VodController::resolveForRecovery(const ContentRef &ref, VodPlaybackSession::RecoveryCompletion completion)
{
    if (m_stopped || m_blockedSources.contains(ref.profileId)) { completion(Error{ErrorCode::Cancelled, {}}); return; }
    const auto deps = m_deps;
    m_jobs.submit(ref.profileId, {}, [deps, ref](RequestContext context) -> Result<JobReply> {
        const auto source = deps.sources->snapshot(ref.profileId);
        if (const auto *error = std::get_if<Error>(&source)) return *error;
        const auto snapshot = std::get<SourceContext>(source);
        context.source = snapshot.revision;
        if (!snapshot.enabled) return Error{ErrorCode::UnsupportedCapability, context.operationId};
        auto result = VodPlaybackResolver::resolve(deps, snapshot, ref, {}, context);
        if (const auto *error = std::get_if<Error>(&result)) return *error;
        return JobReply{snapshot.revision, Success{}, std::get<PlaybackDescriptor>(result)};
    }, [deps, completion = std::move(completion)](Result<JobReply> result) {
        if (const auto *error = std::get_if<Error>(&result)) { completion(*error); return; }
        const auto reply = std::get<JobReply>(result);
        if (!deps.sources->isCurrent(reply.source) || !reply.playback) { completion(Error{ErrorCode::Cancelled, {}}); return; }
        completion(*reply.playback);
    });
}
void VodController::shutdown() { m_stopped = true; m_jobs.shutdown(); }
}
