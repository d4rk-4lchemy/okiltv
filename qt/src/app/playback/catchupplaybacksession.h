#pragma once
#include "../../core/models.h"
#include "catchupstandbytransition.h"
#include <optional>
#include <QTimer>
#include <utility>
namespace OKILTV::App::Playback {
// Archive timeline and confirmed progress. Wall time is explicit; EPG updates
// change metadata only and never produce transport commands.
class CatchupPlaybackSession final {
public:
    CatchupStandbyTransition &standby() { return m_standby; }
    const CatchupStandbyTransition &standby() const { return m_standby; }
    enum class SeekPhase { Idle, WaitingStop, Reloading };
    CatchupPlaybackSession();
    bool beginReload(double target, const QString &url, double base);
    void finishReload();
    void cancelReload();
    void clearQueuedSeek() { m_queuedSeek.reset(); }
    bool reloadInFlight() const { return m_seekPhase != SeekPhase::Idle; }
    const QString &reloadUrl() const { return m_reloadUrl; }
    double reloadBase() const { return m_reloadBase; }
    const std::optional<double> &queuedSeek() const { return m_queuedSeek; }
    void setReloadUrl(const QString &url) { m_reloadUrl = url; }
    void setReloadBase(double base) { m_reloadBase = base; }
    QTimer &reloadAckTimer() { return m_reloadAckTimer; }
    void reloadStarted() { m_seekPhase = SeekPhase::Reloading; }
    void resetContinuation();
    bool allowContinuation(double watched);
    void waitForPublication(qint64 nowMs) { m_publicationSince = nowMs; }
    void clearPublicationWait() { m_publicationSince.reset(); }
    bool publicationWaiting() const { return m_publicationSince.has_value(); }
    bool publicationDue(qint64 nowMs) const { return m_publicationSince && nowMs - *m_publicationSince >= 5000; }
    QVariantMap catchupCurrentProgram(bool active, Core::DateTimeFormatOptions format) const;
    bool catchupTimelineActive(bool active) const;
    qint64 catchupTimelineStartEpochMs() const;
    qint64 catchupTimelineEndEpochMs(bool active, const QDateTime &nowUtc) const;
    double catchupTimelineDurationSeconds(bool active, const QDateTime &nowUtc) const;
    bool canRegenerateCatchupUrl(const std::optional<Core::Channel> &channel) const;
    QString regeneratedCatchupUrl(double targetSeconds, double *streamBaseOffsetSeconds, const std::optional<Core::Channel> &channel, const QDateTime &nowUtc) const;
    bool syncTimeline(bool active, const QDateTime &nowUtc);
    bool updateProgramme(bool active, const std::optional<Core::EpgEntry> &program);
    std::optional<QDateTime> observeProgress(double streamSeconds, const QDateTime &nowUtc);
    const QString &programLabel() const { return m_catchupProgramLabel; }
    void setProgramLabel(QString value) { m_catchupProgramLabel = std::move(value); }
    const bool &endless() const { return m_catchupEndless; }
    void setEndless(bool value) { m_catchupEndless = value; }
    const bool &progressTransportReady() const { return m_catchupProgressTransportReady; }
    void setProgressTransportReady(bool value) { m_catchupProgressTransportReady = value; }
    const std::optional<double> &progressSeekTargetSeconds() const { return m_catchupProgressSeekTargetSeconds; }
    void setProgressSeekTargetSeconds(std::optional<double> value) { m_catchupProgressSeekTargetSeconds = value; }
    const std::optional<Core::EpgEntry> &displayProgram() const { return m_catchupDisplayProgram; }
    void setDisplayProgram(std::optional<Core::EpgEntry> value) { m_catchupDisplayProgram = std::move(value); }
    const std::optional<Core::EpgEntry> &validatedProgram() const { return m_catchupValidatedProgram; }
    void setValidatedProgram(std::optional<Core::EpgEntry> value) { m_catchupValidatedProgram = std::move(value); }
    const QString &livePlaybackUrlBeforeCatchup() const { return m_livePlaybackUrlBeforeCatchup; }
    void setLivePlaybackUrlBeforeCatchup(QString value) { m_livePlaybackUrlBeforeCatchup = std::move(value); }
    const QString &canonicalPlaybackUrl() const { return m_catchupCanonicalPlaybackUrl; }
    void setCanonicalPlaybackUrl(QString value) { m_catchupCanonicalPlaybackUrl = std::move(value); }
    const QDateTime &programStartUtc() const { return m_catchupProgramStartUtc; }
    void setProgramStartUtc(QDateTime value) { m_catchupProgramStartUtc = std::move(value); }
    const QDateTime &programStopUtc() const { return m_catchupProgramStopUtc; }
    void setProgramStopUtc(QDateTime value) { m_catchupProgramStopUtc = std::move(value); }
    const double &streamBaseOffsetSeconds() const { return m_catchupStreamBaseOffsetSeconds; }
    void setStreamBaseOffsetSeconds(double value) { m_catchupStreamBaseOffsetSeconds = value; }
    const int &safetySeconds() const { return m_catchupSafetySeconds; }
    void setSafetySeconds(int value) { m_catchupSafetySeconds = value; }
    const double &desiredDelaySeconds() const { return m_catchupDesiredDelaySeconds; }
    void setDesiredDelaySeconds(double value) { m_catchupDesiredDelaySeconds = value; }
    const double &transportEndTimelineSeconds() const { return m_catchupTransportEndTimelineSeconds; }
    void setTransportEndTimelineSeconds(double value) { m_catchupTransportEndTimelineSeconds = value; }
    const qint64 &timelineStartEpochMs() const { return m_catchupTimelineStartEpochMs; }
    void setTimelineStartEpochMs(qint64 value) { m_catchupTimelineStartEpochMs = value; }
    const qint64 &timelineAvailableEdgeEpochMs() const { return m_catchupTimelineAvailableEdgeEpochMs; }
    void setTimelineAvailableEdgeEpochMs(qint64 value) { m_catchupTimelineAvailableEdgeEpochMs = value; }
    const double &timelineAvailableSeconds() const { return m_catchupTimelineAvailableSeconds; }
    void setTimelineAvailableSeconds(double value) { m_catchupTimelineAvailableSeconds = value; }
    const double &timelinePositionSeconds() const { return m_catchupTimelinePositionSeconds; }
    void setTimelinePositionSeconds(double value) { m_catchupTimelinePositionSeconds = value; }
    const bool &timelineAtLiveEdge() const { return m_catchupTimelineAtLiveEdge; }
    void setTimelineAtLiveEdge(bool value) { m_catchupTimelineAtLiveEdge = value; }
    bool shouldReloadForSeek(double target, double position, const std::optional<QPair<double,double>> &range,
                             const std::optional<Core::Channel> &channel) const;
    void resetRollbackGuard(bool initialLoadContext, qint64 nowMs);
    void clearRollbackGuard();
    std::optional<double> correctRollback(double currentStreamSeconds, bool active, bool hasBackend,
        const std::optional<QPair<double, double>> &seekableRange, qint64 nowMs);
    bool rollbackDeferred() const { return m_catchupRollbackDeferredPending; }
    void seekStarted(qint64 nowMs) { m_seekStartedMs = nowMs; }
    void clearSeekSettling() { m_seekStartedMs.reset(); }
    const std::optional<qint64> &seekStartedMs() const { return m_seekStartedMs; }
    qint64 seekElapsed(qint64 nowMs) const { return m_seekStartedMs ? nowMs - *m_seekStartedMs : -1; }
    const bool &programBoundaryReached() const { return m_catchupProgramBoundaryReached; }
    void setProgramBoundaryReached(bool value) { m_catchupProgramBoundaryReached = value; }
    const bool &activeEofObserved() const { return m_catchupActiveEofObserved; }
    void setActiveEofObserved(bool value) { m_catchupActiveEofObserved = value; }
    const bool &continuousFallback() const { return m_catchupContinuousFallback; }
    void setContinuousFallback(bool value) { m_catchupContinuousFallback = value; }
    const std::optional<double> &continuousRecoveryTarget() const { return m_catchupContinuousRecoveryTarget; }
    void setContinuousRecoveryTarget(std::optional<double> value) { m_catchupContinuousRecoveryTarget = value; }
    const bool &periodReload() const { return m_catchupPeriodReload; }
    void setPeriodReload(bool value) { m_catchupPeriodReload = value; }
    const std::optional<double> &pendingStreamRelativeSeekSeconds() const { return m_catchupPendingStreamRelativeSeekSeconds; }
    void setPendingStreamRelativeSeekSeconds(std::optional<double> value) { m_catchupPendingStreamRelativeSeekSeconds = value; }
    const std::optional<double> &reconnectResumeStreamRelativeSeconds() const { return m_catchupReconnectResumeStreamRelativeSeconds; }
    void setReconnectResumeStreamRelativeSeconds(std::optional<double> value) { m_catchupReconnectResumeStreamRelativeSeconds = value; }
    const std::optional<double> &pendingInitialSeekSeconds() const { return m_catchupPendingInitialSeekSeconds; }
    void setPendingInitialSeekSeconds(std::optional<double> value) { m_catchupPendingInitialSeekSeconds = value; }
    enum class AlignmentPhase { Idle, Filling, Seeking };
    struct AlignmentDecision {
        enum class Action { Inactive, Wait, Complete, Seek, NextPeriod, Restore };
        Action action { Action::Inactive };
        double target { 0.0 };
    };
    bool alignmentActive() const { return m_alignmentPhase != AlignmentPhase::Idle; }
    bool alignmentSeekIssued() const { return m_alignmentPhase == AlignmentPhase::Seeking; }
    void setAlignmentActive(bool active) { m_alignmentPhase = active ? AlignmentPhase::Filling : AlignmentPhase::Idle; }
    void resetAlignmentSeek() { if (alignmentSeekIssued()) m_alignmentPhase = AlignmentPhase::Filling; }
    AlignmentDecision alignRecovery(double position, const std::optional<QPair<double,double>> &range,
        bool failed, bool eof, std::optional<double> nextBase, qint64 nowMs);
    void resetRecovery();
    void resetRolling(bool resetLast);
    void resetNearZero() { m_nearZeroTicks = 0; }
    void recoveryStarted(qint64 nowMs) { m_recoverySince = nowMs; }
    bool rollingBackoff(qint64 nowMs) const { return m_lastRolling && nowMs - *m_lastRolling < 900; }
    bool rollingExhausted(qint64 nowMs);
    void recordRollingAttempt(qint64 nowMs);
    int rollingAttempts() const { return m_rollingAttempts; }
    bool nearZeroRecoveryDue(std::optional<double> cache, std::optional<double> speed, bool stalled, qint64 nowMs);
private:
    CatchupStandbyTransition m_standby;
    AlignmentPhase m_alignmentPhase { AlignmentPhase::Idle };
    std::optional<qint64> m_recoverySince;
    std::optional<qint64> m_rollingSince;
    std::optional<qint64> m_lastRolling;
    int m_rollingAttempts { 0 };
    int m_nearZeroTicks { 0 };
    bool m_catchupProgramBoundaryReached { false };
    bool m_catchupActiveEofObserved { false };
    bool m_catchupContinuousFallback { false };
    std::optional<double> m_catchupContinuousRecoveryTarget;
    bool m_catchupPeriodReload { false };
    std::optional<double> m_catchupPendingStreamRelativeSeekSeconds;
    std::optional<double> m_catchupReconnectResumeStreamRelativeSeconds;
    std::optional<double> m_catchupPendingInitialSeekSeconds;
    std::optional<qint64> m_guardStartedMs;
    std::optional<qint64> m_deferredStartedMs;
    std::optional<qint64> m_seekStartedMs;
    double m_catchupLastObservedStreamSeconds { -1.0 };
    bool m_catchupRollbackGuardConsumed { false };
    bool m_catchupRollbackInitialLoadContext { false };
    bool m_catchupRollbackDeferredPending { false };
    double m_catchupRollbackDeferredTargetSeconds { -1.0 };
    SeekPhase m_seekPhase { SeekPhase::Idle };
    QString m_reloadUrl;
    double m_reloadBase { 0.0 };
    std::optional<double> m_queuedSeek;
    QTimer m_reloadAckTimer;
    std::optional<qint64> m_publicationSince;
    int m_continuationAttempts { 0 };
    double m_continuationPosition { -1.0 };
    QString m_catchupProgramLabel;
    bool m_catchupEndless { false };
    bool m_catchupProgressTransportReady { false };
    std::optional<double> m_catchupProgressSeekTargetSeconds;
    std::optional<Core::EpgEntry> m_catchupDisplayProgram;
    std::optional<Core::EpgEntry> m_catchupValidatedProgram;
    QString m_livePlaybackUrlBeforeCatchup;
    QString m_catchupCanonicalPlaybackUrl;
    QDateTime m_catchupProgramStartUtc;
    QDateTime m_catchupProgramStopUtc;
    double m_catchupStreamBaseOffsetSeconds { 0.0 };
    int m_catchupSafetySeconds { 180 };
    double m_catchupDesiredDelaySeconds { 0.0 };
    double m_catchupTransportEndTimelineSeconds { 0.0 };
    qint64 m_catchupTimelineStartEpochMs { 0 };
    qint64 m_catchupTimelineAvailableEdgeEpochMs { 0 };
    double m_catchupTimelineAvailableSeconds { 0.0 };
    double m_catchupTimelinePositionSeconds { 0.0 };
    bool m_catchupTimelineAtLiveEdge { true };
};
} // namespace OKILTV::App::Playback
