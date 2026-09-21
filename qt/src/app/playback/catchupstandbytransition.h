#pragma once
#include <QString>
#include <QTimer>
#include <QPair>
#include <optional>
namespace OKILTV::App::Playback {
class CatchupStandbyTransition final {
public:
    enum class Phase { Idle, Armed, Delayed, WaitingStop, Loading };
    struct CutoverSample {
        std::optional<double> activeCache;
        double remainingSeconds { 0.0 };
        bool force { false };
        bool alignToWatched { false };
        double watchedSeconds { 0.0 };
        double standbyPosition { -1.0 };
        std::optional<QPair<double, double>> standbyRange;
        bool standbyExhausted { false };
    };
    struct CutoverDecision {
        enum class Action { Wait, Seek, Reject, Commit };
        Action action { Action::Wait };
        double target { 0.0 };
    };
    CutoverDecision evaluateCutover(const CutoverSample &sample);
    CatchupStandbyTransition();
    void arm(const QString &url, double baseOffset);
    void cancel();
    void loaded(qint64 nowMs);
    void failed(qint64 nowMs);
    bool allowAttempt(qint64 nowMs);
    void resetBackoff() { m_lastAttempt.reset(); }
    void beginStop() { m_phase = Phase::WaitingStop; }
    void acknowledgeStop() { if (stopPending()) m_phase = Phase::Armed; }
    void beginDelay() { m_phase = Phase::Delayed; }
    void delayElapsed() { if (delayPending()) m_phase = Phase::Armed; }
    void markReady() { m_ready = true; }
    void markVideoReady() { m_videoReady = true; }
    void markAlignmentIssued() { m_alignmentIssued = true; }
    void deferFallback() { m_fallbackDeferred = true; }
    bool pending() const { return m_phase != Phase::Idle; }
    bool loadIssued() const { return m_phase == Phase::Loading; }
    bool stopPending() const { return m_phase == Phase::WaitingStop; }
    bool delayPending() const { return m_phase == Phase::Delayed; }
    bool ready() const { return m_ready; }
    bool videoReady() const { return m_videoReady; }
    bool alignmentIssued() const { return m_alignmentIssued; }
    bool fallbackDeferred() const { return m_fallbackDeferred; }
    bool retryPending() const { return m_retryPending; }
    int retryBudget() const { return m_retryBudget; }
    bool fastRetryWindowOpen(qint64 nowMs) const;
    qint64 fastRetryElapsed(qint64 nowMs) const { return m_retryWindow ? nowMs - *m_retryWindow : -1; }
    const QString &url() const { return m_url; }
    double baseOffset() const { return m_baseOffset; }
    void setUrl(const QString &url) { m_url = url; }
    void setBaseOffset(double offset) { m_baseOffset = offset; }
    quint64 generation() const { return m_generation; }
    bool accepts(quint64 generation) const { return pending() && generation == m_generation; }
    QTimer &fallbackTimer() { return m_fallbackTimer; }
    QTimer &stopAckTimer() { return m_stopAckTimer; }
    QTimer &videoReadyTimer() { return m_videoReadyTimer; }
    QTimer &retryTimer() { return m_retryTimer; }
    QTimer &delayTimer() { return m_delayTimer; }
private:
    void resetReadiness();
    Phase m_phase { Phase::Idle };
    bool m_ready { false };
    bool m_videoReady { false };
    bool m_alignmentIssued { false };
    bool m_fallbackDeferred { false };
    bool m_retryPending { false };
    int m_retryBudget { 0 };
    std::optional<qint64> m_lastAttempt;
    std::optional<qint64> m_retryWindow;
    std::optional<qint64> m_lastFailure;
    QString m_url;
    double m_baseOffset { 0.0 };
    quint64 m_generation { 0 };
    QTimer m_fallbackTimer;
    QTimer m_stopAckTimer;
    QTimer m_videoReadyTimer;
    QTimer m_retryTimer;
    QTimer m_delayTimer;
};
} // namespace OKILTV::App::Playback
