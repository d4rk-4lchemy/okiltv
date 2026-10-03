#pragma once
#include "vodcontroller.h"
#include "vodprogressservice.h"

namespace OKILTV::Vod {
struct VodOptions { bool enabled = false; };
struct VodComposition {
    VodDependencies dependencies;
    std::shared_ptr<LegacyPlaybackAdapter> legacy;
    std::function<std::unique_ptr<Player::IPlaybackEngine>()> createEngine;
};
// No production adapters in stage A. Disabled composition neither calls the
// factory nor initializes a backend, starts jobs, writes settings or migrates SQL.
class VodModule final {
public:
    using Factory = std::function<VodComposition()>;
    explicit VodModule(VodOptions options = {}, const Factory &factory = {});
    ~VodModule();
    [[nodiscard]] bool enabled() const { return m_controller != nullptr; }
    [[nodiscard]] std::optional<Error> initializationError() const { return m_error; }
    [[nodiscard]] VodController *controller() const { return m_controller.get(); }
    [[nodiscard]] PlaybackCoordinator *coordinator() const { return m_coordinator.get(); }
    [[nodiscard]] VodPlaybackSession *session() const { return m_session.get(); }
    [[nodiscard]] VodProgressService *progressService() const { return m_progress.get(); }
    void shutdown();
    void prepareRecording(PlaybackCoordinator::Completion);
    // Blocks new requests, checkpoints and acknowledges stop before editing
    // credentials. The integration must call finishSourceChange afterwards.
    void prepareSourceChange(const QUuid &, PlaybackCoordinator::Completion);
    void finishSourceChange(const QUuid &);
    std::function<void(const Error &)> progressFailed;
    std::function<void(const SessionSnapshot &)> changed;
private:
    std::optional<Error> m_error;
    std::unique_ptr<VodPlaybackSession> m_session;
    std::unique_ptr<PlaybackCoordinator> m_coordinator;
    std::unique_ptr<VodProgressService> m_progress;
    std::unique_ptr<VodController> m_controller;
    bool m_stopped = false;
};
}
