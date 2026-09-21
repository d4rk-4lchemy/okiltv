#include "catchupstandbytransition.h"
#include <algorithm>
#include <cmath>
namespace OKILTV::App::Playback {
CatchupStandbyTransition::CatchupStandbyTransition()
{
    for (auto *timer : {&m_fallbackTimer, &m_stopAckTimer, &m_videoReadyTimer, &m_retryTimer, &m_delayTimer})
        timer->setSingleShot(true);
    m_fallbackTimer.setInterval(2500);
    m_stopAckTimer.setInterval(500);
    m_videoReadyTimer.setInterval(400);
    m_retryTimer.setInterval(250);
    m_delayTimer.setInterval(750);
}
void CatchupStandbyTransition::resetReadiness()
{
    m_ready = false;
    m_videoReady = false;
    m_alignmentIssued = false;
}
void CatchupStandbyTransition::cancel()
{
    ++m_generation;
    for (auto *timer : {&m_fallbackTimer, &m_stopAckTimer, &m_videoReadyTimer, &m_retryTimer, &m_delayTimer}) timer->stop();
    m_phase = Phase::Idle;
    resetReadiness();
    m_fallbackDeferred = false;
    m_retryPending = false;
    m_retryBudget = 0;
    m_retryWindow.reset();
    m_lastFailure.reset();
    m_url.clear();
    m_baseOffset = 0.0;
}
void CatchupStandbyTransition::arm(const QString &url, double baseOffset)
{
    cancel();
    m_phase = Phase::Armed;
    m_url = url;
    m_baseOffset = baseOffset;
    m_retryBudget = 2;
}
bool CatchupStandbyTransition::fastRetryWindowOpen(qint64 nowMs) const
{
    return m_retryWindow && nowMs - *m_retryWindow < 1200;
}
bool CatchupStandbyTransition::allowAttempt(qint64 nowMs)
{
    if (m_lastAttempt && nowMs - *m_lastAttempt < 5000) {
        if (!m_retryPending || m_retryBudget <= 0 || !fastRetryWindowOpen(nowMs)
            || !m_lastFailure || nowMs - *m_lastFailure < 250) return false;
        --m_retryBudget;
        m_retryPending = false;
    }
    return true;
}
void CatchupStandbyTransition::loaded(qint64 nowMs)
{
    m_phase = Phase::Loading;
    resetReadiness();
    m_stopAckTimer.stop();
    m_videoReadyTimer.stop();
    m_retryTimer.stop();
    m_retryPending = false;
    m_lastAttempt = nowMs;
}
void CatchupStandbyTransition::failed(qint64 nowMs)
{
    m_phase = Phase::Armed;
    resetReadiness();
    m_videoReadyTimer.stop();
    m_stopAckTimer.stop();
    m_delayTimer.stop();
    if (!m_retryWindow) m_retryWindow = nowMs;
    m_lastFailure = nowMs;
    if (m_retryBudget > 0) {
        m_retryPending = true;
        m_retryTimer.start();
    }
}
CatchupStandbyTransition::CutoverDecision CatchupStandbyTransition::evaluateCutover(const CutoverSample &sample)
{
    using Action = CutoverDecision::Action;
    if (!pending() || !ready() || !videoReady()) return {};
    const auto hasCache = sample.activeCache && std::isfinite(*sample.activeCache) && *sample.activeCache >= 0.0;
    const auto nearEnd = !std::isfinite(sample.remainingSeconds) || sample.remainingSeconds <= 0.35;
    const auto readyToReplace = hasCache ? *sample.activeCache <= 0.35 : nearEnd;
    if (!sample.force && !readyToReplace && (hasCache || !fallbackDeferred())) return {};
    if (sample.alignToWatched) {
        const auto target = std::max(0.0, sample.watchedSeconds - m_baseOffset);
        if (sample.standbyRange && sample.standbyRange->second < target + 1.0)
            return {sample.standbyExhausted ? Action::Reject : Action::Wait};
        if (std::isfinite(sample.standbyPosition) && sample.standbyPosition + 1.0 < target) {
            if (!alignmentIssued() && sample.standbyRange && sample.standbyRange->first <= target) {
                markAlignmentIssued();
                return {Action::Seek, target};
            }
            return {};
        }
    }
    return {Action::Commit};
}
} // namespace OKILTV::App::Playback
