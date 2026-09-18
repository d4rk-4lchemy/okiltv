#include "livebuffertuner.h"

#include <algorithm>
#include <cmath>

namespace OKILTV::Player {

void LiveBufferTuner::reset()
{
    *this = LiveBufferTuner {};
}

void LiveBufferTuner::interruptObservation()
{
    m_lastSample.reset();
    m_lastEnd.reset();
    m_waitStarted.reset();
    m_lastBurst.reset();
    m_intervals.clear();
}

void LiveBufferTuner::observe(const double now, const double cacheEnd, const bool readerIdle)
{
    if (!std::isfinite(now) || !std::isfinite(cacheEnd)) {
        interruptObservation();
        return;
    }
    if (m_lastSample.has_value()
        && (now <= *m_lastSample || now - *m_lastSample > 1.0
            || (m_lastEnd.has_value() && cacheEnd < *m_lastEnd - 0.1))) {
        interruptObservation();
    }
    const auto advance = m_lastEnd.has_value() ? cacheEnd - *m_lastEnd : 0.0;
    if (advance > 0.02) {
        if (m_waitStarted.has_value() && now - *m_waitStarted >= 1.5 && advance >= 1.0) {
            recordBurst(now);
        }
        m_waitStarted.reset();
    }
    if (readerIdle) {
        // A full cache, byte cap or user pause must not become a provider gap.
        m_waitStarted.reset();
    } else if (!m_waitStarted.has_value()) {
        m_waitStarted = now;
    }
    // Relax the capacity slowly after the source stops showing long gaps.
    // Changing capacity never seeks forward or changes playback speed.
    if (m_lastEvidence.has_value() && now - *m_lastEvidence > 60.0
        && now - m_lastReduction >= 30.0) {
        m_target = std::max(0.0, m_target - 0.5);
        m_lastReduction = now;
    }
    m_lastEnd = cacheEnd;
    m_lastSample = now;
}

void LiveBufferTuner::recordBurst(const double now)
{
    if (m_lastBurst.has_value()) {
        const auto interval = now - *m_lastBurst;
        if (interval >= 2.0 && interval <= 15.0) {
            m_intervals.push_back(interval);
            if (m_intervals.size() > 3) {
                m_intervals.pop_front();
            }
            if (m_intervals.size() >= 2) {
                const auto [smallest, largest] = std::minmax_element(m_intervals.begin(), m_intervals.end());
                // One slow request or outage is insufficient evidence of batching.
                if (*largest <= *smallest * 1.3) {
                    m_interval = *largest;
                    // Retain a whole delivery cycle plus room for jitter, so
                    // resuming on a fresh burst does not exhaust it at the next one.
                    const auto margin = std::max({ 1.5, m_interval * 0.2, (*largest - *smallest) * 2.0 });
                    const auto desired = std::min(20.0, std::ceil((m_interval + margin) * 2.0) / 2.0);
                    if (desired >= m_target) {
                        m_target = desired;
                        m_lastReduction = now;
                    } else if (now - m_lastReduction >= 30.0) {
                        m_target = std::max(desired, m_target - 0.5);
                        m_lastReduction = now;
                    }
                }
            }
        } else {
            m_intervals.clear();
        }
    }
    m_lastBurst = now;
    m_lastEvidence = now;
}

double LiveBufferTuner::targetSeconds(const double configuredSeconds) const
{
    return std::max(configuredSeconds, m_target);
}

double LiveBufferTuner::detectedIntervalSeconds() const
{
    return m_interval;
}

} // namespace OKILTV::Player
