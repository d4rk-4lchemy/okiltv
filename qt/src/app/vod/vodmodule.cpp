#include "vodmodule.h"
namespace OKILTV::Vod {
VodModule::VodModule(VodOptions options, const Factory &factory)
{
    if (!options.enabled) return;
    if (!factory) { m_error = Error{ErrorCode::UnsupportedCapability, {}}; return; }
    auto composition = factory();
    if (!composition.dependencies.complete() || !composition.legacy || !composition.createEngine) {
        m_error = Error{ErrorCode::UnsupportedCapability, {}};
        return;
    }
    m_session = std::make_unique<VodPlaybackSession>(std::move(composition.createEngine));
    m_coordinator = std::make_unique<PlaybackCoordinator>(composition.legacy, *m_session);
    const auto sources = composition.dependencies.sources;
    m_coordinator->sourceIsCurrent = [sources](const SourceRevision &source) { return sources->isCurrent(source); };
    m_progress = std::make_unique<VodProgressService>(composition.dependencies);
    m_coordinator->changed = [this](const SessionSnapshot &snapshot, bool checkpoint) {
        m_progress->observe(snapshot, checkpoint);
        if (changed) changed(snapshot);
    };
    m_progress->failed = [this](const Error &error) { if (progressFailed) progressFailed(error); };
    m_controller = std::make_unique<VodController>(composition.dependencies, *m_coordinator);
    m_session->recover = [this](const ContentRef &ref, VodPlaybackSession::RecoveryCompletion completion) {
        m_controller->resolveForRecovery(ref, std::move(completion));
    };
}
VodModule::~VodModule()
{
    shutdown();
    if (m_coordinator) m_coordinator->changed = {};
}
void VodModule::prepareRecording(PlaybackCoordinator::Completion completion)
{
    if (!enabled()) { completion(Success{}); return; }
    if (m_stopped) { completion(Error{ErrorCode::Cancelled, {}}); return; }
    if (playbackInterrupted) playbackInterrupted();
    m_controller->cancelPendingPlayback();
    const auto snapshot = m_session->snapshot();
    if (!snapshot.ref.playable()) { completion(Success{}); return; }
    const auto profile = snapshot.ref.profileId;
    m_coordinator->removeSource(profile, [this, profile, completion = std::move(completion)](Outcome stopped) {
        if (const auto *error = std::get_if<Error>(&stopped)) { completion(*error); return; }
        m_progress->flushSource(profile, completion);
    });
}
void VodModule::prepareSourceChange(const QUuid &profile, PlaybackCoordinator::Completion completion)
{
    if (!enabled()) { completion(Success{}); return; }
    if (m_stopped) { completion(Error{ErrorCode::Cancelled, {}}); return; }
    if (playbackInterrupted) playbackInterrupted();
    m_controller->blockSource(profile);
    m_coordinator->removeSource(profile, [this, profile, completion = std::move(completion)](Outcome stopped) {
        if (const auto *error = std::get_if<Error>(&stopped)) { completion(*error); return; }
        m_progress->flushSource(profile, completion);
    });
}
void VodModule::finishSourceChange(const QUuid &profile)
{
    if (m_controller) m_controller->unblockSource(profile);
}
void VodModule::shutdown()
{
    if (m_stopped) return;
    m_stopped = true;
    if (m_coordinator) m_coordinator->shutdown();
    if (m_controller) m_controller->shutdown();
    if (m_progress) m_progress->shutdown();
}
}
