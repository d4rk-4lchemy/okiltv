#include "playbackcoordinator.h"
#include <QPointer>
#include <QTimer>

namespace OKILTV::Vod {
PlaybackCoordinator::PlaybackCoordinator(std::shared_ptr<LegacyPlaybackAdapter> legacy, VodPlaybackSession &session, QObject *parent)
    : QObject(parent), m_legacy(std::move(legacy)), m_session(session)
{
    m_session.changed = [this](const SessionSnapshot &snapshot, bool checkpoint) { onSession(snapshot, checkpoint); };
}
PlaybackCoordinator::~PlaybackCoordinator() { shutdown(); m_session.changed = {}; }
void PlaybackCoordinator::rejectPending()
{
    if (!m_pending) return;
    auto pending = std::move(*m_pending);
    m_pending.reset();
    pending.completion(Error{ErrorCode::Cancelled, pending.operation});
}
void PlaybackCoordinator::play(QUuid operation, PlaybackDescriptor descriptor, QUuid session,
    std::optional<VodProgress> progress, Completion completion)
{
    if (m_shutdown) { completion(Error{ErrorCode::Cancelled, operation}); return; }
    rejectPending();
    m_liveRequested = false;
    m_pending = Pending{operation, std::move(descriptor), session, std::move(progress), std::move(completion)};
    if (m_owner == PlaybackOwner::Vod) m_session.stop(Player::EndReason::Replaced);
    else acquire();
}
void PlaybackCoordinator::acquire()
{
    if (!m_pending || m_releasing || m_shutdown) return;
    m_releasing = true;
    const auto releaseToken = QUuid::createUuid();
    m_releaseToken = releaseToken;
    m_owner = PlaybackOwner::Transition;
    const auto operation = m_pending->operation;
    QTimer::singleShot(10000, this, [this, operation]() {
        if (!m_pending || m_pending->operation != operation || !m_releasing) return;
        auto pending = std::move(*m_pending);
        m_pending.reset();
        // Keep the transition locked until actual resource release is acknowledged.
        pending.completion(Error{ErrorCode::Timeout, operation});
    });
    QPointer<PlaybackCoordinator> self(this);
    m_legacy->release(operation, [self, operation, releaseToken](Outcome outcome) {
        if (!self || !self->m_releasing || self->m_releaseToken != releaseToken) return;
        self->m_releasing = false;
        self->m_releaseToken = QUuid{};
        if (self->m_shutdown) return;
        if (!self->m_pending || self->m_pending->operation != operation) {
            self->m_owner = PlaybackOwner::None;
            if (self->m_liveRequested) self->requestLive();
            else self->acquire();
            return;
        }
        auto pending = std::move(*self->m_pending);
        self->m_pending.reset();
        if (const auto *error = std::get_if<Error>(&outcome)) {
            self->m_owner = PlaybackOwner::Legacy;
            pending.completion(*error);
            return;
        }
        if (!self->sourceIsCurrent || !self->sourceIsCurrent(pending.descriptor.source)) {
            self->m_owner = PlaybackOwner::None;
            pending.completion(Error{ErrorCode::Cancelled, operation});
            return;
        }
        self->m_owner = PlaybackOwner::Vod;
        auto opened = self->m_session.open(pending.descriptor, pending.session, pending.progress);
        if (std::holds_alternative<Error>(opened)) self->m_owner = PlaybackOwner::None;
        pending.completion(opened);
    });
}
void PlaybackCoordinator::cancel(const QUuid &operation)
{
    if (m_pending && m_pending->operation == operation) rejectPending();
}
void PlaybackCoordinator::requestLive()
{
    if (m_shutdown) return;
    rejectPending();
    m_liveRequested = true;
    if (m_owner == PlaybackOwner::Vod) { m_session.stop(); return; }
    if (m_releasing) return;
    m_liveRequested = false;
    m_owner = PlaybackOwner::Legacy;
    m_legacy->activateLive();
}
void PlaybackCoordinator::removeSource(const QUuid &profile, Completion completion)
{
    if (m_pending && m_pending->descriptor.ref.profileId == profile) rejectPending();
    if (m_owner != PlaybackOwner::Vod || m_session.snapshot().ref.profileId != profile) {
        completion(Success{});
        return;
    }
    const auto token = QUuid::createUuid();
    m_removals.append({profile, token, std::move(completion)});
    QTimer::singleShot(10000, this, [this, token]() {
        for (qsizetype index = 0; index < m_removals.size(); ++index) {
            if (m_removals[index].token != token) continue;
            auto removal = m_removals.takeAt(index);
            removal.completion(Error{ErrorCode::Timeout, token});
            break;
        }
    });
    m_session.stop();
}
void PlaybackCoordinator::shutdown()
{
    if (m_shutdown) return;
    m_shutdown = true;
    rejectPending();
    m_session.stop(Player::EndReason::Shutdown);
}
Player::IPlaybackEngine *PlaybackCoordinator::activeEngine() const
{ return m_owner == PlaybackOwner::Vod ? m_session.engine() : nullptr; }
bool PlaybackCoordinator::playing() const
{ return m_owner == PlaybackOwner::Vod && m_session.snapshot().state == SessionState::Playing; }
void PlaybackCoordinator::onSession(const SessionSnapshot &snapshot, bool checkpoint)
{
    if (changed) changed(snapshot, checkpoint);
    if (!snapshot.end) return;
    m_owner = PlaybackOwner::None;
    auto removals = std::move(m_removals);
    m_removals.clear();
    for (auto &removal : removals) removal.completion(Success{});
    if (m_shutdown) return;
    if (m_liveRequested) requestLive();
    else acquire();
}
}
