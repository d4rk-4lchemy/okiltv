#pragma once
#include "core/vod/voderrors.h"
#include <functional>

namespace OKILTV::Vod {
struct LegacyResources {
    bool engineBoundRecording = false;
    bool independentRecording = false;
    bool independentRecordingAllowsVod = false;
};
// Callback adapter keeps legacy headers out of VOD. Stage B4 binds these hooks
// to the ACTUAL primary, retained PiP, standby and timeshift controllers.
class LegacyPlaybackAdapter {
public:
    using Acknowledgement = std::function<void(Outcome)>;
    struct Hooks {
        std::function<LegacyResources()> resources;
        // Ack success only when ALL conflicting resources are released.
        std::function<void(const QUuid &, Acknowledgement)> release;
        // Already-approved internal entrypoint; must not re-enter coordinator.
        std::function<void()> activateLive;
    };
    explicit LegacyPlaybackAdapter(Hooks hooks) : m_hooks(std::move(hooks)) {}
    void release(const QUuid &request, Acknowledgement ack);
    void activateLive();
private:
    Hooks m_hooks;
};
}
