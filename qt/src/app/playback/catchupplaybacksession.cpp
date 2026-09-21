#include "catchupplaybacksession.h"
#include "../../core/catchupurlresolver.h"
#include "../../core/debuglogger.h"
#include <QRegularExpression>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
namespace OKILTV::App::Playback {
namespace {
constexpr double kPlaybackPositionEpsilon = 0.05;
constexpr int kCatchupSeekSettleMs = 3000;
constexpr double kCatchupCacheSeekSafetyMarginSeconds = 1.0;
constexpr double kCatchupUnexpectedRollbackThresholdSeconds = 15.0;
constexpr qint64 kCatchupRollbackGuardWarmupMs = 90000;
constexpr qint64 kCatchupRollbackDeferredCorrectionTimeoutMs = 3500;
std::optional<QRegularExpressionMatch> matchXtreamTimeshiftUrl(const QString &url)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(^(.*?/timeshift/[^/?#]+/[^/?#]+/)(\d+)/(\d{4}-\d{2}-\d{2}:\d{2}-\d{2})/([^/?#]+)([?#].*)?$)"),
        QRegularExpression::CaseInsensitiveOption);
    auto match = pattern.match(url.trimmed());
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    return match;
}
QString shiftedXtreamTimestamp(const QString &timestamp, const qint64 offsetSeconds)
{
    auto parsed = QDateTime::fromString(timestamp, QStringLiteral("yyyy-MM-dd:HH-mm"));
    if (!parsed.isValid()) {
        return {};
    }
    parsed.setTimeZone(QTimeZone::UTC);
    return parsed.addSecs(offsetSeconds).toString(QStringLiteral("yyyy-MM-dd:HH-mm"));
}
}
QVariantMap CatchupPlaybackSession::catchupCurrentProgram(bool active, Core::DateTimeFormatOptions format) const
{
    const auto program = m_catchupDisplayProgram;
    if (!active || !program.has_value()) {
        return {};
    }
    auto result = Core::toVariantMap(program.value(), format);
    const auto durationMs = program->start.msecsTo(program->stop);
    const auto watchedOffsetMs = static_cast<double>(program->start.msecsTo(m_catchupProgramStartUtc))
        + m_catchupTimelinePositionSeconds * 1000.0;
    result.insert(QStringLiteral("progressPercent"), durationMs > 0
        ? std::clamp(watchedOffsetMs / static_cast<double>(durationMs) * 100.0, 0.0, 100.0) : 0.0);
    return result;
}

bool CatchupPlaybackSession::catchupTimelineActive(bool active) const
{
    return active && m_catchupProgramStartUtc.isValid() && m_catchupProgramStopUtc.isValid()
        && m_catchupProgramStopUtc > m_catchupProgramStartUtc;
}

qint64 CatchupPlaybackSession::catchupTimelineStartEpochMs() const
{
    if (m_catchupEndless && m_catchupDisplayProgram.has_value()) {
        return std::max(m_catchupTimelineStartEpochMs, m_catchupDisplayProgram->start.toMSecsSinceEpoch());
    }
    return m_catchupTimelineStartEpochMs;
}

qint64 CatchupPlaybackSession::catchupTimelineEndEpochMs(bool active, const QDateTime &nowUtc) const
{
    if (!active || !m_catchupProgramStopUtc.isValid()) {
        return 0;
    }
    const auto stop = m_catchupEndless
        ? (m_catchupDisplayProgram.has_value() ? m_catchupDisplayProgram->stop : nowUtc)
        : m_catchupProgramStopUtc;
    return stop.toMSecsSinceEpoch();
}

double CatchupPlaybackSession::catchupTimelineDurationSeconds(bool active, const QDateTime &nowUtc) const
{
    return active
        ? std::max(0.0, static_cast<double>(catchupTimelineEndEpochMs(active, nowUtc) - catchupTimelineStartEpochMs()) / 1000.0)
        : 0.0;
}

bool CatchupPlaybackSession::canRegenerateCatchupUrl(const std::optional<Core::Channel> &channel) const
{
    return matchXtreamTimeshiftUrl(m_catchupCanonicalPlaybackUrl).has_value()
        || (m_catchupEndless && channel && channel->source == Core::ChannelSource::M3U
            && !channel->catchupSourceTemplate.trimmed().isEmpty()
            && (channel->catchupMode == QStringLiteral("default")
                || channel->catchupMode == QStringLiteral("append")));
}

QString CatchupPlaybackSession::regeneratedCatchupUrl(double targetSeconds, double *streamBaseOffsetSeconds, const std::optional<Core::Channel> &channel, const QDateTime &nowUtc) const
{
    if (m_catchupEndless && channel && channel->source == Core::ChannelSource::M3U
        && !channel->catchupSourceTemplate.trimmed().isEmpty()) {
        const auto offset = static_cast<qint64>(std::floor(std::clamp(targetSeconds, 0.0, m_catchupTimelineAvailableSeconds)));
        const auto edge = Core::CatchupUrlResolver::availableEdge(nowUtc, m_catchupSafetySeconds, nowUtc);
        const auto window = Core::CatchupUrlResolver {}.resolveWindow(
            *channel, m_catchupProgramStartUtc.addSecs(offset), edge);
        if (!window) {
            return {};
        }
        if (streamBaseOffsetSeconds) {
            *streamBaseOffsetSeconds = static_cast<double>(offset);
        }
        return window->url;
    }
    const auto match = matchXtreamTimeshiftUrl(m_catchupCanonicalPlaybackUrl);
    if (!match.has_value()) {
        return {};
    }
    if (!m_catchupProgramStartUtc.isValid() || !m_catchupProgramStopUtc.isValid()
        || m_catchupProgramStopUtc <= m_catchupProgramStartUtc) {
        return {};
    }

    const auto boundedTarget = std::max(0.0, std::min(m_catchupTimelineAvailableSeconds, targetSeconds));
    const auto minuteOffsetSeconds = static_cast<qint64>(std::floor(boundedTarget / 60.0)) * 60LL;
    const auto shiftedTimestamp = shiftedXtreamTimestamp(match->captured(3), minuteOffsetSeconds);
    if (shiftedTimestamp.isEmpty()) {
        return {};
    }

    if (streamBaseOffsetSeconds != nullptr) {
        *streamBaseOffsetSeconds = static_cast<double>(minuteOffsetSeconds);
    }

    const auto edge = Core::CatchupUrlResolver::availableEdge(
        m_catchupEndless ? nowUtc : m_catchupProgramStopUtc, m_catchupSafetySeconds, nowUtc);
    auto available = m_catchupProgramStartUtc.secsTo(edge);
    if (!m_catchupEndless && edge >= m_catchupProgramStopUtc) {
        const auto remaining = std::max<qint64>(1, available - minuteOffsetSeconds);
        available = minuteOffsetSeconds + (((remaining + 59) / 60) + 1) * 60;
    }
    return Core::CatchupUrlResolver::xtreamWindowUrl(m_catchupCanonicalPlaybackUrl, minuteOffsetSeconds, available);
}

bool CatchupPlaybackSession::syncTimeline(bool active, const QDateTime &nowUtc)
{
    if (!active || !m_catchupProgramStartUtc.isValid() || !m_catchupProgramStopUtc.isValid()
        || m_catchupProgramStopUtc <= m_catchupProgramStartUtc) {
        return false;
    }
    m_catchupTimelineStartEpochMs = m_catchupProgramStartUtc.toMSecsSinceEpoch();
    const auto programStop = m_catchupEndless ? nowUtc : m_catchupProgramStopUtc;
    const auto availableEdge = Core::CatchupUrlResolver::availableEdge(programStop, m_catchupSafetySeconds, nowUtc);
    m_catchupTimelineAvailableEdgeEpochMs = availableEdge.toMSecsSinceEpoch();
    const auto availableSeconds = std::max<qint64>(0, m_catchupProgramStartUtc.secsTo(availableEdge));
    m_catchupTimelineAvailableSeconds = static_cast<double>(availableSeconds);
    const auto visibleSpan = std::max(0.0, static_cast<double>(m_catchupProgramStartUtc.msecsTo(std::min(nowUtc, programStop))) / 1000.0);
    m_catchupTimelinePositionSeconds = std::clamp(m_catchupTimelinePositionSeconds, 0.0, visibleSpan);
    m_catchupTimelineAtLiveEdge = m_catchupTimelineAvailableSeconds <= 0.0
        || (m_catchupTimelineAvailableSeconds - m_catchupTimelinePositionSeconds) <= 0.75;
    return true;
}

bool CatchupPlaybackSession::updateProgramme(bool active, const std::optional<Core::EpgEntry> &program)
{
    if (!active || !m_catchupEndless) {
        return false;
    }
    m_catchupDisplayProgram = program;
    const auto label = program.has_value()
        ? (program->subTitle.trimmed().isEmpty() ? program->title : program->title + QStringLiteral(" — ") + program->subTitle)
        : QStringLiteral("Catch-up");
    if (label != m_catchupProgramLabel) {
        m_catchupProgramLabel = label;
        return true;
    }
    return false;
}

std::optional<QDateTime> CatchupPlaybackSession::observeProgress(double streamSeconds, const QDateTime &nowUtc)
{
    if (!m_catchupProgressTransportReady || !std::isfinite(streamSeconds) || streamSeconds < 0.0) return std::nullopt;
    const auto timelineSeconds = m_catchupStreamBaseOffsetSeconds + streamSeconds;
    const auto seekTarget = m_catchupProgressSeekTargetSeconds;
    // Fast seeks land at provider keyframes. Until the clock lands near the
    // target, preserve the previous bookmark, including failed M3U seeks.
    if (seekTarget.has_value() && std::abs(timelineSeconds - seekTarget.value()) > 10.0) {
        return std::nullopt;
    }
    m_catchupProgressSeekTargetSeconds.reset();
    const auto offsetMs = (m_catchupStreamBaseOffsetSeconds + streamSeconds) * 1000.0;
    const auto maxOffsetMs = m_catchupProgramStartUtc.msecsTo(
        m_catchupEndless ? nowUtc : m_catchupProgramStopUtc);
    if (!m_catchupProgramStartUtc.isValid() || !std::isfinite(offsetMs)
        || offsetMs < 0.0 || offsetMs > static_cast<double>(maxOffsetMs) + 1000.0) {
        return std::nullopt;
    }
    return m_catchupProgramStartUtc.addMSecs(static_cast<qint64>(std::min(offsetMs, static_cast<double>(maxOffsetMs))));
}
CatchupPlaybackSession::CatchupPlaybackSession()
{
    m_reloadAckTimer.setSingleShot(true);
    m_reloadAckTimer.setInterval(500);
}
bool CatchupPlaybackSession::beginReload(double target, const QString &url, double base)
{
    if (reloadInFlight()) {
        m_queuedSeek = target;
        return false;
    }
    m_seekPhase = SeekPhase::WaitingStop;
    m_reloadUrl = url;
    m_reloadBase = base;
    m_queuedSeek.reset();
    m_reloadAckTimer.start();
    return true;
}
void CatchupPlaybackSession::finishReload()
{
    m_reloadAckTimer.stop();
    m_seekPhase = SeekPhase::Idle;
    m_reloadUrl.clear();
    m_reloadBase = 0.0;
}
void CatchupPlaybackSession::cancelReload()
{
    finishReload();
    m_queuedSeek.reset();
}
void CatchupPlaybackSession::resetContinuation()
{
    m_publicationSince.reset();
    m_continuationAttempts = 0;
    m_continuationPosition = -1.0;
}
bool CatchupPlaybackSession::allowContinuation(double watched)
{
    if (watched > m_continuationPosition + 1.0) {
        m_continuationAttempts = 0;
        m_continuationPosition = watched;
    }
    return ++m_continuationAttempts <= 3;
}
void CatchupPlaybackSession::resetRollbackGuard(bool initialLoadContext, qint64 nowMs)
{
    m_guardStartedMs = nowMs;
    m_catchupLastObservedStreamSeconds = -1.0;
    m_catchupRollbackGuardConsumed = false;
    m_catchupRollbackInitialLoadContext = initialLoadContext;
    m_catchupRollbackDeferredPending = false;
    m_catchupRollbackDeferredTargetSeconds = -1.0;
    m_deferredStartedMs.reset();
}

std::optional<double> CatchupPlaybackSession::correctRollback(double currentStreamSeconds, bool active, bool hasBackend,
    const std::optional<QPair<double, double>> &seekableRange, qint64 nowMs)
{
    std::optional<double> seek;
    if (!active || currentStreamSeconds < 0.0) {
        return seek;
    }
    if (m_catchupRollbackDeferredPending) {
        if (!hasBackend) {
            m_catchupRollbackDeferredPending = false;
            m_catchupRollbackDeferredTargetSeconds = -1.0;
            m_deferredStartedMs.reset();
            return seek;
        }
        if (!m_deferredStartedMs.has_value()
            || (nowMs - m_deferredStartedMs.value_or(nowMs)) > kCatchupRollbackDeferredCorrectionTimeoutMs) {
            m_catchupRollbackDeferredPending = false;
            m_catchupRollbackDeferredTargetSeconds = -1.0;
            m_deferredStartedMs.reset();
            m_catchupLastObservedStreamSeconds = currentStreamSeconds;
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up rollback guard deferred correction expired after %1ms; continuing playback.")
                    .arg(kCatchupRollbackDeferredCorrectionTimeoutMs));
            return seek;
        }
        if (seekableRange.has_value()) {
            const auto target = std::max(0.0, m_catchupRollbackDeferredTargetSeconds);
            const auto rangeStart = std::max(0.0, seekableRange->first);
            const auto rangeEnd = std::max(rangeStart, seekableRange->second);
            if (target + kPlaybackPositionEpsilon >= rangeStart
                && target <= rangeEnd - kCatchupCacheSeekSafetyMarginSeconds) {
                m_seekStartedMs = nowMs;
                seek = target;
                m_catchupRollbackDeferredPending = false;
                m_catchupRollbackDeferredTargetSeconds = -1.0;
                m_deferredStartedMs.reset();
                Core::DebugLogger::instance().log(
                    QStringLiteral("player"),
                    QStringLiteral(
                        "Catch-up rollback guard deferred correction applied: target=%1s range=%2..%3s.")
                        .arg(target, 0, 'f', 3)
                        .arg(rangeStart, 0, 'f', 3)
                        .arg(rangeEnd, 0, 'f', 3));
                return seek;
            }
        }
    }
    if (m_catchupRollbackGuardConsumed || !m_guardStartedMs.has_value()
        || (nowMs - m_guardStartedMs.value_or(nowMs)) > kCatchupRollbackGuardWarmupMs) {
        m_catchupLastObservedStreamSeconds = currentStreamSeconds;
        return seek;
    }
    if (m_catchupLastObservedStreamSeconds < 0.0) {
        m_catchupLastObservedStreamSeconds = currentStreamSeconds;
        return seek;
    }
    if (m_seekStartedMs.has_value() && (nowMs - m_seekStartedMs.value_or(nowMs)) < kCatchupSeekSettleMs) {
        m_catchupLastObservedStreamSeconds = currentStreamSeconds;
        return seek;
    }
    const auto rollbackSeconds = m_catchupLastObservedStreamSeconds - currentStreamSeconds;
    if (rollbackSeconds < kCatchupUnexpectedRollbackThresholdSeconds) {
        m_catchupLastObservedStreamSeconds = std::max(m_catchupLastObservedStreamSeconds, currentStreamSeconds);
        return seek;
    }
    if (!hasBackend) {
        m_catchupLastObservedStreamSeconds = currentStreamSeconds;
        return seek;
    }
    if (m_catchupRollbackInitialLoadContext) {
        const auto target = std::max(0.0, m_catchupLastObservedStreamSeconds);
        const auto seekableReady = seekableRange.has_value()
            && (target + kPlaybackPositionEpsilon >= std::max(0.0, seekableRange->first))
            && (target <= std::max(std::max(0.0, seekableRange->first), seekableRange->second) - kCatchupCacheSeekSafetyMarginSeconds);
        if (!seekableReady) {
            m_catchupRollbackDeferredPending = true;
            m_catchupRollbackDeferredTargetSeconds = target;
            m_deferredStartedMs = nowMs;
            m_catchupRollbackGuardConsumed = true;
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up rollback guard deferred correction armed: current=%1s target=%2s rollback=%3s seekable=%4.")
                    .arg(currentStreamSeconds, 0, 'f', 3)
                    .arg(target, 0, 'f', 3)
                    .arg(rollbackSeconds, 0, 'f', 3)
                    .arg(seekableRange.has_value() ? QStringLiteral("yes") : QStringLiteral("no")));
            return seek;
        }
    }
    m_seekStartedMs = nowMs;
    seek = m_catchupLastObservedStreamSeconds;
    m_catchupRollbackGuardConsumed = true;
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up rollback guard corrected backward jump: current=%1s previous=%2s rollback=%3s.")
            .arg(currentStreamSeconds, 0, 'f', 3)
            .arg(m_catchupLastObservedStreamSeconds, 0, 'f', 3)
            .arg(rollbackSeconds, 0, 'f', 3));
    return seek;
}

