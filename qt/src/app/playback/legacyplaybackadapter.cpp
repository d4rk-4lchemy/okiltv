#include "legacyplaybackadapter.h"
namespace OKILTV::Vod {
void LegacyPlaybackAdapter::release(const QUuid &request, Acknowledgement ack)
{
    if (!m_hooks.resources || !m_hooks.release || !m_hooks.activateLive) {
        ack(Error{ErrorCode::PlaybackConflict, request});
        return;
    }
    const auto resources = m_hooks.resources();
    if (resources.engineBoundRecording
        || (resources.independentRecording && !resources.independentRecordingAllowsVod)) {
        ack(Error{ErrorCode::PlaybackConflict, request});
        return;
    }
    m_hooks.release(request, [request, resourcesNow = m_hooks.resources, ack = std::move(ack)](Outcome outcome) {
        if (std::holds_alternative<Success>(outcome)) {
            const auto current = resourcesNow();
            if (current.engineBoundRecording
                || (current.independentRecording && !current.independentRecordingAllowsVod)) {
                ack(Error{ErrorCode::PlaybackConflict, request});
                return;
            }
        }
        ack(outcome);
    });
}
void LegacyPlaybackAdapter::activateLive() { if (m_hooks.activateLive) m_hooks.activateLive(); }
}
