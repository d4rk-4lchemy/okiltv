#include "vodprogressservice.h"

namespace OKILTV::Vod {
VodProgressService::VodProgressService(VodDependencies deps, QObject *parent)
    : QObject(parent), m_deps(std::move(deps))
{
    m_timer.setInterval(15000);
    connect(&m_timer, &QTimer::timeout, this, [this]() { flush(); });
    m_timer.start();
}
VodProgressService::~VodProgressService() { shutdown(); }
void VodProgressService::observe(const SessionSnapshot &snapshot, bool checkpoint)
{
    if (m_stopped || !snapshot.ref.playable()) return;
    if (m_session != snapshot.sessionToken) {
        flush();
        m_snapshot.reset();
        m_dirty = false;
        m_session = snapshot.sessionToken;
        m_currentRef = snapshot.ref;
        m_sequence = 0;
        m_manualStatus.reset();
        m_completionQueued = false;
        RequestContext request;
        const auto deps = m_deps;
        m_jobs.submit(snapshot.ref.profileId, request, [deps, snapshot](RequestContext context) -> Result<JobReply> {
            auto source = deps.sources->snapshot(snapshot.ref.profileId);
            if (const auto *error = std::get_if<Error>(&source)) return *error;
            context.source = std::get<SourceContext>(source).revision;
            auto result = deps.progress->beginSession(snapshot.ref, snapshot.sessionToken, context);
            if (const auto *error = std::get_if<Error>(&result)) return *error;
            return JobReply{context.source, Success{}, {}};
        }, [this](Result<JobReply> result) { if (const auto *error = std::get_if<Error>(&result); error && failed) failed(*error); });
    }
    if (!snapshot.positionValid) return;
    m_dirty = m_dirty || !m_snapshot || m_snapshot->positionMs != snapshot.positionMs
        || m_snapshot->durationMs != snapshot.durationMs
        || m_snapshot->trackPreferences != snapshot.trackPreferences || checkpoint;
    m_snapshot = snapshot;
    if (checkpoint || (!m_completionQueued && observedProgress(snapshot).status == WatchStatus::Watched)) flush();
}
void VodProgressService::flush()
{
    if (!m_dirty || !m_snapshot || m_stopped) return;
    const auto snapshot = *m_snapshot;
    m_dirty = false;
    auto progress = observedProgress(snapshot);
    progress.sequence = ++m_sequence;
    const bool completed = !m_completionQueued && progress.status == WatchStatus::Watched;
    if (completed) m_completionQueued = true;
    RequestContext request;
    const auto deps = m_deps;
    m_jobs.submit(snapshot.ref.profileId, request, [deps, snapshot, progress, completed](RequestContext context) -> Result<JobReply> {
        auto source = deps.sources->snapshot(snapshot.ref.profileId);
        if (const auto *error = std::get_if<Error>(&source)) return *error;
        context.source = std::get<SourceContext>(source).revision;
        auto result = deps.progress->checkpoint(snapshot.ref, progress, context, completed);
        if (const auto *error = std::get_if<Error>(&result)) return *error;
        return JobReply{context.source, Success{}, {}};
    }, [this, ref = snapshot.ref, profile = snapshot.ref.profileId, session = snapshot.sessionToken, completed](Result<JobReply> result) {
        if (const auto *error = std::get_if<Error>(&result)) {
            m_writeErrors.insert(profile, *error);
            if (completed && session == m_session) { m_completionQueued = false; m_dirty = true; }
            if (failed) failed(*error);
        } else {
            m_writeErrors.remove(profile); emit persisted(ref);
            if (completed && ref.kind == ContentKind::Movie) emit movieListsChanged(ref);
        }
    });
}
VodProgress VodProgressService::observedProgress(const SessionSnapshot &snapshot) const
{
    VodProgress progress;
    progress.positionMs = snapshot.positionMs;
    progress.durationMs = snapshot.durationMs;
    progress.trackPreferences = snapshot.trackPreferences;
    progress.contentRevision = snapshot.contentRevision;
    progress.sessionToken = snapshot.sessionToken;
    progress.updatedAtUtc = QDateTime::currentDateTimeUtc();
    progress.status = watchedByPosition(progress.positionMs, progress.durationMs) ? WatchStatus::Watched : WatchStatus::InProgress;
    if (snapshot.sessionToken == m_session && m_manualStatus) {
        progress.status = *m_manualStatus ? WatchStatus::Watched : WatchStatus::InProgress;
        if (!*m_manualStatus) progress.positionMs = 0;
    }
    return progress;
}
void VodProgressService::setWatched(const ContentRef &ref, bool watched, std::function<void(Result<VodProgress>)> completion)
{
    if (m_stopped || !ref.playable()) { completion(Error{ErrorCode::Cancelled, {}}); return; }
    flush();
    const bool current = m_currentRef == ref && !m_session.isNull();
    const auto previous = m_manualStatus;
    const auto session = m_session;
    if (current) m_manualStatus = watched;
    const bool previousCompletion = m_completionQueued;
    if (current && watched) m_completionQueued = true;
    const auto observed = current && m_snapshot ? std::optional<VodProgress>(observedProgress(*m_snapshot)) : std::nullopt;
    const auto sequence = current ? ++m_sequence : 1;
    const auto deps = m_deps;
    m_jobs.submit(ref.profileId, {}, [deps, ref, watched, observed, sequence, current, session](RequestContext context) -> Result<JobReply> {
        auto source = deps.sources->snapshot(ref.profileId);
        if (const auto *error = std::get_if<Error>(&source)) return *error;
        context.source = std::get<SourceContext>(source).revision;
        auto saved = deps.progress->read(ref, context);
        if (const auto *error = std::get_if<Error>(&saved)) return *error;
        auto progress = observed.value_or(std::get<std::optional<VodProgress>>(saved).value_or(VodProgress{}));
        if (!current) {
            progress.sessionToken = QUuid::createUuid();
            auto begun = deps.progress->beginSession(ref, progress.sessionToken, context);
            if (const auto *error = std::get_if<Error>(&begun)) return *error;
        }
        if (current) progress.sessionToken = session;
        progress.sequence = sequence;
        progress.status = watched ? WatchStatus::Watched : WatchStatus::InProgress;
        if (!watched) progress.positionMs = 0;
        progress.updatedAtUtc = QDateTime::currentDateTimeUtc();
        auto written = deps.progress->checkpoint(ref, progress, context, watched);
        if (const auto *error = std::get_if<Error>(&written)) return *error;
        return JobReply{context.source, std::optional<VodProgress>{progress}, {}};
    }, [this, ref, current, previous, previousCompletion, watched, session, completion = std::move(completion)](Result<JobReply> result) {
        if (const auto *error = std::get_if<Error>(&result)) {
            if (current && session == m_session) { m_manualStatus = previous; m_completionQueued = previousCompletion; m_dirty = true; }
            completion(*error);
        } else {
            const auto saved = std::get<std::optional<VodProgress>>(std::get<JobReply>(result).value);
            completion(saved.value());
            emit persisted(ref);
            if (watched && ref.kind == ContentKind::Movie) emit movieListsChanged(ref);
        }
        flush();
    });
}
void VodProgressService::setMovieList(const ContentRef &ref, MovieList list, bool enabled, std::function<void(Result<MovieListState>)> completion)
{
    if (m_stopped || !ref.valid() || ref.kind != ContentKind::Movie) { completion(Error{ErrorCode::Cancelled, {}}); return; }
    if (m_listWrites.contains(ref.key())) { completion(Error{ErrorCode::Cancelled, {}}); return; }
    m_listWrites.insert(ref.key()); emit movieListWritePendingChanged(ref);
    const auto deps = m_deps;
    m_jobs.submit(ref.profileId, {}, [deps, ref, list, enabled](RequestContext context) -> Result<JobReply> {
        const auto source = deps.sources->snapshot(ref.profileId);
        if (const auto *error = std::get_if<Error>(&source)) return *error;
        context.source = std::get<SourceContext>(source).revision;
        const auto result = deps.lists->setMovieList(ref, list, enabled, context);
        if (const auto *error = std::get_if<Error>(&result)) return *error;
        return JobReply{context.source, std::get<MovieListState>(result), {}};
    }, [this, ref, completion = std::move(completion)](Result<JobReply> result) {
        m_listWrites.remove(ref.key()); emit movieListWritePendingChanged(ref);
        if (const auto *error = std::get_if<Error>(&result)) completion(*error);
        else {
            completion(std::get<MovieListState>(std::get<JobReply>(result).value));
            emit movieListsChanged(ref);
        }
    });
}
void VodProgressService::flushSource(const QUuid &profile, std::function<void(Outcome)> completion)
{
    if (m_stopped) { completion(Error{ErrorCode::Cancelled, {}}); return; }
    // A sentinel on the serial lane acknowledges all preceding writes. The
    // caller may only publish changed source credentials after this callback.
    if (m_snapshot && m_snapshot->ref.profileId == profile) {
        m_dirty = true;
        flush();
    }
    m_jobs.submit(profile, {}, [](const RequestContext &) -> Result<JobReply> { return JobReply{}; },
        [this, profile, completion = std::move(completion)](Result<JobReply> result) {
            if (const auto *error = std::get_if<Error>(&result)) completion(*error);
            else if (m_writeErrors.contains(profile)) completion(m_writeErrors.value(profile));
            else completion(Success{});
        });
}
void VodProgressService::shutdown()
{
    if (m_stopped) return;
    flush();
    m_stopped = true;
    m_timer.stop();
    if (!m_jobs.drain(1000) && failed) failed(Error{ErrorCode::Timeout, {}});
    m_jobs.shutdown();
}
}