void CatchupPlaybackSession::clearRollbackGuard()
{
    m_guardStartedMs.reset();
    m_deferredStartedMs.reset();
    m_catchupLastObservedStreamSeconds = -1.0;
    m_catchupRollbackGuardConsumed = false;
    m_catchupRollbackInitialLoadContext = false;
    m_catchupRollbackDeferredPending = false;
    m_catchupRollbackDeferredTargetSeconds = -1.0;
}
CatchupPlaybackSession::AlignmentDecision CatchupPlaybackSession::alignRecovery(double position,
    const std::optional<QPair<double,double>> &range, bool failed, bool eof, std::optional<double> nextBase, qint64 nowMs)
{
    using Action = AlignmentDecision::Action;
    if (!alignmentActive() || !m_catchupReconnectResumeStreamRelativeSeconds) return {};
    const auto target = *m_catchupReconnectResumeStreamRelativeSeconds;
    if (alignmentSeekIssued() && std::isfinite(position) && position >= target - 0.1) {
        m_alignmentPhase = AlignmentPhase::Idle;
        m_catchupReconnectResumeStreamRelativeSeconds.reset();
        seekStarted(nowMs);
        return {Action::Complete};
    }
    if (!alignmentSeekIssued() && range && range->second >= target + 1.0) {
        m_alignmentPhase = AlignmentPhase::Seeking;
        return {Action::Seek, target};
    }
    if (!alignmentSeekIssued() && range && range->second > position + 30.0)
        return {Action::Seek, range->second - 1.0};
    if (!alignmentSeekIssued() && (failed || eof)) {
        if (nextBase && *nextBase >= m_catchupStreamBaseOffsetSeconds + target && eof) return {Action::NextPeriod};
        m_alignmentPhase = AlignmentPhase::Idle;
        m_catchupReconnectResumeStreamRelativeSeconds.reset();
        return {Action::Restore};
    }
    return {Action::Wait};
}
void CatchupPlaybackSession::resetRecovery()
{
    resetNearZero();
    m_recoverySince.reset();
    resetRolling(true);
}
void CatchupPlaybackSession::resetRolling(bool resetLast)
{
    m_rollingAttempts = 0;
    m_rollingSince.reset();
    if (resetLast) m_lastRolling.reset();
}
bool CatchupPlaybackSession::rollingExhausted(qint64 nowMs)
{
    if (!m_rollingSince || nowMs - *m_rollingSince > 10000) {
        m_rollingAttempts = 0;
        m_rollingSince = nowMs;
    }
    return m_rollingAttempts >= 3;
}
void CatchupPlaybackSession::recordRollingAttempt(qint64 nowMs)
{
    ++m_rollingAttempts;
    m_rollingSince = nowMs;
    m_lastRolling = nowMs;
}
bool CatchupPlaybackSession::nearZeroRecoveryDue(std::optional<double> cache, std::optional<double> speed, bool stalled, qint64 nowMs)
{
    if (m_catchupTimelineAvailableSeconds <= 0.0) return false;
    if (std::max(0.0, m_catchupTimelineAvailableSeconds - m_catchupTimelinePositionSeconds) <= 90.0) {
        resetNearZero();
        return false;
    }
    if (!cache || !std::isfinite(*cache) || *cache < 0.0) return false;
    const auto hasSpeed = speed && std::isfinite(*speed) && *speed >= 0.0;
    const auto lowRefill = hasSpeed ? *speed <= 1024.0 : stalled;
    if (*cache <= 1.0 && lowRefill) ++m_nearZeroTicks;
    else resetNearZero();
    return (!m_recoverySince || nowMs - *m_recoverySince >= 5000) && m_nearZeroTicks >= 1;
}
// Preserve the established positional contract; parameter names identify their roles.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool CatchupPlaybackSession::shouldReloadForSeek(double target, double position,
    const std::optional<QPair<double,double>> &range, const std::optional<Core::Channel> &channel) const
{
    if (m_catchupCanonicalPlaybackUrl.trimmed().isEmpty() || !canRegenerateCatchupUrl(channel)) return false;
    if (position < 0.0 || !range) return true;
    const auto currentTimeline = m_catchupStreamBaseOffsetSeconds + position;
    const auto cachedStart = m_catchupStreamBaseOffsetSeconds + range->first;
    const auto cachedEnd = m_catchupStreamBaseOffsetSeconds + range->second - 1.0;
    const auto effectiveStart = std::max(cachedStart, std::max(0.0, currentTimeline - 60.0));
    return target + 0.05 < effectiveStart || target > cachedEnd;
}
} // namespace OKILTV::App::Playback
