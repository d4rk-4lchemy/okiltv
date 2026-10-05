#include "vodplaybacksession.h"
#include <QTimer>
#include <QPointer>
#include <algorithm>

namespace OKILTV::Vod {
VodPlaybackSession::VodPlaybackSession(std::unique_ptr<Player::IPlaybackEngine> engine, QObject *parent)
    : QObject(parent), m_engine(std::move(engine))
{
    if (m_engine) m_engine->setListener([this](const Player::PlaybackEvent &value) { event(value); });
}
VodPlaybackSession::VodPlaybackSession(std::function<std::unique_ptr<Player::IPlaybackEngine>()> factory, QObject *parent)
    : QObject(parent), m_factory(std::move(factory)) {}
VodPlaybackSession::~VodPlaybackSession()
{
    if (m_engine) {
        m_engine->setListener({});
        if (!m_finished) m_engine->stop(Player::EndReason::Shutdown);
    }
}
Outcome VodPlaybackSession::open(const PlaybackDescriptor &descriptor, const QUuid &session,
    std::optional<VodProgress> resumeProgress, bool startPaused)
{
    if (!m_finished) return Error{ErrorCode::PlaybackConflict, {}};
    if (!descriptor.ref.playable() || descriptor.source.profileId != descriptor.ref.profileId
        || descriptor.source.catalogNamespace != descriptor.ref.catalogNamespace
        || !descriptor.mediaUri.isValid() || descriptor.mediaUri.host().isEmpty()
        || (descriptor.mediaUri.scheme() != QStringLiteral("http") && descriptor.mediaUri.scheme() != QStringLiteral("https"))
        || !descriptor.expiresAtUtc.isValid() || descriptor.expiresAtUtc <= QDateTime::currentDateTimeUtc()) {
        return Error{ErrorCode::ContentUnavailable, {}};
    }
    if (!m_engine && m_factory) {
        m_engine = m_factory();
        if (m_engine) m_engine->setListener([this](const Player::PlaybackEvent &value) { event(value); });
    }
    if (!m_engine) return Error{ErrorCode::PlaybackFailed, {}};
    if (m_snapshot.sessionToken != session) m_retries = 0;
    m_snapshot = {};
    m_snapshot.ref = descriptor.ref;
    m_snapshot.sessionToken = session;
    m_snapshot.loadToken = QUuid::createUuid();
    m_snapshot.state = SessionState::Opening;
    m_snapshot.contentRevision = descriptor.contentRevision;
    m_snapshot.pauseRequested = startPaused;
    if (resumeProgress) m_snapshot.trackPreferences = resumeProgress->trackPreferences;
    m_finished = false;
    m_stopping = false;
    m_resumeAwaiting = false;
    m_loaded = false;
    m_initialProgress = resumeProgress;
    m_resumePosition.reset();
    m_resumeDuration.reset();
    if (resumeProgress && resumeProgress->status != WatchStatus::Watched
        && (m_recoveryOpening || !watchedByPosition(resumeProgress->positionMs, resumeProgress->durationMs)) && resumeProgress->positionMs >= (m_recoveryOpening ? 0 : 60000)
        && resumeProgress->contentRevision == descriptor.contentRevision) {
        m_resumePosition = m_recoveryOpening ? resumeProgress->positionMs : resumeProgress->positionMs - 5000;
        m_resumeDuration = resumeProgress->durationMs;
    }
    Player::PlaybackRequest request;
    request.mediaUri = descriptor.mediaUri;
    request.allowedHeaders = descriptor.allowedHeaders;
    request.transport = Player::TransportPolicy::NativeMedia;
    request.policy = Player::PlaybackPolicy::OnDemand;
    request.validatedEngineOptions.insert(QStringLiteral("keep-open"), QStringLiteral("no"));
    request.startPaused = startPaused;
    request.trackPreferences = m_snapshot.trackPreferences;
    if (descriptor.subtitleSelection) {
        if (descriptor.subtitleSelection->isEmpty()) request.trackPreferences.remove(QStringLiteral("sub"));
        else request.trackPreferences.insert(QStringLiteral("sub"), *descriptor.subtitleSelection);
    }
    request.externalSubtitles = descriptor.externalSubtitles;
    if (changed) changed(m_snapshot, false);
    m_engine->load(request, m_snapshot.loadToken);
    return Success{};
}
void VodPlaybackSession::stop(Player::EndReason reason)
{
    if (!m_finished && m_engine) {
        m_stopping = true;
        if (m_recovering) {
            m_recovering = false;
            m_recoveryToken = QUuid{};
            // The failed load already delivered END_FILE. No further backend
            // stop acknowledgement exists while only the resolver is running.
            Player::PlaybackEvent ended;
            ended.loadToken = m_snapshot.loadToken;
            ended.end = reason;
            event(ended);
            return;
        }
        if (changed) changed(m_snapshot, true);
        m_engine->stop(reason); // Ownership is retained until matching stop ack.
    }
}
void VodPlaybackSession::pause() { if (!m_finished && m_engine) { m_snapshot.pauseRequested = true; m_engine->pause(); } }
void VodPlaybackSession::resume() { if (!m_finished && m_engine) { m_snapshot.pauseRequested = false; m_engine->resume(); } }
void VodPlaybackSession::seek(qint64 position)
{
    if (m_finished || !m_snapshot.seekable || !m_engine) return;
    position = std::max(qint64(0), position);
    if (const auto duration = m_snapshot.durationMs) position = std::min(position, *duration);
    m_engine->seek(position);
}
void VodPlaybackSession::setVolume(double percent) { if (m_engine) m_engine->setVolume(std::clamp(percent, 0.0, 100.0)); }
void VodPlaybackSession::event(const Player::PlaybackEvent &value)
{
    if (m_finished || m_recovering || value.loadToken != m_snapshot.loadToken) return;
    bool checkpoint = false;
    if (!value.end) {
        if (value.videoWidth && value.videoHeight && *value.videoWidth > 0 && *value.videoHeight > 0) {
            m_snapshot.videoWidth = value.videoWidth;
            m_snapshot.videoHeight = value.videoHeight;
        }
        if (!value.tracks.isEmpty()) m_snapshot.tracks = value.tracks;
        if (const auto preferences = value.trackPreferences; preferences && *preferences != m_snapshot.trackPreferences) {
            m_snapshot.trackPreferences = *preferences;
            checkpoint = true;
        }
    }
    // End/stop events may already expose a zeroed engine; retain last good time.
    if (!value.end && value.positionMs >= 0 && (!m_resumeAwaiting || value.seekCompleted)
        && (value.state == Player::EngineState::Playing || value.state == Player::EngineState::Paused || value.seekCompleted)) {
        m_snapshot.positionMs = value.positionMs;
        m_snapshot.positionValid = true;
    }
    if (const auto duration = value.durationMs; duration && *duration > 0) m_snapshot.durationMs = duration;
    m_snapshot.seekable = value.seekable;
    m_snapshot.buffering = value.state == Player::EngineState::Buffering;
    if (const auto end = value.end) {
        if (*end == Player::EndReason::Error && value.retryable && !m_stopping && recover && m_retries < 2) {
            recoverAfterFailure();
            return;
        }
        m_finished = true;
        m_resumePosition.reset();
        m_snapshot.end = end;
        m_snapshot.state = *end == Player::EndReason::NaturalEnd ? SessionState::Ended
            : (*end == Player::EndReason::Error || *end == Player::EndReason::Unknown
                ? SessionState::Failed : SessionState::Stopped);
        checkpoint = true;
    } else if (value.state == Player::EngineState::Loaded && !m_loaded) {
        m_loaded = true;
        const auto resumePosition = m_resumePosition;
        const auto previousDuration = m_resumeDuration;
        const auto duration = m_snapshot.durationMs;
        m_resumePosition.reset();
        const bool comparable = !previousDuration || (duration && std::abs(*duration - *previousDuration) <= std::max(qint64(30000), *previousDuration / 5));
        if (resumePosition && value.seekable && duration && comparable && *resumePosition < *duration) {
            m_snapshot.state = SessionState::SeekingResume;
            m_resumeAwaiting = true;
            const auto token = m_snapshot.loadToken;
            // Remote exact seeks may need new video/audio ranges and decoder
            // preroll. Keep the spinner and wait for the matching backend event.
            QTimer::singleShot(60000, this, [this, token]() {
                if (!m_finished && m_snapshot.loadToken == token && m_resumeAwaiting)
                    stop(Player::EndReason::Error);
            });
            seek(*resumePosition);
        }
    } else if (value.state == Player::EngineState::Playing && !m_resumeAwaiting) {
        m_snapshot.state = SessionState::Playing;
    } else if (value.state == Player::EngineState::Paused && !m_resumeAwaiting) {
        checkpoint = checkpoint || m_snapshot.state != SessionState::Paused;
        m_snapshot.state = SessionState::Paused;
    }
    if (value.seekCompleted) {
        m_resumeAwaiting = false;
        if (m_snapshot.state == SessionState::SeekingResume)
            m_snapshot.state = m_snapshot.pauseRequested ? SessionState::Paused : SessionState::Playing;
        checkpoint = true;
    }
    if (changed) changed(m_snapshot, checkpoint);
}
void VodPlaybackSession::recoverAfterFailure()
{
    ++m_retries;
    m_recovering = true;
    m_snapshot.state = SessionState::Recovering;
    m_snapshot.buffering = false;
    const auto token = QUuid::createUuid();
    m_recoveryToken = token;
    if (changed) changed(m_snapshot, true);
    QPointer<VodPlaybackSession> self(this);
    // A provider may still be releasing the previous media connection after
    // returning 503. Immediate retries exhaust the budget without giving it time.
    const int retryDelayMs = m_retries == 1 ? 2000 : 5000;
    QTimer::singleShot(retryDelayMs, this, [self, token]() {
        if (!self || !self->m_recovering || self->m_recoveryToken != token) return;
        self->recover(self->m_snapshot.ref, [self, token](Result<PlaybackDescriptor> result) {
            if (!self || !self->m_recovering || self->m_recoveryToken != token) return;
            self->m_recovering = false;
            const auto previous = self->m_snapshot;
            auto progress = self->m_initialProgress;
            if (previous.positionValid) {
                VodProgress observed;
                observed.positionMs = previous.positionMs;
                observed.durationMs = previous.durationMs;
                observed.contentRevision = previous.contentRevision;
                progress = observed;
            }
            if (progress) progress->trackPreferences = previous.trackPreferences;
            if (const auto *descriptor = std::get_if<PlaybackDescriptor>(&result); descriptor && descriptor->ref == previous.ref) {
                self->m_finished = true;
                self->m_recoveryOpening = true;
                const auto opened = self->open(*descriptor, previous.sessionToken, progress, previous.pauseRequested);
                self->m_recoveryOpening = false;
                if (std::holds_alternative<Success>(opened)) return;
                self->m_snapshot = previous;
                self->m_finished = false;
            }
            Player::PlaybackEvent ended;
            ended.loadToken = self->m_snapshot.loadToken;
            ended.end = Player::EndReason::Error;
            self->event(ended);
        });
    });
}
}
