#pragma once

#include "../../player/mpvplayer.h"
#include <cmath>

namespace OKILTV::App::Playback {

// Shared by ordinary Live refill and reconnect refill. All decisions use the
// same generation/age-fenced, playback-relative observation as F3.
struct LiveBufferPolicy {
    static constexpr double depletionSeconds = 0.2;
    static constexpr double resumeToleranceSeconds = 0.3;
    static constexpr qint64 confirmationMs = 500;
    static constexpr qint64 maximumHoldMs = 30000;

    static bool valid(const Player::MpvPlayer::PlaybackBufferSnapshot &sample)
    {
        return sample.fresh && sample.seconds && std::isfinite(*sample.seconds) && *sample.seconds >= 0.0;
    }
    static bool ready(const Player::MpvPlayer::PlaybackBufferSnapshot &sample, double target)
    {
        return valid(sample) && sample.seconds.value_or(0.0) + resumeToleranceSeconds >= target;
    }
};

class LiveDepletion final {
public:
    void reset() { m_startedMs.reset(); m_lastObservedMs.reset(); m_lastSampleMs.reset(); }
    bool observe(const Player::MpvPlayer::PlaybackBufferSnapshot &sample, qint64 nowMs)
    {
        if (!LiveBufferPolicy::valid(sample) || sample.seconds.value_or(0.0) > LiveBufferPolicy::depletionSeconds
            || sample.readerIdle.value_or(false) || sample.readerEof.value_or(false)) {
            reset();
            return false;
        }
        // Reading one cached observation repeatedly cannot confirm depletion.
        const auto sampleMs = sample.sampledAtMs.value_or(nowMs);
        if (m_lastSampleMs && sampleMs <= *m_lastSampleMs) return false;
        if (!m_lastObservedMs || nowMs < *m_lastObservedMs || nowMs - *m_lastObservedMs > 1500)
            m_startedMs = nowMs;
        m_lastSampleMs = sampleMs;
        m_lastObservedMs = nowMs;
        return m_startedMs && nowMs - *m_startedMs >= LiveBufferPolicy::confirmationMs;
    }
private:
    std::optional<qint64> m_startedMs;
    std::optional<qint64> m_lastObservedMs;
    std::optional<qint64> m_lastSampleMs;
};

} // namespace OKILTV::App::Playback
