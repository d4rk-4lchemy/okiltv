#pragma once
#include "core/vod/vodmodels.h"
#include "player/iplaybackengine.h"
#include <QObject>

namespace OKILTV::Vod {
enum class SessionState { Idle, Opening, SeekingResume, Playing, Paused, Recovering, Ended, Stopped, Failed };
struct SessionSnapshot {
    ContentRef ref;
    QUuid sessionToken;
    QUuid loadToken;
    SessionState state = SessionState::Idle;
    qint64 positionMs = 0;
    std::optional<qint64> durationMs;
    bool seekable = false;
    bool positionValid = false;
    std::optional<QString> contentRevision;
    bool buffering = false;
    bool pauseRequested = false;
    std::optional<Player::EndReason> end;
    QJsonObject trackPreferences;
};
class VodPlaybackSession final : public QObject {
public:
    explicit VodPlaybackSession(std::unique_ptr<Player::IPlaybackEngine>, QObject *parent = nullptr);
    explicit VodPlaybackSession(std::function<std::unique_ptr<Player::IPlaybackEngine>()>, QObject *parent = nullptr);
    ~VodPlaybackSession() override;
    Outcome open(const PlaybackDescriptor &, const QUuid &session, std::optional<VodProgress> resume, bool startPaused = false);
    void stop(Player::EndReason reason = Player::EndReason::UserStop);
    void pause();
    void resume();
    void seek(qint64 positionMs);
    void setVolume(double percent);
    [[nodiscard]] const SessionSnapshot &snapshot() const { return m_snapshot; }
    [[nodiscard]] Player::IPlaybackEngine *engine() const { return m_engine.get(); }
    // Owner-thread event; safe DTO only. Checkpoint final snapshot before reset.
    std::function<void(const SessionSnapshot &, bool checkpoint)> changed;
    using RecoveryCompletion = std::function<void(Result<PlaybackDescriptor>)>;
    std::function<void(const ContentRef &, RecoveryCompletion)> recover;
private:
    void event(const Player::PlaybackEvent &);
    void recoverAfterFailure();
    std::unique_ptr<Player::IPlaybackEngine> m_engine;
    std::function<std::unique_ptr<Player::IPlaybackEngine>()> m_factory;
    SessionSnapshot m_snapshot;
    std::optional<qint64> m_resumePosition;
    std::optional<qint64> m_resumeDuration;
    std::optional<VodProgress> m_initialProgress;
    int m_retries = 0;
    bool m_recovering = false;
    bool m_recoveryOpening = false;
    bool m_stopping = false;
    bool m_resumeAwaiting = false;
    QUuid m_recoveryToken;
    bool m_finished = true;
    bool m_loaded = false;
};
}
