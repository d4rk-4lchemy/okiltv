#pragma once

#include <deque>
#include <optional>

namespace OKILTV::Player {

// Learns provider delivery cadence only when the demuxer actually wants data.
// All timestamps are monotonic seconds, supplied by the caller for deterministic tests.
class LiveBufferTuner
{
public:
    void reset();
    void interruptObservation();
    void observe(double now, double cacheEnd, bool readerIdle);
    double targetSeconds(double configuredSeconds) const;
    double detectedIntervalSeconds() const;

private:
    void recordBurst(double now);
    std::optional<double> m_lastSample;
    std::optional<double> m_lastEnd;
    std::optional<double> m_waitStarted;
    std::optional<double> m_lastBurst;
    std::optional<double> m_lastEvidence;
    std::deque<double> m_intervals;
    double m_interval { 0.0 };
    double m_target { 0.0 };
    double m_lastReduction { 0.0 };
};

} // namespace OKILTV::Player
