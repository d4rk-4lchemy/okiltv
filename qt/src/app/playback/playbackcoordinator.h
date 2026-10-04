#pragma once
#include "legacyplaybackadapter.h"
#include "vodplaybacksession.h"
#include <QObject>

namespace OKILTV::Vod {
enum class PlaybackOwner { None, Legacy, Transition, Vod };
class PlaybackCoordinator final : public QObject {
public:
    using Completion = std::function<void(Outcome)>;
    PlaybackCoordinator(std::shared_ptr<LegacyPlaybackAdapter>, VodPlaybackSession &, QObject *parent = nullptr);
    ~PlaybackCoordinator() override;
    void play(QUuid operation, PlaybackDescriptor, QUuid session, std::optional<VodProgress>, Completion);
    void cancel(const QUuid &operation);
    void requestLive();
    void removeSource(const QUuid &profile, Completion);
    void shutdown();
    [[nodiscard]] PlaybackOwner owner() const { return m_owner; }
    [[nodiscard]] Player::IPlaybackEngine *activeEngine() const;
    [[nodiscard]] bool playing() const;
    [[nodiscard]] const SessionSnapshot &sessionSnapshot() const { return m_session.snapshot(); }
    std::function<void(const SessionSnapshot &, bool)> changed;
    std::function<bool(const SourceRevision &)> sourceIsCurrent;
private:
    struct Pending {
        QUuid operation;
        PlaybackDescriptor descriptor;
        QUuid session;
        std::optional<VodProgress> progress;
        Completion completion;
    };
    void acquire();
    void onSession(const SessionSnapshot &, bool);
    void rejectPending();
    std::shared_ptr<LegacyPlaybackAdapter> m_legacy;
    VodPlaybackSession &m_session;
    std::optional<Pending> m_pending;
    PlaybackOwner m_owner = PlaybackOwner::Legacy;
    bool m_shutdown = false;
    bool m_liveRequested = false;
    bool m_releasing = false;
    QUuid m_releaseToken;
    struct Removal { QUuid profile; QUuid token; Completion completion; };
    QList<Removal> m_removals;
};
}
