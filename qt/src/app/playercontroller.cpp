#include "playercontroller.h"

#include "timeshiftcontroller.h"

#include "../core/appdatapaths.h"
#include "../core/debuglogger.h"
#include "../core/processutils.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>

namespace OKILTV::App {

using namespace Core;

namespace {

bool playerTraceEnabled()
{
    static const bool enabled = qEnvironmentVariableIsSet("OKILTV_TRACE_PLAYER");
    return enabled;
}

static QString sanitizeForFilename(const QString &s)
{
    // Some letters don't decompose to ASCII via NFD (e.g. ł, ø, ß) — map them first
    static const QHash<QChar, QString> kSubstitutions = {
        {u'ł', QStringLiteral("l")},  {u'Ł', QStringLiteral("L")},
        {u'ø', QStringLiteral("o")},  {u'Ø', QStringLiteral("O")},
        {u'ð', QStringLiteral("d")},  {u'Ð', QStringLiteral("D")},
        {u'þ', QStringLiteral("th")}, {u'Þ', QStringLiteral("Th")},
        {u'ß', QStringLiteral("ss")},
        {u'æ', QStringLiteral("ae")}, {u'Æ', QStringLiteral("AE")},
        {u'œ', QStringLiteral("oe")}, {u'Œ', QStringLiteral("OE")},
    };

    QString expanded;
    expanded.reserve(s.size());
    for (const QChar ch : s) {
        const auto it = kSubstitutions.constFind(ch);
        expanded += (it != kSubstitutions.constEnd()) ? *it : QString(ch);
    }

    // NFD splits accented letters into base + combining mark (e.g. Ś → S + ́)
    const QString nfd = expanded.normalized(QString::NormalizationForm_D);

    QString out;
    out.reserve(nfd.size());
    for (const QChar ch : nfd) {
        const auto cat = ch.category();
        // Drop combining marks left over from NFD decomposition
        if (cat == QChar::Mark_NonSpacing || cat == QChar::Mark_SpacingCombining || cat == QChar::Mark_Enclosing) {
            continue;
        }
        if (ch.unicode() < 128 && (ch.isLetterOrNumber() || ch == u'-' || ch == u'_')) {
            out += ch;
        } else {
            out += u'_';
        }
    }

    // Collapse consecutive underscores and strip leading/trailing ones
    out.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    while (!out.isEmpty() && out.front() == u'_') out.remove(0, 1);
    while (!out.isEmpty() && out.back() == u'_') out.chop(1);

    return out.left(60);
}

// Always record raw bytes to .ts, then remux to .mkv — extension not used externally anymore

constexpr int kPlaybackStallTickThreshold = 2;
constexpr double kPlaybackPositionEpsilon = 0.05;
constexpr int kLoadingIndicatorDelayMs = 1500;
constexpr int kReconnectAttemptIntervalMs = 1000;
constexpr int kReconnectPostDepletionGraceMs = 10000;
constexpr int kReconnectMaxAttempts = 5;
constexpr int kReconnectStabilizationStableTickThreshold = 12;
constexpr int kReconnectStabilizationUnstableTickThreshold = 3;
constexpr int kReconnectStabilizationMinRefillTicks = 2;
constexpr int kNoRefillTickThreshold = 3;
constexpr int kVideoFreezeTickThreshold = 3;
constexpr int kDecoderStallTickThreshold = 6;
constexpr double kVideoFramePtsEpsilonSeconds = 0.03;
constexpr double kDeadStreamLowThroughputBitsPerSecond = 1024.0;
constexpr double kReconnectDepletedBufferThresholdSeconds = 0.05;
constexpr double kPreemptiveReconnectBufferRatioThreshold = 0.40;
constexpr double kNoRefillCacheSpeedThresholdBytesPerSecond = 1024.0;
constexpr double kNoRefillCacheIncreaseEpsilonSeconds = 0.05;
constexpr int kSoftReconnectWatchdogCooldownMs = 15000;
constexpr int kCatchupSeekSettleMs = 3000;
constexpr double kMinimumBufferSeconds = 0.1;
constexpr double kMaximumBufferSeconds = 60.0;
constexpr qint64 kDebugBitrateWindowMs = 5000;
constexpr double kAdaptiveSteadyStateMaxBytesSafetyMultiplier = 1.25;
constexpr double kAdaptiveSteadyStateSecondsRetuneThreshold = 0.5;
constexpr double kAdaptiveSteadyStateBytesRetuneThresholdRatio = 0.15;
constexpr int kAdaptiveSteadyStateRetuneIntervalMs = 2000;
constexpr qint64 kAdaptiveSteadyStateMinBytes = 8LL * 1024 * 1024;
constexpr auto kPlaybackSignalsConnectedProperty = "_okiltvPlaybackSignalsConnected";
constexpr double kCatchupCacheSeekSafetyMarginSeconds = 1.0;
constexpr double kCatchupCacheTargetSeconds = 120.0;
constexpr double kCatchupCacheRefillMarginSeconds = 5.0;
constexpr double kCatchupBackwardSeekWindowSeconds = 60.0;
constexpr double kCatchupDebugEffectiveCacheMaxSeconds = kCatchupCacheTargetSeconds;

double normalizedBufferTargetSeconds(const double value)
{
    if (!std::isfinite(value)) {
        return 3.0;
    }

    return std::clamp(value, kMinimumBufferSeconds, kMaximumBufferSeconds);
}

double normalizedWaitForDataStreamSeconds(const double value)
{
    if (!std::isfinite(value)) {
        return 5.0;
    }

    return std::clamp(std::round(value * 10.0) / 10.0, 0.1, 120.0);
}

QString debugTimestampLocal(const qint64 epochMs)
{
    if (epochMs <= 0) {
        return QStringLiteral("N/A");
    }
    const auto timestamp = QDateTime::fromMSecsSinceEpoch(epochMs).toLocalTime();
    return timestamp.isValid()
        ? timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
        : QStringLiteral("N/A");
}

std::optional<QRegularExpressionMatch> matchXtreamTimeshiftUrl(const QString &url)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(^(.*?/timeshift/[^/?#]+/[^/?#]+/)(\d+)/(\d{4}-\d{2}-\d{2}:\d{2}-\d{2})/([^/?#]+)([?#].*)?$)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(url.trimmed());
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

QString fallbackStreamSegment(const QString &url)
{
    auto candidate = url.trimmed();
    if (candidate.isEmpty()) {
        return QStringLiteral("N/A");
    }

    const auto queryIndex = candidate.indexOf(u'?');
    if (queryIndex >= 0) {
        candidate.truncate(queryIndex);
    }
    const auto fragmentIndex = candidate.indexOf(u'#');
    if (fragmentIndex >= 0) {
        candidate.truncate(fragmentIndex);
    }

    const auto slashIndex = candidate.lastIndexOf(u'/');
    if (slashIndex >= 0 && slashIndex + 1 < candidate.size()) {
        candidate = candidate.mid(slashIndex + 1);
    }
    return candidate.isEmpty() ? QStringLiteral("N/A") : candidate;
}

std::tm localTimeFrom(const std::time_t timestamp)
{
    std::tm timeInfo {};
#if defined(Q_OS_WIN)
    localtime_s(&timeInfo, &timestamp);
#else
    localtime_r(&timestamp, &timeInfo);
#endif
    return timeInfo;
}

std::optional<double> instantaneousBitrateBitsPerSecond(const Player::MpvPlayer *player)
{
    if (player == nullptr) {
        return std::nullopt;
    }

    double instantaneousBitsPerSecond = 0.0;
    bool hasInstantaneousBitrate = false;
    if (const auto videoBitrate = player->videoBitrateBitsPerSecond();
        videoBitrate.has_value() && std::isfinite(videoBitrate.value()) && videoBitrate.value() >= 0.0) {
        instantaneousBitsPerSecond += videoBitrate.value();
        hasInstantaneousBitrate = true;
    }
    if (const auto audioBitrate = player->audioBitrateBitsPerSecond();
        audioBitrate.has_value() && std::isfinite(audioBitrate.value()) && audioBitrate.value() >= 0.0) {
        instantaneousBitsPerSecond += audioBitrate.value();
        hasInstantaneousBitrate = true;
    }

    return hasInstantaneousBitrate ? std::optional<double>(instantaneousBitsPerSecond) : std::nullopt;
}

}

PlayerController::PlayerController(QObject *parent)
    : QObject(parent)
{
    m_loadingIndicatorDelayTimer.setInterval(kLoadingIndicatorDelayMs);
    m_loadingIndicatorDelayTimer.setSingleShot(true);
    connect(&m_loadingIndicatorDelayTimer, &QTimer::timeout, this, [this]() {
        if (!m_loadingIndicatorPending || !m_currentChannel.has_value() || m_isPlaying) {
            return;
        }

        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Playback did not start within %1 ms; showing loading spinner.")
                .arg(kLoadingIndicatorDelayMs));
        setIsLoading(true);
    });
    m_pauseStateSyncTimer.setInterval(150);
    m_pauseStateSyncTimer.setSingleShot(true);
    connect(&m_pauseStateSyncTimer, &QTimer::timeout, this, [this]() {
        if (m_pauseStateSyncRetriesRemaining <= 0 || !m_currentChannel.has_value()) {
            stopPauseStateResync();
            return;
        }

        const auto paused = playbackPlayer()->pauseState();
        if (paused.has_value()) {
            if (paused.value() && m_pauseToggleRequested) {
                m_userPausedManually = true;
                m_pauseToggleRequested = false;
            } else if (!paused.value()) {
                m_userPausedManually = false;
                m_pauseToggleRequested = false;
            }
            setIsPlaying(!paused.value());
            stopPauseStateResync();
            return;
        }

        m_pauseStateSyncRetriesRemaining -= 1;
        if (m_pauseStateSyncRetriesRemaining > 0) {
            m_pauseStateSyncTimer.start();
        }
    });
    m_startupBufferFallbackTimer.setSingleShot(true);
    connect(&m_startupBufferFallbackTimer, &QTimer::timeout, this, &PlayerController::handleStartupBufferFallbackTimeout);
    m_startupBufferProbeTimer.setInterval(100);
    connect(&m_startupBufferProbeTimer, &QTimer::timeout, this, &PlayerController::evaluateStartupBufferAndResumeIfReady);
    m_reconnectAttemptTimer.setInterval(reconnectAttemptIntervalMs());
    connect(&m_reconnectAttemptTimer, &QTimer::timeout, this, &PlayerController::handleReconnectAttemptTick);
    m_hwdecFallbackTimer.setSingleShot(true);
    m_hwdecFallbackTimer.setInterval(5000);
    connect(&m_hwdecFallbackTimer, &QTimer::timeout, this, &PlayerController::handleHwdecFallbackCheck);
    ensurePlaybackSignalConnections(&m_player);

    m_positionTimer.setInterval(1000);
    connect(&m_positionTimer, &QTimer::timeout, this, &PlayerController::updatePosition);
    m_positionTimer.start();
}

void PlayerController::ensurePlaybackSignalConnections(Player::MpvPlayer *player)
{
    if (player == nullptr || player->property(kPlaybackSignalsConnectedProperty).toBool()) {
        return;
    }

    player->setProperty(kPlaybackSignalsConnectedProperty, true);
    connect(player, &Player::MpvPlayer::fileLoaded, this, [this, player]() {
        if (playbackPlayer() != player) {
            return;
        }

        Core::DebugLogger::instance().log(QStringLiteral("player"), QStringLiteral("mpv signaled file-loaded."));
        if (m_reconnectActive) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Reconnect attempt reached file-loaded; waiting for actual playback recovery."));
        }
        setChannelLoadFailed(false);
        if (m_resumePlaybackAfterLoad) {
            startStartupBufferFallbackWatchdog();
            startStartupBufferProbe();
            evaluateStartupBufferAndResumeIfReady();
        }
        if (inCatchupMode()) {
            playbackPlayer()->setStartupBufferingStrictMode(false);
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Catch-up file-loaded: disabled strict startup cache-pause for responsive seeks."));
            if (m_catchupPendingStreamRelativeSeekSeconds.has_value()) {
                const auto residualSeekSeconds = std::max(0.0, m_catchupPendingStreamRelativeSeekSeconds.value());
                m_catchupPendingStreamRelativeSeekSeconds = std::nullopt;
                if (residualSeekSeconds > kPlaybackPositionEpsilon) {
                    Core::DebugLogger::instance().log(
                        QStringLiteral("player"),
                        QStringLiteral(
                            "Catch-up regenerated URL loaded; skipping residual stream seek (%1s) and starting from minute anchor.")
                            .arg(residualSeekSeconds, 0, 'f', 3));
                }
            }
        }
        emit playbackFileLoaded();
        clearPlaybackStallTracking();
        syncIsPlayingFromBackend();
        syncIsBufferingFromBackend();
        schedulePauseStateResync();
        evaluateReconnectRecovery();
    });
    connect(player, &Player::MpvPlayer::pauseStateChanged, this, [this, player](const bool paused) {
        if (playbackPlayer() != player) {
            return;
        }

        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("mpv pause state changed: %1").arg(paused ? "paused" : "playing"));
        if (!m_currentChannel.has_value()) {
            return;
        }

        const auto manualToggleRequested = m_pauseToggleRequested;
        if (manualToggleRequested) {
            m_pauseToggleRequested = false;
        }
        if (paused) {
            m_userPausedManually = manualToggleRequested;
        } else {
            m_userPausedManually = false;
        }

        if (paused) {
            clearPlaybackStallTracking();
            refreshBufferingState();
        }
        stopPauseStateResync();
        setIsPlaying(!paused);
        evaluateReconnectRecovery();
    });
    connect(player, &Player::MpvPlayer::bufferingStateChanged, this, [this, player](const bool buffering) {
        if (playbackPlayer() != player) {
            return;
        }

        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("mpv buffering state changed: %1").arg(buffering ? "buffering" : "ready"));
        if (!m_currentChannel.has_value()) {
            m_backendBuffering = false;
            clearPlaybackStallTracking();
            refreshBufferingState();
            return;
        }

        m_backendBuffering = buffering;
        if (buffering) {
            clearPlaybackStallTracking();
        }
        refreshBufferingState();
        evaluateReconnectRecovery();
    });
    connect(player, &Player::MpvPlayer::playbackEnded, this, [this, player]() {
        if (playbackPlayer() != player) {
            return;
        }

        Core::DebugLogger::instance().log(QStringLiteral("player"), QStringLiteral("mpv signaled playback ended."));
        clearReconnectAttemptInFlight(QStringLiteral("playback-ended"));
        stopStartupBufferProbe();
        stopStartupBufferFallbackWatchdog(true);
        if (m_resumePlaybackAfterLoad) {
            const auto tuneElapsedMs = m_tuneAttemptTimer.isValid() ? m_tuneAttemptTimer.elapsed() : 0;
            recoverPendingPlaybackLoadFailure(
                QStringLiteral("pending-playback-ended"),
                QStringLiteral("Playback ended while tune was pending (%1 ms elapsed); starting recovery.")
                    .arg(tuneElapsedMs));
            return;
        }
        if (m_currentChannel.has_value()) {
            stopPauseStateResync();
            stopDeferredLoadingIndicator();
            m_resumePlaybackAfterLoad = false;
            setChannelLoadFailed(false);
            setIsLoading(false);
            m_backendBuffering = false;
            clearPlaybackStallTracking();
            setIsPlaying(false);
            if (inCatchupMode()) {
                m_catchupProgramBoundaryReached = true;
                syncCatchupTimelineState();
                refreshBufferingState();
                return;
            }
            if (m_timeshiftController && m_timeshiftController->handlePlaybackFailure(QStringLiteral("playback-ended"))) {
                refreshBufferingState();
                return;
            }
            startReconnectLoop(QStringLiteral("playback-ended"));
            refreshBufferingState();
            return;
        }
        stopPauseStateResync();
        stopDeferredLoadingIndicator();
        m_resumePlaybackAfterLoad = false;
        stopReconnectLoop(QStringLiteral("playback-ended-no-channel"));
        setChannelLoadFailed(false);
        setIsLoading(false);
        m_backendBuffering = false;
        clearPlaybackStallTracking();
        refreshBufferingState();
        setIsPlaying(false);
    });
    connect(player, &Player::MpvPlayer::errorOccurred, this, [this, player](const QString &message) {
        if (playbackPlayer() != player) {
            return;
        }

        Core::DebugLogger::instance().log(QStringLiteral("player"), QStringLiteral("Error: %1").arg(message));
        clearReconnectAttemptInFlight(QStringLiteral("error"));
        stopStartupBufferProbe();
        stopStartupBufferFallbackWatchdog(true);
        if (m_resumePlaybackAfterLoad && m_currentChannel.has_value()) {
            recoverPendingPlaybackLoadFailure(
                QStringLiteral("pending-mpv-error"),
                QStringLiteral("mpv error while tune was pending: %1. Starting recovery.").arg(message));
            return;
        }
        if (!m_resumePlaybackAfterLoad && m_currentChannel.has_value()) {
            stopPauseStateResync();
            stopDeferredLoadingIndicator();
            m_resumePlaybackAfterLoad = false;
            setIsLoading(false);
            m_backendBuffering = false;
            clearPlaybackStallTracking();
            setIsPlaying(false);
            if (m_timeshiftController && m_timeshiftController->handlePlaybackFailure(QStringLiteral("mpv-error"))) {
                refreshBufferingState();
                return;
            }
            startReconnectLoop(QStringLiteral("mpv-error"));
            refreshBufferingState();
            return;
        }
        stopPauseStateResync();
        stopDeferredLoadingIndicator();
        stopReconnectLoop(QStringLiteral("fatal-error"));
        m_resumePlaybackAfterLoad = false;
        setChannelLoadFailed(true);
        setIsLoading(false);
        setChannelSwitchInProgress(false);
        m_backendBuffering = false;
        clearPlaybackStallTracking();
        refreshBufferingState();
        setIsPlaying(false);
        emit playbackError(message);
    });
    connect(player, &Player::MpvPlayer::videoReconfigured, this, [this, player]() {
        if (playbackPlayer() != player) {
            return;
        }

        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("mpv signaled video reconfiguration; keeping playback session active."));
        player->detectAndApplyDeinterlace();
        syncIsBufferingFromBackend();
        evaluateReconnectRecovery();
    });
}

bool PlayerController::isPlaying() const
{
    return m_isPlaying;
}

bool PlayerController::isLoading() const
{
    return m_isLoading;
}

bool PlayerController::isBuffering() const
{
    return m_isBuffering;
}

bool PlayerController::channelSwitchInProgress() const
{
    return m_channelSwitchInProgress;
}

bool PlayerController::channelLoadFailed() const
{
    return m_channelLoadFailed;
}

double PlayerController::volume() const
{
    return m_volume;
}

bool PlayerController::muted() const
{
    return m_muted;
}

void PlayerController::setVolume(const double value)
{
    const auto clamped = std::clamp(value, 0.0, 100.0);
    if (qFuzzyCompare(clamped, m_volume)) {
        return;
    }

    const auto wasMuted = m_muted;
    m_volume = clamped;
    if (m_volume > 0.0) {
        m_lastNonZeroVolume = m_volume;
    }
    m_muted = m_volume <= 0.0;
    playbackPlayer()->setVolume(static_cast<int>(m_volume));
    Core::DebugLogger::instance().log(QStringLiteral("player"), QStringLiteral("Volume set to %1.").arg(m_volume, 0, 'f', 1));
    emit volumeChanged();
    if (m_muted != wasMuted) {
        emit mutedChanged();
    }
}

QString PlayerController::positionText() const
{
    return m_positionText;
}

QString PlayerController::nowPlayingName() const
{
    return m_nowPlayingName;
}

QVariantMap PlayerController::currentChannel() const
{
    return m_currentChannel.has_value() ? toVariantMap(m_currentChannel.value()) : QVariantMap {};
}

QString PlayerController::playbackMode() const
{
    return m_playbackMode;
}

QString PlayerController::catchupProgramLabel() const
{
    return m_catchupProgramLabel;
}

bool PlayerController::catchupTimelineActive() const
{
    return inCatchupMode() && m_catchupProgramStartUtc.isValid() && m_catchupProgramStopUtc.isValid()
        && m_catchupProgramStopUtc > m_catchupProgramStartUtc;
}

qint64 PlayerController::catchupTimelineStartEpochMs() const
{
    return m_catchupTimelineStartEpochMs;
}

qint64 PlayerController::catchupTimelineAvailableEdgeEpochMs() const
{
    return m_catchupTimelineAvailableEdgeEpochMs;
}

double PlayerController::catchupTimelineAvailableSeconds() const
{
    return m_catchupTimelineAvailableSeconds;
}

double PlayerController::catchupTimelinePositionSeconds() const
{
    return m_catchupTimelinePositionSeconds;
}

bool PlayerController::catchupTimelineAtLiveEdge() const
{
    return m_catchupTimelineAtLiveEdge;
}

QString PlayerController::catchupTimelineNoticeText() const
{
    return m_catchupTimelineNoticeText;
}

bool PlayerController::timeshiftActive() const
{
    return m_timeshiftController && m_timeshiftController->isActive();
}

bool PlayerController::timeshiftPreparing() const
{
    return m_timeshiftController && m_timeshiftController->isPreparing();
}

bool PlayerController::timeshiftAtLiveEdge() const
{
    return !m_timeshiftController || m_timeshiftController->isAtLiveEdge();
}

double PlayerController::timeshiftBehindLiveSeconds() const
{
    return m_timeshiftController ? m_timeshiftController->behindLiveSeconds() : 0.0;
}

int PlayerController::timeshiftWindowSeconds() const
{
    return m_timeshiftController ? m_timeshiftController->windowSeconds() : 0;
}

double PlayerController::timeshiftAvailableSeconds() const
{
    return m_timeshiftController ? m_timeshiftController->availableDurationSeconds() : 0.0;
}

double PlayerController::timeshiftPositionSeconds() const
{
    return m_timeshiftController ? m_timeshiftController->currentPositionSeconds() : 0.0;
}

qint64 PlayerController::timeshiftWindowStartEpochMs() const
{
    return m_timeshiftController ? m_timeshiftController->windowStartEpochMs() : 0;
}

qint64 PlayerController::timeshiftLiveEdgeEpochMs() const
{
    return m_timeshiftController ? m_timeshiftController->liveEdgeEpochMs() : 0;
}

qint64 PlayerController::timeshiftAttachedWindowStartEpochMs() const
{
    return m_timeshiftController ? m_timeshiftController->attachedWindowStartEpochMs() : 0;
}

qint64 PlayerController::timeshiftAttachedWindowEndEpochMs() const
{
    return m_timeshiftController ? m_timeshiftController->attachedWindowEndEpochMs() : 0;
}

QString PlayerController::timeshiftNoticeText() const
{
    return m_timeshiftController ? m_timeshiftController->noticeText() : QString {};
}

QVariantMap PlayerController::debugOverlaySnapshot()
{
    const auto *activePlayer = playbackPlayer();
    const auto streamUrl = m_currentPlaybackUrl;
    const auto sourceWidth = activePlayer->videoWidth();
    const auto sourceHeight = activePlayer->videoHeight();
    const auto sourceResolution = sourceWidth.has_value() && sourceHeight.has_value()
        ? QStringLiteral("%1x%2").arg(sourceWidth.value()).arg(sourceHeight.value())
        : QStringLiteral("N/A");

    QString volumeText = QStringLiteral("N/A");
    if (const auto volume = activePlayer->volumePercent(); volume.has_value() && std::isfinite(volume.value())) {
        volumeText = QStringLiteral("%1%")
                         .arg(QString::number(std::round(std::clamp(volume.value(), 0.0, 100.0)), 'f', 0));
    }

    QVariant mpvBufferDurationSeconds;
    QString mpvBufferDurationText = QStringLiteral("N/A");
    if (const auto cacheDuration = activePlayer->demuxerCacheDurationSeconds();
        cacheDuration.has_value() && std::isfinite(cacheDuration.value())) {
        const auto normalizedDuration = std::max(0.0, cacheDuration.value());
        mpvBufferDurationSeconds = normalizedDuration;
        mpvBufferDurationText = formatDebugBufferDuration(normalizedDuration);
    }

    QVariant timeshiftBufferToLiveSeconds;
    QString timeshiftBufferToLiveText = QStringLiteral("N/A");
    const auto timeshiftActiveNow = timeshiftActive();
    if (timeshiftActiveNow) {
        const auto normalizedBehindLive = std::max(0.0, timeshiftBehindLiveSeconds());
        timeshiftBufferToLiveSeconds = normalizedBehindLive;
        timeshiftBufferToLiveText = formatDebugBufferDuration(normalizedBehindLive);
    }

    QVariant catchupEffectiveBufferDurationSeconds;
    QString catchupEffectiveBufferDurationText = QStringLiteral("N/A");
    if (!timeshiftActiveNow && inCatchupMode()) {
        if (const auto cacheDuration = activePlayer->demuxerCacheDurationSeconds();
            cacheDuration.has_value() && std::isfinite(cacheDuration.value())) {
            const auto normalizedDuration = std::max(0.0, cacheDuration.value());
            const auto effectiveDuration = std::min(normalizedDuration, kCatchupDebugEffectiveCacheMaxSeconds);
            catchupEffectiveBufferDurationSeconds = effectiveDuration;
            catchupEffectiveBufferDurationText = formatDebugBufferDuration(effectiveDuration);
        }
    }

    const auto useCatchupEffectiveCache = !timeshiftActiveNow && inCatchupMode();
    const auto bufferDurationSeconds = timeshiftActiveNow
        ? timeshiftBufferToLiveSeconds
        : (useCatchupEffectiveCache ? catchupEffectiveBufferDurationSeconds : mpvBufferDurationSeconds);
    const auto bufferDurationText = timeshiftActiveNow
        ? timeshiftBufferToLiveText
        : (useCatchupEffectiveCache ? catchupEffectiveBufferDurationText : mpvBufferDurationText);
    const auto bufferDurationSourceText = timeshiftActiveNow
        ? QStringLiteral("TS to live edge")
        : (useCatchupEffectiveCache ? QStringLiteral("Catch-up cache") : QStringLiteral("mpv cache"));

    const auto minBufferNeeded = activePlayer->bufferTargetSeconds();
    const auto minBufferNeededSeconds = std::isfinite(minBufferNeeded) && minBufferNeeded > 0.0 ? minBufferNeeded : 0.0;

    const auto averageBitrateBitsPerSecond = m_averageBitrateBitsPerSecond.has_value()
        ? m_averageBitrateBitsPerSecond
        : instantaneousBitrateBitsPerSecond(activePlayer);
    const auto bitrateText = averageBitrateBitsPerSecond.has_value()
        ? formatDebugBitrate(averageBitrateBitsPerSecond.value())
        : QStringLiteral("N/A");
    const auto interlaced = activePlayer->isInterlaced();
    const auto scanningText = interlaced.has_value()
        ? (interlaced.value() ? QStringLiteral("Interlaced") : QStringLiteral("Progressive"))
        : QStringLiteral("N/A");

    // When deinterlacing is ON but source is progressive, yadif would double the fps.
    // Show container (source) fps in that case so the display reflects the actual source rate.
    // When deinterlacing is ON and source is interlaced, the doubled fps is intentional.
    const bool useSourceFps = activePlayer->deinterlaceEnabled()
        && !interlaced.value_or(true);
    const auto frameRateFps = useSourceFps
        ? activePlayer->sourceFrameRateFps()
        : activePlayer->estimatedFrameRateFps();
    const auto frameRateText = frameRateFps.has_value()
        ? formatDebugFramerate(frameRateFps.value())
        : QStringLiteral("N/A");
    const auto droppedFrames = std::max(0, activePlayer->droppedFrameCount().value_or(0));
    const auto frameRateWithDropsText =
        QStringLiteral("%1 (dropped frames: %2)").arg(frameRateText).arg(droppedFrames);

    const auto tracks = activePlayer->trackList();
    int nv = 0, na = 0, ns = 0;
    for (const auto &t : tracks) {
        const auto tm = t.toMap();
        const auto type = tm.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("video")) {
            ++nv;
        } else if (type == QLatin1String("audio")) {
            ++na;
        } else if (type == QLatin1String("sub")) {
            ++ns;
        }
    }
    const auto streamsText = QStringLiteral("v(%1) a(%2) s(%3)").arg(nv).arg(na).arg(ns);

    return {
        { QStringLiteral("streamHost"), debugStreamHostFromUrl(streamUrl) },
        { QStringLiteral("streamId"), debugStreamIdFromUrl(streamUrl) },
        { QStringLiteral("streamsText"), streamsText },
        { QStringLiteral("sourceResolution"), sourceResolution },
        { QStringLiteral("scanningText"), scanningText },
        { QStringLiteral("volumeText"), volumeText },
        { QStringLiteral("videoCodec"), activePlayer->videoCodec().value_or(QStringLiteral("N/A")) },
        { QStringLiteral("audioCodec"), activePlayer->audioCodec().value_or(QStringLiteral("N/A")) },
        { QStringLiteral("frameRateText"), frameRateWithDropsText },
        { QStringLiteral("bitrateText"), bitrateText },
        { QStringLiteral("bitrateValueKbps"),
            averageBitrateBitsPerSecond.has_value() ? averageBitrateBitsPerSecond.value() / 1000.0 : -1.0 },
        { QStringLiteral("bufferDurationSeconds"), bufferDurationSeconds },
        { QStringLiteral("bufferDurationText"), bufferDurationText },
        { QStringLiteral("bufferDurationSourceText"), bufferDurationSourceText },
        { QStringLiteral("minBufferNeededSeconds"), minBufferNeededSeconds },
        { QStringLiteral("mpvBufferDurationSeconds"), mpvBufferDurationSeconds },
        { QStringLiteral("mpvBufferDurationText"), mpvBufferDurationText },
        { QStringLiteral("catchupEffectiveBufferDurationSeconds"), catchupEffectiveBufferDurationSeconds },
        { QStringLiteral("catchupEffectiveBufferDurationText"), catchupEffectiveBufferDurationText },
        { QStringLiteral("timeshiftBufferToLiveSeconds"), timeshiftBufferToLiveSeconds },
        { QStringLiteral("timeshiftBufferToLiveText"), timeshiftBufferToLiveText },
        { QStringLiteral("timeshiftMode"),
            !timeshiftActive()
                ? QStringLiteral("Off")
                : (timeshiftAtLiveEdge() ? QStringLiteral("Live") : QStringLiteral("Behind")) },
        { QStringLiteral("timeshiftBehindLiveText"),
            !timeshiftActive()
                ? QStringLiteral("N/A")
                : formatDebugBufferDuration(std::max(0.0, timeshiftBehindLiveSeconds())) },
        { QStringLiteral("timeshiftWindowText"),
            timeshiftWindowSeconds() > 0
                ? formatDebugBufferDuration(static_cast<double>(timeshiftWindowSeconds()))
                : QStringLiteral("N/A") },
        { QStringLiteral("timeshiftTracksText"),
            !timeshiftActive()
                ? QStringLiteral("N/A")
                : QStringLiteral("a(%1) s(%2)")
                      .arg(m_timeshiftController ? m_timeshiftController->audioTrackCount() : 0)
                      .arg(m_timeshiftController ? m_timeshiftController->subtitleTrackCount() : 0) },
        { QStringLiteral("timeshiftDroppedSubsText"),
            !timeshiftActive()
                ? QStringLiteral("N/A")
                : (m_timeshiftController ? m_timeshiftController->droppedSubtitleSummary() : QStringLiteral("None")) },
        { QStringLiteral("timeshiftSeekableText"),
            !timeshiftActive()
                ? QStringLiteral("N/A")
                : (activePlayer->seekable().value_or(false) ? QStringLiteral("Yes") : QStringLiteral("No")) },
        { QStringLiteral("timeshiftAttachedText"),
            !timeshiftActive()
                ? QStringLiteral("N/A")
                : QStringLiteral("%1 -> %2")
                      .arg(debugTimestampLocal(timeshiftAttachedWindowStartEpochMs()))
                      .arg(debugTimestampLocal(timeshiftAttachedWindowEndEpochMs())) },
        { QStringLiteral("timeshiftCurrentPointText"),
            !timeshiftActive()
                ? QStringLiteral("N/A")
                : debugTimestampLocal(m_timeshiftController ? m_timeshiftController->currentPlaybackEpochMs() : 0) },
        { QStringLiteral("timeshiftSeekModeText"),
            !timeshiftActive()
                ? QStringLiteral("N/A")
                : (m_timeshiftController ? m_timeshiftController->lastSeekModeText() : QStringLiteral("N/A")) },
        { QStringLiteral("timestamp"), debugTimestampNowLocal() }
    };
}

static QString trackSubtitle(const QVariantMap &track)
{
    auto title = track.value(QStringLiteral("title")).toString().trimmed();
    if (!title.isEmpty()) {
        return title;
    }
    return track.value(QStringLiteral("lang")).toString().trimmed();
}

QVariantList PlayerController::audioTracks()
{
    const auto *activePlayer = playbackPlayer();
    QVariantList result;
    int displayIndex = 1;
    for (const auto &t : activePlayer->trackList()) {
        const auto tm = t.toMap();
        if (tm.value(QStringLiteral("type")).toString() != QLatin1String("audio")) {
            continue;
        }
        QVariantMap entry;
        entry[QStringLiteral("id")]       = tm.value(QStringLiteral("id"));
        entry[QStringLiteral("name")]     = QStringLiteral("Audio #%1").arg(displayIndex++);
        entry[QStringLiteral("subtitle")] = trackSubtitle(tm);
        result.append(entry);
    }
    return result;
}

QVariantList PlayerController::subtitleTracks()
{
    const auto *activePlayer = playbackPlayer();
    QVariantMap none;
    none[QStringLiteral("id")]       = 0;
    none[QStringLiteral("name")]     = QStringLiteral("Subtitle #0");
    none[QStringLiteral("subtitle")] = QStringLiteral("None");
    QVariantList result;
    result.append(none);
    int displayIndex = 1;
    for (const auto &t : activePlayer->trackList()) {
        const auto tm = t.toMap();
        if (tm.value(QStringLiteral("type")).toString() != QLatin1String("sub")) {
            continue;
        }
        QVariantMap entry;
        entry[QStringLiteral("id")]       = tm.value(QStringLiteral("id"));
        entry[QStringLiteral("name")]     = QStringLiteral("Subtitle #%1").arg(displayIndex++);
        entry[QStringLiteral("subtitle")] = trackSubtitle(tm);
        result.append(entry);
    }
    return result;
}

void PlayerController::selectAudioTrack(const int id)
{
    auto *activePlayer = playbackPlayer();
    if (!activePlayer->isAvailable()) {
        return;
    }
    activePlayer->selectAudioTrack(id);
}

void PlayerController::selectSubtitleTrack(const int id)
{
    auto *activePlayer = playbackPlayer();
    if (!activePlayer->isAvailable()) {
        return;
    }
    activePlayer->selectSubtitleTrack(id);
}

QString PlayerController::debugStreamHostFromUrl(const QString &url)
{
    const auto trimmed = url.trimmed();
    if (trimmed.isEmpty()) {
        return QStringLiteral("N/A");
    }

    auto parsed = QUrl(trimmed);
    if (!parsed.isValid() || parsed.host().trimmed().isEmpty()) {
        parsed = QUrl::fromUserInput(trimmed);
    }

    const auto host = parsed.host().trimmed();
    return host.isEmpty() ? QStringLiteral("N/A") : host;
}

QString PlayerController::debugStreamIdFromUrl(const QString &url)
{
    const auto trimmed = url.trimmed();
    if (trimmed.isEmpty()) {
        return QStringLiteral("N/A");
    }

    auto parsed = QUrl(trimmed);
    if (!parsed.isValid()) {
        parsed = QUrl::fromUserInput(trimmed);
    }

    const auto path = parsed.path();
    if (!path.isEmpty()) {
        const auto segments = path.split(u'/', Qt::SkipEmptyParts);
        if (!segments.isEmpty()) {
            const auto streamId = segments.constLast().trimmed();
            if (!streamId.isEmpty()) {
                return streamId;
            }
        }
    }

    return fallbackStreamSegment(trimmed);
}

QString PlayerController::debugTimestampNowLocal()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto secondsPoint = time_point_cast<std::chrono::seconds>(now);
    const auto micros = duration_cast<std::chrono::microseconds>(now - secondsPoint).count();
    const auto timestamp = system_clock::to_time_t(secondsPoint);
    const auto timeInfo = localTimeFrom(timestamp);

    std::ostringstream stream;
    stream << std::put_time(&timeInfo, "%Y-%m-%d %H:%M:%S") << '.'
           << std::setw(6) << std::setfill('0') << micros;
    return QString::fromStdString(stream.str());
}

QString PlayerController::formatDebugBufferDuration(const double bufferDurationSeconds)
{
    if (!std::isfinite(bufferDurationSeconds) || bufferDurationSeconds < 0.0) {
        return QStringLiteral("N/A");
    }

    return QStringLiteral("%1 s").arg(bufferDurationSeconds, 0, 'f', 2);
}

QString PlayerController::formatDebugFramerate(const double framesPerSecond)
{
    if (!std::isfinite(framesPerSecond) || framesPerSecond <= 0.0) {
        return QStringLiteral("N/A");
    }

    return QStringLiteral("%1 fps").arg(framesPerSecond, 0, 'f', 2);
}

QString PlayerController::formatDebugBitrate(const double bitsPerSecond)
{
    if (!std::isfinite(bitsPerSecond) || bitsPerSecond < 0.0) {
        return QStringLiteral("N/A");
    }

    const auto kilobitsPerSecond = bitsPerSecond / 1000.0;
    return QStringLiteral("%1 Kbps").arg(kilobitsPerSecond, 0, 'f', 0);
}

double PlayerController::adaptiveSteadyStateCacheLimitSeconds(const double bufferTargetSeconds)
{
    return Player::MpvPlayer::steadyStateCacheLimitSecondsForBufferTarget(bufferTargetSeconds);
}

double PlayerController::adaptiveSteadyStateCacheHysteresisSeconds(const double bufferTargetSeconds)
{
    return Player::MpvPlayer::steadyStateCacheHysteresisSecondsForBufferTarget(bufferTargetSeconds);
}

qint64 PlayerController::adaptiveSteadyStateMaxBytes(
    const double bufferTargetSeconds,
    const std::optional<double> averageBitsPerSecond)
{
    const auto cacheLimitSeconds = adaptiveSteadyStateCacheLimitSeconds(bufferTargetSeconds);
    if (!averageBitsPerSecond.has_value()
        || !std::isfinite(averageBitsPerSecond.value())
        || averageBitsPerSecond.value() <= 0.0) {
        return Player::MpvPlayer::demuxerMaxBytesForBufferSeconds(cacheLimitSeconds);
    }

    const auto dynamicBudgetBytes = static_cast<qint64>(std::llround(
        (averageBitsPerSecond.value() / 8.0) * cacheLimitSeconds * kAdaptiveSteadyStateMaxBytesSafetyMultiplier));
    const auto maxBudgetBytes = Player::MpvPlayer::demuxerMaxBytesForBufferSeconds(kMaximumBufferSeconds);
    return std::clamp(dynamicBudgetBytes, kAdaptiveSteadyStateMinBytes, maxBudgetBytes);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int PlayerController::startupBufferFallbackTimeoutMs(const double bufferTargetSeconds, const int segmentSecondsHint)
{
    const auto normalized = normalizedBufferTargetSeconds(bufferTargetSeconds);
    const auto baseTimeoutMs = static_cast<int>(std::lround(normalized * 1000.0));
    if (segmentSecondsHint <= 0) {
        return baseTimeoutMs;
    }

    const auto normalizedSegmentSeconds = std::clamp(segmentSecondsHint, 2, 60);
    const auto segmentSafetyMs = static_cast<int>(std::lround((static_cast<double>(normalizedSegmentSeconds) + 1.0) * 1000.0));
    constexpr int kStartupFallbackTimeoutMaxMs = 180000;
    return std::min(kStartupFallbackTimeoutMaxMs, baseTimeoutMs + segmentSafetyMs);
}

int PlayerController::reconnectDepletionTimeoutMsForWaitSeconds(const double waitForDataStreamSeconds)
{
    const auto normalizedWait = normalizedWaitForDataStreamSeconds(waitForDataStreamSeconds);
    const auto waitTimeoutMs = static_cast<int>(std::lround(normalizedWait * 1000.0));
    return std::max(kReconnectPostDepletionGraceMs, waitTimeoutMs);
}

bool PlayerController::shouldStartPreemptiveReconnect(
    const std::optional<double> cacheDurationSeconds,
    const double bufferTargetSeconds,
    const std::optional<double> throughputBitsPerSecond,
    const bool playbackAdvanced,
    const bool backendBuffering)
{
    // Some platforms keep backendBuffering=true for a long time after stream loss.
    // Do not suppress reconnect if playback has stopped advancing.
    if (backendBuffering && playbackAdvanced) {
        return false;
    }

    const auto weakThroughput = !throughputBitsPerSecond.has_value()
        || !std::isfinite(throughputBitsPerSecond.value())
        || std::max(0.0, throughputBitsPerSecond.value()) <= kDeadStreamLowThroughputBitsPerSecond;

    bool lowBufferRatio = false;
    if (cacheDurationSeconds.has_value() && std::isfinite(cacheDurationSeconds.value())
        && std::isfinite(bufferTargetSeconds) && bufferTargetSeconds > 0.0) {
        const auto normalizedCacheDuration = std::max(0.0, cacheDurationSeconds.value());
        const auto normalizedBufferTarget = normalizedBufferTargetSeconds(bufferTargetSeconds);
        const auto bufferRatio = normalizedCacheDuration / normalizedBufferTarget;
        lowBufferRatio = bufferRatio <= kPreemptiveReconnectBufferRatioThreshold;
    }

    return weakThroughput && (lowBufferRatio || !playbackAdvanced);
}

bool PlayerController::deadStreamLikelyDisconnected(
    const std::optional<double> cacheDurationSeconds,
    const std::optional<double> throughputBitsPerSecond)
{
    if (!cacheDurationSeconds.has_value() || !std::isfinite(cacheDurationSeconds.value())) {
        return false;
    }

    const auto normalizedCacheDuration = std::max(0.0, cacheDurationSeconds.value());
    if (normalizedCacheDuration > kReconnectDepletedBufferThresholdSeconds) {
        return false;
    }

    if (!throughputBitsPerSecond.has_value() || !std::isfinite(throughputBitsPerSecond.value())) {
        return true;
    }

    const auto normalizedThroughput = std::max(0.0, throughputBitsPerSecond.value());
    return normalizedThroughput <= kDeadStreamLowThroughputBitsPerSecond;
}

int PlayerController::reconnectAttemptIntervalMs()
{
    return kReconnectAttemptIntervalMs;
}

void PlayerController::startReconnectLoop(const QString &reason)
{
    auto *activePlayer = playbackPlayer();
    if (activePlayer == nullptr || !m_currentChannel.has_value() || m_resumePlaybackAfterLoad || m_channelLoadFailed) {
        return;
    }
    if (m_timeshiftController && m_timeshiftController->isActive()) {
        return;
    }

    if (m_reconnectActive) {
        return;
    }

    m_reconnectActive = true;
    m_reconnectAttemptCount = 0;
    m_reconnectAttemptInFlight = false;
    m_reconnectStabilizing = false;
    m_reconnectAttemptIssuedTimer.invalidate();
    m_reconnectRecoveryHealthyTickCount = 0;
    m_reconnectRecoveryUnhealthyTickCount = 0;
    m_reconnectStabilizationRefillTickCount = 0;
    m_noRefillConsecutiveCount = 0;
    m_videoFreezeConsecutiveCount = 0;
    m_lastObservedCacheDurationSeconds = std::nullopt;
    m_lastReconnectStabilizationCacheDurationSeconds = std::nullopt;
    m_lastDisplayedVideoFramePtsSeconds = std::nullopt;
    m_userPausedManually = false;

    const auto reconnectUrl = recoveryPlaybackUrl();
    const auto reconnectLoadfileOptions = recoveryLoadfileOptions();
    if (reconnectUrl.isEmpty()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Reconnect loop aborted: no recovery URL for current playback mode."));
        return;
    }

    const auto attemptTimeoutMs = static_cast<int>(std::lround(
        normalizedWaitForDataStreamSeconds(m_waitForDataStreamSeconds) * 1000.0));
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        reconnectLoadfileOptions.isEmpty()
            ? QStringLiteral("Reconnect loop started: reason=%1 stream=%2 wait-for-data=%3s attempt-timeout=%4ms max-attempts=%5")
                  .arg(reason, reconnectUrl)
                  .arg(m_waitForDataStreamSeconds, 0, 'f', 1)
                  .arg(attemptTimeoutMs)
                  .arg(kReconnectMaxAttempts)
            : QStringLiteral("Reconnect loop started: reason=%1 stream=%2 options=%3 wait-for-data=%4s attempt-timeout=%5ms max-attempts=%6")
                  .arg(reason, reconnectUrl, reconnectLoadfileOptions)
                  .arg(m_waitForDataStreamSeconds, 0, 'f', 1)
                  .arg(attemptTimeoutMs)
                  .arg(kReconnectMaxAttempts));
    // Reconnect should be best-effort and not wait for strict startup cache-pause.
    activePlayer->setStartupBufferingStrictMode(false);
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Reconnect loop enabled best-effort startup buffering for recovery attempts."));
    activePlayer->setPaused(false);
    refreshBufferingState();
    m_reconnectAttemptTimer.start();
    handleReconnectAttemptTick();
}

void PlayerController::stopReconnectLoop(const QString &reason)
{
    const auto wasActive = m_reconnectActive;
    auto *activePlayer = playbackPlayer();
    m_reconnectAttemptTimer.stop();
    m_reconnectActive = false;
    m_reconnectAttemptCount = 0;
    m_reconnectAttemptInFlight = false;
    m_reconnectStabilizing = false;
    m_reconnectAttemptIssuedTimer.invalidate();
    m_reconnectRecoveryHealthyTickCount = 0;
    m_reconnectRecoveryUnhealthyTickCount = 0;
    m_reconnectStabilizationRefillTickCount = 0;
    m_noRefillConsecutiveCount = 0;
    m_videoFreezeConsecutiveCount = 0;
    m_lastObservedCacheDurationSeconds = std::nullopt;
    m_lastReconnectStabilizationCacheDurationSeconds = std::nullopt;
    m_lastDisplayedVideoFramePtsSeconds = std::nullopt;
    if (wasActive && activePlayer != nullptr && !inCatchupMode()) {
        activePlayer->setStartupBufferingStrictMode(true);
        m_reconnectWatchdogCooldownTimer.restart();
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Reconnect loop restored strict startup buffering and started watchdog cooldown (%1 ms).")
                .arg(kSoftReconnectWatchdogCooldownMs));
    }
    if (wasActive) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Reconnect loop stopped: reason=%1").arg(reason));
    }
    refreshBufferingState();
}

void PlayerController::clearReconnectAttemptInFlight(const QString &reason)
{
    if (!m_reconnectAttemptInFlight) {
        return;
    }

    m_reconnectAttemptInFlight = false;
    m_reconnectStabilizing = false;
    m_reconnectAttemptIssuedTimer.invalidate();
    m_reconnectRecoveryHealthyTickCount = 0;
    m_reconnectRecoveryUnhealthyTickCount = 0;
    m_reconnectStabilizationRefillTickCount = 0;
    m_lastReconnectStabilizationCacheDurationSeconds = std::nullopt;
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Reconnect attempt resolved: %1.").arg(reason));
}

void PlayerController::recoverPendingPlaybackLoadFailure(const QString &reason, const QString &logMessage)
{
    if (!m_currentChannel.has_value()) {
        return;
    }

    if (!logMessage.trimmed().isEmpty()) {
        Core::DebugLogger::instance().log(QStringLiteral("player"), logMessage);
    }

    stopPauseStateResync();
    stopDeferredLoadingIndicator();
    m_resumePlaybackAfterLoad = false;
    setChannelLoadFailed(false);
    setIsLoading(false);
    m_backendBuffering = false;
    clearPlaybackStallTracking();
    setIsPlaying(false);
    if (m_timeshiftController && m_timeshiftController->handlePlaybackFailure(reason)) {
        refreshBufferingState();
        return;
    }
    startReconnectLoop(reason);
    refreshBufferingState();
}

void PlayerController::handleReconnectAttemptTick()
{
    auto *activePlayer = playbackPlayer();
    if (!m_reconnectActive) {
        return;
    }

    if (activePlayer == nullptr || !m_currentChannel.has_value() || m_resumePlaybackAfterLoad) {
        stopReconnectLoop(QStringLiteral("channel-or-mode-changed"));
        return;
    }

    const auto waitTimeoutMs = static_cast<int>(std::lround(
        normalizedWaitForDataStreamSeconds(m_waitForDataStreamSeconds) * 1000.0));
    if (m_reconnectAttemptInFlight) {
        if (m_reconnectStabilizing) {
            return;
        }
        if (!m_reconnectAttemptIssuedTimer.isValid()) {
            m_reconnectAttemptIssuedTimer.restart();
            return;
        }
        if (m_reconnectAttemptIssuedTimer.elapsed() < waitTimeoutMs) {
            return;
        }

        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Reconnect attempt #%1 timed out after %2 ms; stopping current attempt before retry.")
                .arg(m_reconnectAttemptCount)
                .arg(waitTimeoutMs));
        activePlayer->stop();
        clearReconnectAttemptInFlight(QStringLiteral("attempt-timeout"));
    }

    if (m_reconnectAttemptCount >= kReconnectMaxAttempts) {
        failReconnect(QStringLiteral("attempt-limit"));
        return;
    }

    const auto url = recoveryPlaybackUrl();
    const auto loadfileOptions = recoveryLoadfileOptions();
    if (url.isEmpty()) {
        failReconnect(QStringLiteral("empty-stream-url"));
        return;
    }

    if (m_reconnectAttemptCount > 0) {
        // Hard-stop any lingering transport from the previous failed attempt before opening a new one.
        activePlayer->stop();
    }

    m_reconnectAttemptCount += 1;
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        loadfileOptions.isEmpty()
            ? QStringLiteral("Reconnect attempt #%1/%2 to %3.")
                  .arg(m_reconnectAttemptCount)
                  .arg(kReconnectMaxAttempts)
                  .arg(url)
            : QStringLiteral("Reconnect attempt #%1/%2 to %3 (%4).")
                  .arg(m_reconnectAttemptCount)
                  .arg(kReconnectMaxAttempts)
                  .arg(url, loadfileOptions));
    activePlayer->setStartupBufferingStrictMode(false);
    m_reconnectStabilizing = false;
    m_reconnectRecoveryHealthyTickCount = 0;
    m_reconnectRecoveryUnhealthyTickCount = 0;
    m_reconnectStabilizationRefillTickCount = 0;
    m_lastReconnectStabilizationCacheDurationSeconds = std::nullopt;
    m_reconnectAttemptInFlight = true;
    m_reconnectAttemptIssuedTimer.restart();
    activePlayer->setPaused(false);
    activePlayer->play(url, loadfileOptions);
}

void PlayerController::failReconnect(const QString &reason)
{
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Reconnect failed: %1. Marking channel as load failure.").arg(reason));
    stopReconnectLoop(reason);
    playbackPlayer()->stop();
    stopPauseStateResync();
    stopDeferredLoadingIndicator();
    m_resumePlaybackAfterLoad = false;
    setChannelLoadFailed(true);
    setIsLoading(false);
    m_backendBuffering = false;
    clearPlaybackStallTracking();
    refreshBufferingState();
    setIsPlaying(false);
    emit playbackError(QStringLiteral("Channel couldn't be loaded"));
}

void PlayerController::evaluateReconnectRecovery()
{
    if (!m_reconnectActive || !m_currentChannel.has_value() || m_resumePlaybackAfterLoad) {
        return;
    }

    if (m_reconnectStabilizing
        && m_reconnectRecoveryHealthyTickCount >= kReconnectStabilizationStableTickThreshold
        && m_reconnectStabilizationRefillTickCount >= kReconnectStabilizationMinRefillTicks) {
        clearReconnectAttemptInFlight(QStringLiteral("stabilization-healthy"));
        stopReconnectLoop(QStringLiteral("playback-recovered"));
        return;
    }

    if (m_reconnectStabilizing
        && m_reconnectRecoveryUnhealthyTickCount >= kReconnectStabilizationUnstableTickThreshold) {
        auto *activePlayer = playbackPlayer();
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Reconnect stabilization failed (%1 unstable ticks); retrying next sequential attempt.")
                .arg(m_reconnectRecoveryUnhealthyTickCount));
        if (activePlayer != nullptr) {
            activePlayer->stop();
        }
        clearReconnectAttemptInFlight(QStringLiteral("stabilization-failed"));
        handleReconnectAttemptTick();
    }
}

void PlayerController::resetBitrateAverageWindow()
{
    m_debugBitrateSamples.clear();
    m_averageBitrateBitsPerSecond = std::nullopt;
}

void PlayerController::resetAdaptiveSteadyStateBufferingState()
{
    m_steadyStateBufferRetuneTimer.invalidate();
    m_lastAdaptiveCacheLimitSeconds = -1.0;
    m_lastAdaptiveCacheHysteresisSeconds = -1.0;
    m_lastAdaptiveDemuxerMaxBytes = -1;
}

std::optional<double> PlayerController::updateBitrateAverageBitsPerSecond(
    const std::optional<double> instantaneousBitsPerSecond)
{
    using namespace std::chrono;

    const auto nowMs = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    const auto minTimestampMs = nowMs - kDebugBitrateWindowMs;

    while (!m_debugBitrateSamples.empty() && m_debugBitrateSamples.front().first < minTimestampMs) {
        m_debugBitrateSamples.pop_front();
    }

    if (!instantaneousBitsPerSecond.has_value()
        || !std::isfinite(instantaneousBitsPerSecond.value())
        || instantaneousBitsPerSecond.value() < 0.0) {
        m_debugBitrateSamples.clear();
        m_averageBitrateBitsPerSecond = std::nullopt;
        return std::nullopt;
    }

    m_debugBitrateSamples.emplace_back(nowMs, instantaneousBitsPerSecond.value());

    double totalBitsPerSecond = 0.0;
    for (const auto &[_, sampleBitsPerSecond] : m_debugBitrateSamples) {
        totalBitsPerSecond += sampleBitsPerSecond;
    }
    if (m_debugBitrateSamples.empty()) {
        m_averageBitrateBitsPerSecond = std::nullopt;
        return std::nullopt;
    }

    m_averageBitrateBitsPerSecond = totalBitsPerSecond / static_cast<double>(m_debugBitrateSamples.size());
    return m_averageBitrateBitsPerSecond;
}

void PlayerController::maybeRetuneSteadyStateBuffering(
    const std::optional<double> cacheDurationSeconds,
    const double bufferTargetSeconds)
{
    if (!m_isPlaying
        || inCatchupMode()
        || !m_currentChannel.has_value()
        || m_resumePlaybackAfterLoad
        || m_reconnectActive
        || m_sharedPlaybackPlayer
        || m_channelLoadFailed
        || m_userPausedManually) {
        return;
    }

    const auto cacheLimitSeconds = adaptiveSteadyStateCacheLimitSeconds(bufferTargetSeconds);
    const auto hysteresisSeconds = adaptiveSteadyStateCacheHysteresisSeconds(bufferTargetSeconds);
    const auto maxBytes = adaptiveSteadyStateMaxBytes(bufferTargetSeconds, m_averageBitrateBitsPerSecond);

    const auto secondsChanged =
        m_lastAdaptiveCacheLimitSeconds < 0.0
        || std::abs(cacheLimitSeconds - m_lastAdaptiveCacheLimitSeconds) >= kAdaptiveSteadyStateSecondsRetuneThreshold
        || m_lastAdaptiveCacheHysteresisSeconds < 0.0
        || std::abs(hysteresisSeconds - m_lastAdaptiveCacheHysteresisSeconds) >= kAdaptiveSteadyStateSecondsRetuneThreshold;
    const auto bytesChanged =
        m_lastAdaptiveDemuxerMaxBytes <= 0
        || std::abs(static_cast<double>(maxBytes - m_lastAdaptiveDemuxerMaxBytes))
            >= static_cast<double>(std::max<qint64>(
                1,
                std::llround(std::abs(static_cast<double>(m_lastAdaptiveDemuxerMaxBytes))
                    * kAdaptiveSteadyStateBytesRetuneThresholdRatio)));
    if (!secondsChanged && !bytesChanged) {
        return;
    }

    const auto cacheBelowBand = cacheDurationSeconds.has_value()
        && std::isfinite(cacheDurationSeconds.value())
        && std::max(0.0, cacheDurationSeconds.value()) + kNoRefillCacheIncreaseEpsilonSeconds < hysteresisSeconds;
    if (m_steadyStateBufferRetuneTimer.isValid()
        && m_steadyStateBufferRetuneTimer.elapsed() < kAdaptiveSteadyStateRetuneIntervalMs
        && !cacheBelowBand) {
        return;
    }

    playbackPlayer()->setSteadyStateBufferingPolicy({
        .cacheLimitSeconds = cacheLimitSeconds,
        .hysteresisSeconds = hysteresisSeconds,
        .maxBytes = maxBytes,
    });
    m_lastAdaptiveCacheLimitSeconds = cacheLimitSeconds;
    m_lastAdaptiveCacheHysteresisSeconds = hysteresisSeconds;
    m_lastAdaptiveDemuxerMaxBytes = maxBytes;
    m_steadyStateBufferRetuneTimer.restart();

    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral(
            "Adaptive steady-state buffering retune: avg-bitrate=%1 cache=%2 buffer=%3 limit=%4 hysteresis=%5 max-bytes=%6.")
            .arg(
                m_averageBitrateBitsPerSecond.has_value()
                    ? QString::number(m_averageBitrateBitsPerSecond.value(), 'f', 0)
                    : QStringLiteral("N/A"))
            .arg(
                cacheDurationSeconds.has_value() && std::isfinite(cacheDurationSeconds.value())
                    ? QString::number(std::max(0.0, cacheDurationSeconds.value()), 'f', 2)
                    : QStringLiteral("N/A"))
            .arg(bufferTargetSeconds, 0, 'f', 1)
            .arg(cacheLimitSeconds, 0, 'f', 1)
            .arg(hysteresisSeconds, 0, 'f', 1)
            .arg(maxBytes));
}

Player::MpvPlayer *PlayerController::player()
{
    return playbackPlayer();
}

void PlayerController::setTimeshiftController(TimeshiftController *controller)
{
    if (m_timeshiftController == controller) {
        return;
    }

    if (m_timeshiftController) {
        disconnect(m_timeshiftController, nullptr, this, nullptr);
    }

    m_timeshiftController = controller;
    if (m_timeshiftController) {
        connect(m_timeshiftController, &TimeshiftController::stateChanged, this, [this]() {
            emit timeshiftStateChanged();
        });
    }

    emit timeshiftStateChanged();
}

Player::MpvPlayer *PlayerController::primaryBasePlayer()
{
    return &m_player;
}

Player::MpvPlayer *PlayerController::playbackPlayer()
{
    return m_sharedPlaybackPlayer ? m_sharedPlaybackPlayer.data() : &m_player;
}

const Player::MpvPlayer *PlayerController::playbackPlayer() const
{
    return m_sharedPlaybackPlayer ? m_sharedPlaybackPlayer.data() : &m_player;
}

void PlayerController::attachSharedPlayback(
    Player::MpvPlayer *sharedPlayer,
    const Channel &channel,
    const bool protectedSession,
    const bool stopBasePlayerOnInitialAttach)
{
    if (sharedPlayer == nullptr) {
        return;
    }

    if (!m_sharedPlaybackPlayer) {
        stopRecording();
        if (stopBasePlayerOnInitialAttach) {
            m_player.stop();
        }
    }

    const auto previousName = m_nowPlayingName;
    stopReconnectLoop(QStringLiteral("attach-shared-playback"));
    ensurePlaybackSignalConnections(sharedPlayer);
    m_sharedPlaybackPlayer = sharedPlayer;
    m_sharedPlaybackProtected = protectedSession;
    clearCatchupState();
    m_sharedPlaybackPlayer->setAudioEnabled(true);
    const auto effectiveVolume = (m_muted || m_volume <= 0.0)
        ? 0
        : static_cast<int>(std::round(std::clamp(m_volume, 0.0, 100.0)));
    m_sharedPlaybackPlayer->setVolume(effectiveVolume);
    m_currentChannel = channel;
    const auto previousPlaybackUrl = m_currentPlaybackUrl;
    m_currentPlaybackUrl = channel.streamUrl;
    m_nowPlayingName = channel.name;
    setChannelLoadFailed(false);
    setIsLoading(false);
    setChannelSwitchInProgress(false);
    m_backendBuffering = false;
    resetBitrateAverageWindow();
    resetAdaptiveSteadyStateBufferingState();
    clearPlaybackStallTracking();
    refreshBufferingState();
    syncIsPlayingFromBackend();
    if (!isPlaying()) {
        setIsPlaying(true);
    }
    emit currentChannelChanged();
    if (previousPlaybackUrl != m_currentPlaybackUrl) {
        emit currentPlaybackUrlChanged();
    }
    emit playbackChannelActivated(channel.id);
    if (m_nowPlayingName != previousName) {
        emit nowPlayingNameChanged();
    }
}

void PlayerController::adoptExistingPlaybackChannel(const Channel &channel)
{
    const auto previousName = m_nowPlayingName;
    stopReconnectLoop(QStringLiteral("adopt-existing-playback"));
    stopPauseStateResync();
    stopDeferredLoadingIndicator();
    stopStartupBufferProbe();
    stopStartupBufferFallbackWatchdog(true);
    m_resumePlaybackAfterLoad = false;
    m_backendBuffering = false;
    resetBitrateAverageWindow();
    resetAdaptiveSteadyStateBufferingState();
    clearPlaybackStallTracking();
    refreshBufferingState();
    clearCatchupState();
    setChannelLoadFailed(false);
    setIsLoading(false);
    setChannelSwitchInProgress(false);
    syncIsPlayingFromBackend();
    m_currentChannel = channel;
    const auto previousPlaybackUrl = m_currentPlaybackUrl;
    m_currentPlaybackUrl = channel.streamUrl;
    m_nowPlayingName = channel.name;
    emit currentChannelChanged();
    if (previousPlaybackUrl != m_currentPlaybackUrl) {
        emit currentPlaybackUrlChanged();
    }
    emit playbackChannelActivated(channel.id);
    if (m_nowPlayingName != previousName) {
        emit nowPlayingNameChanged();
    }
}

void PlayerController::detachSharedPlayback(const bool clearChannel)
{
    if (!m_sharedPlaybackPlayer) {
        return;
    }

    const auto sharedPlayer = m_sharedPlaybackPlayer;
    const auto protectedSession = m_sharedPlaybackProtected;
    m_sharedPlaybackPlayer = nullptr;
    m_sharedPlaybackProtected = false;
    if (sharedPlayer && protectedSession) {
        sharedPlayer->setAudioEnabled(false);
        sharedPlayer->setVolume(0);
    }
    stopPauseStateResync();
    stopDeferredLoadingIndicator();
    stopStartupBufferProbe();
    stopStartupBufferFallbackWatchdog(true);
    stopReconnectLoop(QStringLiteral("detach-shared-playback"));
    resetBitrateAverageWindow();
    resetAdaptiveSteadyStateBufferingState();
    m_player.resetSteadyStateBuffering();
    clearCatchupState();
    m_resumePlaybackAfterLoad = false;
    setIsLoading(false);
    setChannelSwitchInProgress(false);
    m_backendBuffering = false;
    clearPlaybackStallTracking();
    refreshBufferingState();
    setIsPlaying(false);

    if (clearChannel && m_currentChannel.has_value()) {
        m_currentChannel = std::nullopt;
        emit currentChannelChanged();
    }
    if (clearChannel) {
        if (!m_currentPlaybackUrl.isEmpty()) {
            m_currentPlaybackUrl.clear();
            emit currentPlaybackUrlChanged();
        }
        m_currentLoadfileOptions.clear();
    }
    if (clearChannel && m_nowPlayingName != QStringLiteral("No channel")) {
        m_nowPlayingName = QStringLiteral("No channel");
        emit nowPlayingNameChanged();
    }
}

bool PlayerController::isSharedPlaybackPlayer(const Player::MpvPlayer *candidate) const
{
    return candidate != nullptr && m_sharedPlaybackPlayer && m_sharedPlaybackPlayer.data() == candidate;
}

bool PlayerController::usingSharedPlayback() const
{
    return m_sharedPlaybackPlayer;
}

QString PlayerController::currentPlaybackUrl() const
{
    return m_currentPlaybackUrl;
}

double PlayerController::playbackPositionSeconds() const
{
    return playbackPlayer()->position();
}

bool PlayerController::inCatchupMode() const
{
    return m_playbackMode == QStringLiteral("catchup");
}

void PlayerController::applySettings(
    const QString &mpvDllPath,
    const QMap<QString, QString> &mpvOptions,
    const double waitForDataStreamSeconds,
    const bool deinterlaceEnabled,
    const double bufferSizeSeconds,
    const QString &playerUserAgent,
    const bool remuxRecordingsToMkv)
{
    m_remuxToMkv = remuxRecordingsToMkv && Core::ffmpegToolsAvailable();
    m_waitForDataStreamSeconds = normalizedWaitForDataStreamSeconds(waitForDataStreamSeconds);
    m_player.configureLibraryPath(mpvDllPath);
    m_player.configureOptions(mpvOptions);
    m_player.configurePlaybackTuning(m_waitForDataStreamSeconds, deinterlaceEnabled, bufferSizeSeconds);
    m_player.configureUserAgent(playerUserAgent);
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Applied player settings: dll=%1 options=%2 wait=%3s deinterlace=%4 buffer=%5s user-agent=%6.")
            .arg(mpvDllPath.isEmpty() ? QStringLiteral("<bundled default>") : mpvDllPath)
            .arg(mpvOptions.size())
            .arg(m_waitForDataStreamSeconds, 0, 'f', 1)
            .arg(deinterlaceEnabled ? QStringLiteral("on") : QStringLiteral("off"))
            .arg(bufferSizeSeconds, 0, 'f', 1)
            .arg(playerUserAgent.trimmed().isEmpty() ? QStringLiteral("<default>") : playerUserAgent.trimmed()));
}

void PlayerController::startPlaybackRequest(
    Player::MpvPlayer *activePlayer,
    const QString &url,
    const bool pauseWhenReady,
    const QString &loadfileOptions)
{
    if (activePlayer == nullptr) {
        return;
    }

    m_pauseToggleRequested = false;
    m_userPausedManually = false;
    m_pauseAfterLoad = pauseWhenReady;
    stopReconnectLoop(QStringLiteral("new-play-request"));
    stopRecording();
    m_hwdecFallbackTimer.stop();
    m_hwdecFallbackApplied = false;
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        loadfileOptions.trimmed().isEmpty()
            ? QStringLiteral("Play requested: %1").arg(url)
            : QStringLiteral("Play requested: %1 (%2)").arg(url, loadfileOptions));
    m_currentLoadfileOptions = loadfileOptions.trimmed();
    m_tuneAttemptTimer.restart();
    stopStartupBufferProbe();
    stopStartupBufferFallbackWatchdog(true);
    resetBitrateAverageWindow();
    resetAdaptiveSteadyStateBufferingState();
    const auto catchupRequest = inCatchupMode();
    if (catchupRequest) {
        activePlayer->setStartupBufferingStrictMode(false);
    } else {
        activePlayer->resetSteadyStateBuffering();
        activePlayer->setStartupBufferingStrictMode(true);
    }
    stopPauseStateResync();
    setChannelLoadFailed(false);
    if (catchupRequest) {
        stopDeferredLoadingIndicator();
        setIsLoading(false);
    } else {
        beginDeferredLoadingIndicator();
    }
    m_resumePlaybackAfterLoad = !catchupRequest;
    m_backendBuffering = false;
    clearPlaybackStallTracking();
    refreshBufferingState();
    setIsPlaying(false);
    activePlayer->setPaused(catchupRequest ? pauseWhenReady : true);
    activePlayer->play(url, loadfileOptions);
}

QString PlayerController::catchupLoadfileOptions(const double streamBaseOffsetSeconds) const
{
    constexpr qint64 kCatchupDemuxerMaxBytes = 500LL * 1024LL * 1024LL;
    constexpr qint64 kCatchupDemuxerMaxBackBytes = 250LL * 1024LL * 1024LL;
    constexpr double kCatchupCacheRefillFloorSeconds = kCatchupCacheTargetSeconds - kCatchupCacheRefillMarginSeconds;
    const auto sharedOptions = QStringLiteral(
        "force-seekable=yes,hr-seek=no,cache=yes,cache-pause=no,cache-pause-wait=0,cache-secs=%1,demuxer-readahead-secs=%2,demuxer-hysteresis-secs=%3,demuxer-seekable-cache=yes,demuxer-max-bytes=%4,demuxer-max-back-bytes=%5")
                                   .arg(kCatchupCacheTargetSeconds, 0, 'f', 0)
                                   .arg(kCatchupCacheTargetSeconds, 0, 'f', 0)
                                   .arg(kCatchupCacheRefillFloorSeconds, 0, 'f', 0)
                                   .arg(kCatchupDemuxerMaxBytes)
                                   .arg(kCatchupDemuxerMaxBackBytes);
    if (!m_catchupProgramStartUtc.isValid() || !m_catchupProgramStopUtc.isValid() || m_catchupProgramStopUtc <= m_catchupProgramStartUtc) {
        return sharedOptions;
    }
    const auto baseDurationSeconds = std::max<qint64>(1, m_catchupProgramStartUtc.secsTo(m_catchupProgramStopUtc));
    const auto effectiveBaseOffset = streamBaseOffsetSeconds >= 0.0
        ? streamBaseOffsetSeconds
        : m_catchupStreamBaseOffsetSeconds;
    const auto remainingSeconds = std::max<qint64>(
        1,
        baseDurationSeconds - static_cast<qint64>(std::floor(std::max(0.0, effectiveBaseOffset))));
    const auto boundedDurationSeconds = std::max<qint64>(1, remainingSeconds + 60);
    return QStringLiteral("%1,length=%2").arg(sharedOptions).arg(boundedDurationSeconds);
}

void PlayerController::setCatchupState(const QString &liveUrl, const QString &programLabel)
{
    const auto previousMode = m_playbackMode;
    const auto previousLabel = m_catchupProgramLabel;

    m_livePlaybackUrlBeforeCatchup = liveUrl.trimmed();
    m_playbackMode = QStringLiteral("catchup");
    m_catchupProgramLabel = programLabel.trimmed();

    if (m_playbackMode != previousMode) {
        emit playbackModeChanged();
    }
    if (m_catchupProgramLabel != previousLabel) {
        emit catchupProgramLabelChanged();
    }
    syncCatchupTimelineState();
}

void PlayerController::clearCatchupState()
{
    const auto previousMode = m_playbackMode;
    const auto previousLabel = m_catchupProgramLabel;

    m_playbackMode = QStringLiteral("live");
    m_catchupProgramLabel.clear();
    m_livePlaybackUrlBeforeCatchup.clear();
    m_catchupCanonicalPlaybackUrl.clear();
    m_catchupProgramStartUtc = {};
    m_catchupProgramStopUtc = {};
    m_catchupProgramBoundaryReached = false;
    m_catchupStreamBaseOffsetSeconds = 0.0;
    m_catchupPendingStreamRelativeSeekSeconds = std::nullopt;
    m_catchupTimelineStartEpochMs = 0;
    m_catchupTimelineAvailableEdgeEpochMs = 0;
    m_catchupTimelineAvailableSeconds = 0.0;
    m_catchupTimelinePositionSeconds = 0.0;
    m_catchupTimelineAtLiveEdge = true;
    m_catchupTimelineNoticeText.clear();

    if (m_playbackMode != previousMode) {
        emit playbackModeChanged();
    }
    if (m_catchupProgramLabel != previousLabel) {
        emit catchupProgramLabelChanged();
    }
    emit catchupTimelineChanged();
    emit timeshiftStateChanged();
}

void PlayerController::syncCatchupTimelineState()
{
    if (!inCatchupMode() || !m_catchupProgramStartUtc.isValid() || !m_catchupProgramStopUtc.isValid()
        || m_catchupProgramStopUtc <= m_catchupProgramStartUtc) {
        return;
    }
    m_catchupTimelineStartEpochMs = m_catchupProgramStartUtc.toMSecsSinceEpoch();
    const auto programStop = m_catchupProgramStopUtc;
    const auto nowUtc = QDateTime::currentDateTimeUtc();
    const auto availableEdge = programStop.isValid() ? std::min(nowUtc, programStop) : nowUtc;
    m_catchupTimelineAvailableEdgeEpochMs = availableEdge.toMSecsSinceEpoch();
    const auto availableSeconds = std::max<qint64>(0, m_catchupProgramStartUtc.secsTo(availableEdge));
    m_catchupTimelineAvailableSeconds = static_cast<double>(availableSeconds);
    m_catchupTimelinePositionSeconds = std::max(0.0, std::min(m_catchupTimelineAvailableSeconds, m_catchupTimelinePositionSeconds));
    m_catchupTimelineAtLiveEdge = m_catchupTimelineAvailableSeconds <= 0.0
        || (m_catchupTimelineAvailableSeconds - m_catchupTimelinePositionSeconds) <= 0.75;
    emit catchupTimelineChanged();
    emit timeshiftStateChanged();
}

bool PlayerController::seekCatchupToTimelinePosition(const double targetSeconds)
{
    if (!inCatchupMode()) {
        return false;
    }
    const auto bounded = std::max(0.0, std::min(m_catchupTimelineAvailableSeconds, targetSeconds));
    m_catchupTimelinePositionSeconds = bounded;
    m_catchupTimelineAtLiveEdge = (m_catchupTimelineAvailableSeconds - bounded) <= 0.75;
    emit catchupTimelineChanged();
    if (playbackPlayer() == nullptr) {
        return false;
    }
    if (shouldReloadCatchupForSeek(bounded) && reloadCatchupForTimelineSeek(bounded)) {
        return true;
    }

    const auto streamRelativeTarget = std::max(0.0, bounded - m_catchupStreamBaseOffsetSeconds);
    m_catchupSeekSettleTimer.restart();
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up timeline seek: mode=absolute+keyframes target=%1s streamTarget=%2s streamBase=%3s current=%4s available=%5s host=%6")
            .arg(bounded, 0, 'f', 3)
            .arg(streamRelativeTarget, 0, 'f', 3)
            .arg(m_catchupStreamBaseOffsetSeconds, 0, 'f', 3)
            .arg(m_lastPlaybackPositionSeconds, 0, 'f', 3)
            .arg(m_catchupTimelineAvailableSeconds, 0, 'f', 3)
            .arg(QUrl(m_currentPlaybackUrl).host(QUrl::FullyDecoded)));
    playbackPlayer()->seekAbsoluteFast(streamRelativeTarget);
    return true;
}

bool PlayerController::shouldReloadCatchupForSeek(const double targetSeconds) const
{
    auto *activePlayer = playbackPlayer();
    if (!inCatchupMode() || activePlayer == nullptr || m_catchupCanonicalPlaybackUrl.trimmed().isEmpty()) {
        return false;
    }

    // Only Xtream catch-up supports deterministic URL regeneration. Non-Xtream stays native-seek only.
    if (!matchXtreamTimeshiftUrl(m_catchupCanonicalPlaybackUrl).has_value()) {
        return false;
    }

    const auto currentStreamPosition = activePlayer->position();
    if (currentStreamPosition < 0.0) {
        return true;
    }

    const auto currentTimelinePosition = m_catchupStreamBaseOffsetSeconds + currentStreamPosition;
    const auto seekableRange = activePlayer->demuxerSeekableRangeSeconds();
    if (!seekableRange.has_value()) {
        return true;
    }

    const auto cachedTimelineStart = m_catchupStreamBaseOffsetSeconds + seekableRange->first;
    const auto cachedTimelineEnd =
        m_catchupStreamBaseOffsetSeconds + seekableRange->second - kCatchupCacheSeekSafetyMarginSeconds;
    const auto backwardWindowStart = std::max(0.0, currentTimelinePosition - kCatchupBackwardSeekWindowSeconds);
    const auto effectiveSeekStart = std::max(cachedTimelineStart, backwardWindowStart);

    return targetSeconds + kPlaybackPositionEpsilon < effectiveSeekStart
        || targetSeconds > cachedTimelineEnd;
}

bool PlayerController::reloadCatchupForTimelineSeek(const double targetSeconds)
{
    auto *activePlayer = playbackPlayer();
    if (activePlayer == nullptr) {
        return false;
    }

    double streamBaseOffsetSeconds = 0.0;
    const auto regeneratedUrl = regeneratedXtreamCatchupUrl(targetSeconds, &streamBaseOffsetSeconds);
    if (regeneratedUrl.trimmed().isEmpty()) {
        return false;
    }

    if (m_reconnectActive) {
        stopReconnectLoop(QStringLiteral("catchup-seek-url-regenerate"));
    }

    const auto residualSeekSeconds = std::max(0.0, targetSeconds - streamBaseOffsetSeconds);
    const auto previousPlaybackUrl = m_currentPlaybackUrl;
    m_currentPlaybackUrl = regeneratedUrl;
    m_catchupStreamBaseOffsetSeconds = streamBaseOffsetSeconds;
    // Deliberate compromise: long seek starts from regenerated minute anchor without residual second-precision seek.
    m_catchupPendingStreamRelativeSeekSeconds = std::nullopt;
    m_catchupSeekSettleTimer.restart();
    m_resumePlaybackAfterLoad = false;
    m_pauseAfterLoad = false;
    stopDeferredLoadingIndicator();
    setIsLoading(false);
    m_backendBuffering = true;
    clearPlaybackStallTracking();
    refreshBufferingState();

    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral(
            "Catch-up timeline seek: mode=url-regenerate target=%1s streamBase=%2s residual=%3s current=%4s cachedDuration=%5s host=%6")
            .arg(targetSeconds, 0, 'f', 3)
            .arg(streamBaseOffsetSeconds, 0, 'f', 3)
            .arg(residualSeekSeconds, 0, 'f', 3)
            .arg(m_lastPlaybackPositionSeconds, 0, 'f', 3)
            .arg([activePlayer]() -> QString {
                const auto cacheDuration = activePlayer->demuxerCacheDurationSeconds();
                if (!cacheDuration.has_value()) {
                    return QStringLiteral("N/A");
                }
                return QString::number(cacheDuration.value(), 'f', 3);
            }())
            .arg(QUrl(m_currentPlaybackUrl).host(QUrl::FullyDecoded)));

    activePlayer->setStartupBufferingStrictMode(false);
    activePlayer->setPaused(false);
    activePlayer->play(regeneratedUrl, catchupLoadfileOptions(streamBaseOffsetSeconds));
    if (previousPlaybackUrl != m_currentPlaybackUrl) {
        emit currentPlaybackUrlChanged();
    }
    return true;
}

QString PlayerController::regeneratedXtreamCatchupUrl(
    const double targetSeconds,
    double *streamBaseOffsetSeconds) const
{
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

    const auto totalDurationSeconds = std::max<qint64>(1, m_catchupProgramStartUtc.secsTo(m_catchupProgramStopUtc));
    const auto remainingSeconds = std::max<qint64>(1, totalDurationSeconds - minuteOffsetSeconds);
    const auto durationMinutes = static_cast<qint64>(((remainingSeconds + 59) / 60) + 1);
    if (streamBaseOffsetSeconds != nullptr) {
        *streamBaseOffsetSeconds = static_cast<double>(minuteOffsetSeconds);
    }

    const auto suffix = match->captured(5);
    return QStringLiteral("%1%2/%3/%4%5")
        .arg(match->captured(1))
        .arg(durationMinutes)
        .arg(shiftedTimestamp)
        .arg(match->captured(4))
        .arg(suffix);
}

QString PlayerController::recoveryPlaybackUrl() const
{
    if (inCatchupMode()) {
        return m_currentPlaybackUrl.trimmed();
    }
    if (!m_currentChannel.has_value()) {
        return {};
    }
    return m_currentChannel->streamUrl.trimmed();
}

QString PlayerController::recoveryLoadfileOptions() const
{
    if (inCatchupMode()) {
        return catchupLoadfileOptions();
    }
    return m_currentLoadfileOptions.trimmed();
}

void PlayerController::playChannel(const Channel &channel)
{
    auto *activePlayer = playbackPlayer();
    if (activePlayer == nullptr) {
        return;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Channel switch requested; stopping active playback before retune."));
    setChannelSwitchInProgress(true);
    stopPauseStateResync();
    stopDeferredLoadingIndicator();
    stopStartupBufferProbe();
    stopStartupBufferFallbackWatchdog(true);
    stopReconnectLoop(QStringLiteral("channel-switch"));
    m_resumePlaybackAfterLoad = false;
    m_pauseAfterLoad = false;
    setChannelLoadFailed(false);
    setIsLoading(false);
    m_backendBuffering = false;
    clearPlaybackStallTracking();
    refreshBufferingState();
    setIsPlaying(false);
    stopRecording();
    if (m_timeshiftController) {
        m_timeshiftController->handleUserChannelSwitchRequest(channel);
    }
    activePlayer->stop();

    clearCatchupState();
    m_currentChannel = channel;
    const auto previousPlaybackUrl = m_currentPlaybackUrl;
    m_currentPlaybackUrl = channel.streamUrl;
    m_nowPlayingName = channel.name;
    emit currentChannelChanged();
    if (previousPlaybackUrl != m_currentPlaybackUrl) {
        emit currentPlaybackUrlChanged();
    }
    emit playbackChannelActivated(channel.id);
    emit nowPlayingNameChanged();
    if (m_timeshiftController && m_timeshiftController->handlePrimaryPlaybackActivation()) {
        if (!timeshiftPreparing() && !m_resumePlaybackAfterLoad) {
            setChannelSwitchInProgress(false);
        }
        return;
    }
    startPlaybackRequest(activePlayer, channel.streamUrl, false);
}

void PlayerController::playCatchupChannel(
    const Channel &channel,
    const QString &catchupUrl,
    const QString &programLabel,
    const QDateTime &programStartUtc,
    const QDateTime &programStopUtc,
    const QString &canonicalCatchupUrl)
{
    auto *activePlayer = playbackPlayer();
    if (activePlayer == nullptr) {
        return;
    }

    setChannelSwitchInProgress(true);
    stopPauseStateResync();
    stopDeferredLoadingIndicator();
    stopStartupBufferProbe();
    stopStartupBufferFallbackWatchdog(true);
    stopReconnectLoop(QStringLiteral("catchup-switch"));
    m_resumePlaybackAfterLoad = false;
    m_pauseAfterLoad = false;
    setChannelLoadFailed(false);
    setIsLoading(false);
    m_backendBuffering = false;
    clearPlaybackStallTracking();
    refreshBufferingState();
    setIsPlaying(false);
    stopRecording();
    if (m_timeshiftController) {
        m_timeshiftController->handleUserStopRequest();
    }
    activePlayer->stop();

    Core::DebugLogger::instance().log(
        QStringLiteral("catchup.play.start"),
        QStringLiteral("Switching to provider catch-up playback for %1.").arg(channel.name));
    m_catchupProgramStartUtc = programStartUtc.toUTC();
    m_catchupProgramStopUtc = programStopUtc.toUTC();
    m_catchupProgramBoundaryReached = false;
    m_catchupStreamBaseOffsetSeconds = 0.0;
    m_catchupPendingStreamRelativeSeekSeconds = std::nullopt;
    m_catchupCanonicalPlaybackUrl = canonicalCatchupUrl.trimmed().isEmpty()
        ? catchupUrl.trimmed()
        : canonicalCatchupUrl.trimmed();
    setCatchupState(channel.streamUrl, programLabel);
    m_currentChannel = channel;
    const auto previousPlaybackUrl = m_currentPlaybackUrl;
    const auto previousName = m_nowPlayingName;
    m_currentPlaybackUrl = catchupUrl;
    m_nowPlayingName = channel.name;
    emit currentChannelChanged();
    if (previousPlaybackUrl != m_currentPlaybackUrl) {
        emit currentPlaybackUrlChanged();
    }
    emit playbackChannelActivated(channel.id);
    if (m_nowPlayingName != previousName) {
        emit nowPlayingNameChanged();
    }
    startPlaybackRequest(activePlayer, catchupUrl, false, catchupLoadfileOptions());
}

void PlayerController::playCatchupChannel(const Channel &channel, const QString &catchupUrl, const QString &programLabel)
{
    const auto fallbackStart = QDateTime::currentDateTimeUtc();
    playCatchupChannel(channel, catchupUrl, programLabel, fallbackStart, fallbackStart.addSecs(60), catchupUrl);
}

void PlayerController::playCurrentPlaybackUrl(
    const QString &url,
    const bool pauseWhenReady,
    const QString &loadfileOptions)
{
    if (!m_currentChannel.has_value()) {
        return;
    }

    const auto previousPlaybackUrl = m_currentPlaybackUrl;
    m_currentPlaybackUrl = url;
    if (previousPlaybackUrl != m_currentPlaybackUrl) {
        emit currentPlaybackUrlChanged();
    }
    startPlaybackRequest(playbackPlayer(), url, pauseWhenReady, loadfileOptions);
}

void PlayerController::refreshCurrentChannelMetadata(const Channel &channel)
{
    if (!m_currentChannel.has_value()
        || m_currentChannel->id != channel.id
        || m_currentChannel->profileId != channel.profileId) {
        return;
    }

    const auto previousName = m_nowPlayingName;
    m_currentChannel = channel;
    m_nowPlayingName = channel.name;
    emit currentChannelChanged();
    if (m_nowPlayingName != previousName) {
        emit nowPlayingNameChanged();
    }
}

const std::optional<Channel> &PlayerController::currentChannelValue() const
{
    return m_currentChannel;
}

void PlayerController::returnToLiveFromCatchup()
{
    if (!inCatchupMode() || !m_currentChannel.has_value()) {
        return;
    }

    const auto liveUrl = !m_livePlaybackUrlBeforeCatchup.trimmed().isEmpty()
        ? m_livePlaybackUrlBeforeCatchup.trimmed()
        : m_currentChannel->streamUrl.trimmed();
    if (liveUrl.isEmpty()) {
        clearCatchupState();
        return;
    }

    auto *activePlayer = playbackPlayer();
    if (activePlayer == nullptr) {
        clearCatchupState();
        return;
    }

    setChannelSwitchInProgress(true);
    stopPauseStateResync();
    stopDeferredLoadingIndicator();
    stopStartupBufferProbe();
    stopStartupBufferFallbackWatchdog(true);
    stopReconnectLoop(QStringLiteral("catchup-return-live"));
    m_resumePlaybackAfterLoad = false;
    m_pauseAfterLoad = false;
    setChannelLoadFailed(false);
    setIsLoading(false);
    m_backendBuffering = false;
    clearPlaybackStallTracking();
    refreshBufferingState();
    setIsPlaying(false);
    stopRecording();
    if (m_timeshiftController) {
        m_timeshiftController->handleUserStopRequest();
    }
    activePlayer->stop();

    Core::DebugLogger::instance().log(
        QStringLiteral("catchup.play.return_live"),
        QStringLiteral("Returning to live playback for %1.").arg(m_currentChannel->name));
    clearCatchupState();
    const auto previousPlaybackUrl = m_currentPlaybackUrl;
    m_currentPlaybackUrl = liveUrl;
    if (previousPlaybackUrl != m_currentPlaybackUrl) {
        emit currentPlaybackUrlChanged();
    }
    emit playbackChannelActivated(m_currentChannel->id);
    startPlaybackRequest(activePlayer, liveUrl, false);
}

bool PlayerController::isRecording() const
{
    return m_isRecording;
}

bool PlayerController::isRemuxing() const
{
    return m_isRemuxing;
}

QString PlayerController::findFfmpegBinary()
{
    return Core::resolveProcessBinary(QStringLiteral("ffmpeg"));
}

void PlayerController::startRemux(const QString &tempPath, const QString &finalPath)
{
    m_isRemuxing = true;
    emit isRemuxingChanged();

    // Delay start: on Windows, mpv releases the stream-record file handle
    // asynchronously in its event loop (~16 ms). Without a delay, FFmpeg may
    // fail to open the file (or QFile::remove may fail after remux) because
    // mpv's write handle is still open.
    QTimer::singleShot(500, this, [this, tempPath, finalPath]() {
        if (!QFile::exists(tempPath)) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Remux skipped: temp file missing at %1").arg(tempPath));
            m_isRemuxing = false;
            emit isRemuxingChanged();
            return;
        }

        const auto ffmpeg = findFfmpegBinary();
        auto *process = new QProcess(this);
        const QStringList args = {
            QStringLiteral("-y"),
            QStringLiteral("-i"), tempPath,
            QStringLiteral("-c"), QStringLiteral("copy"),
            finalPath
        };

        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Remuxing %1 → %2").arg(tempPath, finalPath));

        connect(process, &QProcess::finished, this, [this, process, tempPath, finalPath](int exitCode, QProcess::ExitStatus) {
            const bool outputProduced = QFileInfo(finalPath).size() > 0;
            if (outputProduced) {
                Core::DebugLogger::instance().log(
                    QStringLiteral("player"),
                    QStringLiteral("Remux complete (exit %1): %2").arg(exitCode).arg(finalPath));
                // Keep spinner visible during deletion retries
                deleteTempRecording(tempPath, 5);
            } else {
                Core::DebugLogger::instance().log(
                    QStringLiteral("player"),
                    QStringLiteral("Remux failed (exit %1): keeping %2").arg(exitCode).arg(tempPath));
                m_isRemuxing = false;
                emit isRemuxingChanged();
            }
            process->deleteLater();
        });

        connect(process, &QProcess::errorOccurred, this, [this, process, tempPath](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                Core::DebugLogger::instance().log(
                    QStringLiteral("player"),
                    QStringLiteral("FFmpeg not found or failed to start — keeping raw recording at %1").arg(tempPath));
                m_isRemuxing = false;
                emit isRemuxingChanged();
                process->deleteLater();
            }
        });
        process->start(ffmpeg, args);
    });
}

void PlayerController::deleteTempRecording(const QString &path, int retriesLeft)
{
    if (QFile::remove(path)) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Temp recording deleted: %1").arg(path));
        m_isRemuxing = false;
        emit isRemuxingChanged();
        return;
    }
    if (retriesLeft <= 0) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Failed to delete temp recording after retries: %1").arg(path));
        m_isRemuxing = false;
        emit isRemuxingChanged();
        return;
    }
    // File still locked (antivirus, shell, lingering mpv handle) — retry
    QTimer::singleShot(2000, this, [this, path, retriesLeft]() {
        deleteTempRecording(path, retriesLeft - 1);
    });
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool PlayerController::takeScreenshot(const QString &outputDir, const QString &channelName, const QString &programmeName)
{
    const auto dir = outputDir.trimmed().isEmpty() ? Core::AppDataPaths::screenshotsDirectory() : outputDir;
    const auto timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const auto filename = QStringLiteral("%1_%2_%3.png")
        .arg(sanitizeForFilename(channelName), sanitizeForFilename(programmeName), timestamp);
    const auto path = QDir(dir).filePath(filename);
    QDir().mkpath(dir);
    const auto ok = playbackPlayer()->takeScreenshot(path);
    if (ok) {
        emit screenshotTaken(path);
    }
    return ok;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool PlayerController::startRecording(const QString &outputDir, const QString &channelName, const QString &programmeName)
{
    if (m_sharedPlaybackPlayer && m_sharedPlaybackProtected) {
        return false;
    }
    if (m_isRecording) {
        stopRecording();
    }
    if (!m_currentChannel.has_value()) {
        return false;
    }
    const auto dir = outputDir.trimmed().isEmpty() ? Core::AppDataPaths::recordingsDirectory() : outputDir;
    const auto timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const auto baseName = QStringLiteral("%1_%2_%3")
        .arg(sanitizeForFilename(channelName), sanitizeForFilename(programmeName), timestamp);
    QDir().mkpath(dir);
    QString recordPath;
    if (m_remuxToMkv) {
        recordPath = QDir(dir).filePath(baseName + QStringLiteral(".ts"));
        m_remuxTempPath = recordPath;
        m_remuxFinalPath = QDir(dir).filePath(baseName + QStringLiteral(".mkv"));
    } else {
        recordPath = QDir(dir).filePath(baseName + QStringLiteral(".ts"));
        m_remuxTempPath.clear();
        m_remuxFinalPath.clear();
    }
    const auto ok = playbackPlayer()->startStreamRecord(recordPath);
    if (ok) {
        m_isRecording = true;
        emit isRecordingChanged();
        emit recordingStarted(m_remuxToMkv ? m_remuxFinalPath : recordPath);
    }
    return ok;
}

void PlayerController::stopRecording()
{
    if (!m_isRecording) {
        return;
    }
    playbackPlayer()->stopStreamRecord();
    m_isRecording = false;
    emit isRecordingChanged();
    emit recordingStopped();
    if (!m_remuxTempPath.isEmpty() && !m_remuxFinalPath.isEmpty()) {
        startRemux(m_remuxTempPath, m_remuxFinalPath);
    }
    m_remuxTempPath.clear();
    m_remuxFinalPath.clear();
}

void PlayerController::shutdownForApplicationExit()
{
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Application shutdown requested. Forcing immediate playback stop."));

    if (m_sharedPlaybackPlayer && m_sharedPlaybackPlayer.data() != &m_player) {
        auto sharedPlayer = m_sharedPlaybackPlayer;
        sharedPlayer->setAudioEnabled(false);
        sharedPlayer->setVolume(0);
        sharedPlayer->stop();
    }

    if (m_sharedPlaybackPlayer) {
        m_sharedPlaybackProtected = false;
        detachSharedPlayback();
    }

    stop();
}

void PlayerController::stop()
{
    if (m_sharedPlaybackPlayer && m_sharedPlaybackProtected) {
        detachSharedPlayback();
        return;
    }
    m_pauseToggleRequested = false;
    m_userPausedManually = false;
    if (m_timeshiftController) {
        m_timeshiftController->handleUserStopRequest();
    }
    clearCatchupState();
    stopRecording();
    Core::DebugLogger::instance().log(QStringLiteral("player"), QStringLiteral("Stop requested."));
    stopStartupBufferProbe();
    stopStartupBufferFallbackWatchdog(true);
    stopReconnectLoop(QStringLiteral("stop-requested"));
    m_hwdecFallbackTimer.stop();
    m_hwdecFallbackApplied = false;
    resetBitrateAverageWindow();
    resetAdaptiveSteadyStateBufferingState();
    m_player.resetSteadyStateBuffering();
    stopPauseStateResync();
    stopDeferredLoadingIndicator();
    m_resumePlaybackAfterLoad = false;
    m_pauseAfterLoad = false;
    setChannelSwitchInProgress(false);
    setChannelLoadFailed(false);
    playbackPlayer()->stop();
    setIsLoading(false);
    m_backendBuffering = false;
    clearPlaybackStallTracking();
    refreshBufferingState();
    setIsPlaying(false);
    if (m_positionText != QStringLiteral("00:00")) {
        m_positionText = QStringLiteral("00:00");
        emit positionTextChanged();
    }
    if (m_nowPlayingName != QStringLiteral("No channel")) {
        m_nowPlayingName = QStringLiteral("No channel");
        emit nowPlayingNameChanged();
    }
    if (m_currentChannel.has_value()) {
        m_currentChannel = std::nullopt;
        emit currentChannelChanged();
    }
    if (!m_currentPlaybackUrl.isEmpty()) {
        m_currentPlaybackUrl.clear();
        emit currentPlaybackUrlChanged();
    }
    m_currentLoadfileOptions.clear();
}

void PlayerController::togglePause()
{
    if (m_sharedPlaybackPlayer && m_sharedPlaybackProtected) {
        return;
    }
    if (!inCatchupMode()
        && m_timeshiftController
        && !m_timeshiftController->isActive()
        && m_timeshiftController->handlePauseRequest()) {
        return;
    }
    Core::DebugLogger::instance().log(QStringLiteral("player"), QStringLiteral("Toggle pause requested."));
    m_pauseToggleRequested = true;
    playbackPlayer()->togglePause();
    schedulePauseStateResync();
}

void PlayerController::toggleMute()
{
    if (m_muted || m_volume <= 0.0) {
        const auto restoreVolume = m_lastNonZeroVolume > 0.0 ? m_lastNonZeroVolume : 100.0;
        setVolume(restoreVolume);
        return;
    }

    setVolume(0.0);
}

void PlayerController::seekRelative(const double seconds)
{
    if (m_sharedPlaybackPlayer && m_sharedPlaybackProtected) {
        return;
    }
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Relative seek requested: %1 seconds.").arg(seconds, 0, 'f', 1));
    playbackPlayer()->seekRelative(seconds);
}

void PlayerController::jumpToLiveEdge()
{
    if (inCatchupMode()) {
        seekCatchupToTimelinePosition(m_catchupTimelineAvailableSeconds);
        return;
    }
    if (m_timeshiftController) {
        m_timeshiftController->jumpToLiveEdge();
    }
}

void PlayerController::seekTimeshiftRelative(const double seconds)
{
    if (inCatchupMode()) {
        seekCatchupToTimelinePosition(m_catchupTimelinePositionSeconds + seconds);
        return;
    }
    if (m_timeshiftController) {
        m_timeshiftController->seekRelative(seconds);
    }
}

void PlayerController::seekTimeshiftToFraction(const double fraction)
{
    if (inCatchupMode()) {
        const auto clamped = std::max(0.0, std::min(1.0, fraction));
        seekCatchupToTimelinePosition(clamped * m_catchupTimelineAvailableSeconds);
        return;
    }
    if (m_timeshiftController) {
        m_timeshiftController->seekToFraction(fraction);
    }
}

void PlayerController::beginDeferredLoadingIndicator()
{
    m_loadingIndicatorPending = true;
    setIsLoading(false);
    m_loadingIndicatorDelayTimer.start();
}

void PlayerController::startStartupBufferFallbackWatchdog()
{
    if (!m_currentChannel.has_value() || inCatchupMode()) {
        return;
    }

    m_startupBufferFallbackAppliedForTune = false;
    m_startupBufferFallbackTargetSeconds = normalizedBufferTargetSeconds(playbackPlayer()->bufferTargetSeconds());
    auto segmentSecondsHint = 0;
    if (timeshiftActive() && m_timeshiftController) {
        segmentSecondsHint = m_timeshiftController->configuredSegmentSeconds();
    }
    const auto timeoutMs = startupBufferFallbackTimeoutMs(m_startupBufferFallbackTargetSeconds, segmentSecondsHint);
    m_startupBufferFallbackTimer.start(timeoutMs);
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Startup buffer watchdog armed: target=%1s timeout=%2ms segment-hint=%3s.")
            .arg(m_startupBufferFallbackTargetSeconds, 0, 'f', 1)
            .arg(timeoutMs)
            .arg(segmentSecondsHint > 0 ? QString::number(segmentSecondsHint) : QStringLiteral("N/A")));
}

void PlayerController::stopStartupBufferFallbackWatchdog(const bool resetTuneState)
{
    if (m_startupBufferFallbackTimer.isActive()) {
        m_startupBufferFallbackTimer.stop();
    }

    if (!resetTuneState) {
        return;
    }

    m_startupBufferFallbackAppliedForTune = false;
    m_startupBufferFallbackTargetSeconds = 0.0;
}

void PlayerController::startStartupBufferProbe()
{
    if (!m_startupBufferProbeTimer.isActive()) {
        m_startupBufferProbeTimer.start();
    }
}

void PlayerController::stopStartupBufferProbe()
{
    if (m_startupBufferProbeTimer.isActive()) {
        m_startupBufferProbeTimer.stop();
    }
}

void PlayerController::evaluateStartupBufferAndResumeIfReady()
{
    if (!m_currentChannel.has_value() || !m_resumePlaybackAfterLoad || inCatchupMode()) {
        stopStartupBufferProbe();
        return;
    }

    const auto cacheDuration = playbackPlayer()->demuxerCacheDurationSeconds();
    if (!cacheDuration.has_value() || !std::isfinite(cacheDuration.value())) {
        return;
    }

    const auto normalizedCacheDuration = std::max(0.0, cacheDuration.value());
    if (normalizedCacheDuration < m_startupBufferFallbackTargetSeconds) {
        return;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Startup buffer target reached (%1s >= %2s). Starting playback.")
            .arg(normalizedCacheDuration, 0, 'f', 2)
            .arg(m_startupBufferFallbackTargetSeconds, 0, 'f', 2));
    stopStartupBufferProbe();
    stopStartupBufferFallbackWatchdog(true);
    m_resumePlaybackAfterLoad = false;
    if (inCatchupMode()) {
        playbackPlayer()->setStartupBufferingStrictMode(false);
    }
    playbackPlayer()->setPaused(m_pauseAfterLoad);
    schedulePauseStateResync(12);
    syncIsPlayingFromBackend();
    syncIsBufferingFromBackend();
}

void PlayerController::handleStartupBufferFallbackTimeout()
{
    if (!m_currentChannel.has_value() || m_startupBufferFallbackAppliedForTune || inCatchupMode()) {
        return;
    }

    const auto backendPaused = playbackPlayer()->pauseState();
    const auto backendPlaying = backendPaused.has_value() && !backendPaused.value();
    if (m_isPlaying || backendPlaying) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Startup buffer watchdog expired after playback already started; no fallback needed (backend=%1).")
                .arg(backendPlaying ? QStringLiteral("playing") : QStringLiteral("unknown")));
        if (backendPlaying && !m_isPlaying) {
            syncIsPlayingFromBackend();
        }
        return;
    }

    const auto cacheDuration = playbackPlayer()->demuxerCacheDurationSeconds();
    const auto hasCacheDuration = cacheDuration.has_value() && std::isfinite(cacheDuration.value());
    const auto normalizedCacheDuration = hasCacheDuration ? std::max(0.0, cacheDuration.value()) : -1.0;
    if (hasCacheDuration && normalizedCacheDuration >= m_startupBufferFallbackTargetSeconds) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Startup buffer watchdog reached target naturally (%1s >= %2s); starting playback.")
                .arg(normalizedCacheDuration, 0, 'f', 2)
                .arg(m_startupBufferFallbackTargetSeconds, 0, 'f', 2));
        evaluateStartupBufferAndResumeIfReady();
        return;
    }

    const auto timeshiftActiveNow = timeshiftActive();
    const auto fallbackReason = hasCacheDuration
        ? QStringLiteral("cacheDuration=%1s < target=%2s")
              .arg(normalizedCacheDuration, 0, 'f', 2)
              .arg(m_startupBufferFallbackTargetSeconds, 0, 'f', 2)
        : QStringLiteral("cacheDuration unavailable");
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Startup buffer fallback triggered: %1. %2")
            .arg(fallbackReason)
            .arg(
                timeshiftActiveNow
                    ? QStringLiteral("Keeping strict cache-pause for timeshift startup.")
                    : QStringLiteral("Switching to best-effort startup for this tune.")));

    stopStartupBufferProbe();
    m_startupBufferFallbackAppliedForTune = true;
    m_resumePlaybackAfterLoad = false;
    if (!timeshiftActiveNow) {
        playbackPlayer()->setStartupBufferingStrictMode(false);
    }
    playbackPlayer()->setPaused(m_pauseAfterLoad);
    schedulePauseStateResync(12);
    syncIsPlayingFromBackend();
    syncIsBufferingFromBackend();
}

void PlayerController::handleHwdecFallbackCheck()
{
    if (!m_isPlaying || !m_currentChannel.has_value() || m_reconnectActive || m_hwdecFallbackApplied
        || playbackPlayer() == nullptr) {
        return;
    }
    if (inCatchupMode()
        && m_catchupSeekSettleTimer.isValid()
        && m_catchupSeekSettleTimer.elapsed() < kCatchupSeekSettleMs) {
        return;
    }

    // Only retry if there IS a video track — audio-only streams have no videoCodec
    const auto videoCodec = playbackPlayer()->videoCodec();
    if (!videoCodec.has_value()) {
        return;
    }

    // If video dimensions are already known, the decoder is producing frames — all good
    const auto videoWidth = playbackPlayer()->videoWidth();
    if (videoWidth.has_value() && videoWidth.value() > 0) {
        return;
    }

    // Video track present but no frames decoded after grace period — hwdec likely failed silently.
    // Retry with software decode.
    m_hwdecFallbackApplied = true;
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Video track present (codec=%1) but no frames produced after %2 ms. "
                        "Retrying with hwdec=no.")
            .arg(videoCodec.value())
            .arg(m_hwdecFallbackTimer.interval()));

    m_tuneAttemptTimer.restart();
    stopStartupBufferProbe();
    stopStartupBufferFallbackWatchdog(true);
    stopPauseStateResync();
    stopReconnectLoop(QStringLiteral("hwdec-fallback"));
    setChannelLoadFailed(false);
    const auto catchupRetry = inCatchupMode();
    if (catchupRetry) {
        playbackPlayer()->setStartupBufferingStrictMode(false);
        stopDeferredLoadingIndicator();
        setIsLoading(false);
    } else {
        playbackPlayer()->setStartupBufferingStrictMode(true);
        beginDeferredLoadingIndicator();
    }
    m_resumePlaybackAfterLoad = !catchupRetry;
    m_backendBuffering = false;
    clearPlaybackStallTracking();
    refreshBufferingState();
    setIsPlaying(false);
    const auto retryUrl = recoveryPlaybackUrl();
    const auto retryLoadfileOptions = recoveryLoadfileOptions();
    if (retryUrl.isEmpty()) {
        setChannelLoadFailed(true);
        setChannelSwitchInProgress(false);
        setIsLoading(false);
        emit playbackError(QStringLiteral("Channel couldn't be loaded"));
        return;
    }
    playbackPlayer()->setHwdec(QStringLiteral("no"));
    playbackPlayer()->setPaused(!catchupRetry);
    playbackPlayer()->play(retryUrl, retryLoadfileOptions);
}

void PlayerController::stopDeferredLoadingIndicator()
{
    m_loadingIndicatorPending = false;
    m_loadingIndicatorDelayTimer.stop();
}

void PlayerController::schedulePauseStateResync(const int retries)
{
    if (!m_currentChannel.has_value()) {
        return;
    }

    m_pauseStateSyncRetriesRemaining = std::max(0, retries);
    if (m_pauseStateSyncRetriesRemaining > 0) {
        m_pauseStateSyncTimer.start();
    }
}

void PlayerController::stopPauseStateResync()
{
    m_pauseStateSyncTimer.stop();
    m_pauseStateSyncRetriesRemaining = 0;
}

void PlayerController::syncIsPlayingFromBackend()
{
    if (!m_currentChannel.has_value()) {
        setIsPlaying(false);
        return;
    }

    const auto paused = playbackPlayer()->pauseState();
    if (!paused.has_value()) {
        return;
    }

    setIsPlaying(!paused.value());
    evaluateReconnectRecovery();
}

void PlayerController::syncIsBufferingFromBackend()
{
    if (!m_currentChannel.has_value()) {
        m_backendBuffering = false;
        clearPlaybackStallTracking();
        refreshBufferingState();
        return;
    }

    const auto buffering = playbackPlayer()->bufferingState();
    if (!buffering.has_value()) {
        return;
    }

    m_backendBuffering = buffering.value();
    if (m_backendBuffering) {
        clearPlaybackStallTracking();
    }
    refreshBufferingState();
    evaluateReconnectRecovery();
}

void PlayerController::clearPlaybackStallTracking()
{
    m_playbackStalled = false;
    m_lastPlaybackPositionSeconds = -1.0;
    m_stalledPlaybackTickCount = 0;
    m_noRefillConsecutiveCount = 0;
    m_videoFreezeConsecutiveCount = 0;
    m_lastObservedCacheDurationSeconds = std::nullopt;
    m_lastDisplayedVideoFramePtsSeconds = std::nullopt;
}

void PlayerController::refreshBufferingState()
{
    const auto userPaused = m_userPausedManually && !m_isPlaying;
    const auto reconnectNeedsSpinner = m_reconnectActive
        && (!m_reconnectStabilizing || m_backendBuffering || m_playbackStalled || !m_isPlaying);
    setIsBuffering(reconnectNeedsSpinner || (!userPaused && (m_backendBuffering || m_playbackStalled)));
}

void PlayerController::setIsPlaying(const bool value)
{
    if (m_isPlaying == value) {
        return;
    }

    m_isPlaying = value;
    if (m_isPlaying) {
        m_resumePlaybackAfterLoad = false;
        setChannelSwitchInProgress(false);
        stopStartupBufferProbe();
        stopStartupBufferFallbackWatchdog(false);
        stopDeferredLoadingIndicator();
        setIsLoading(false);
        if (!m_hwdecFallbackApplied) {
            m_hwdecFallbackTimer.start();
        }
    } else {
        m_hwdecFallbackTimer.stop();
    }
    emit isPlayingChanged();
}

void PlayerController::setIsLoading(const bool value)
{
    if (m_isLoading == value) {
        return;
    }

    m_isLoading = value;
    emit isLoadingChanged();
}

void PlayerController::setIsBuffering(const bool value)
{
    if (m_isBuffering == value) {
        return;
    }

    m_isBuffering = value;
    emit isBufferingChanged();
}

void PlayerController::setChannelSwitchInProgress(const bool value)
{
    if (m_channelSwitchInProgress == value) {
        return;
    }

    m_channelSwitchInProgress = value;
    emit channelSwitchInProgressChanged();
}

void PlayerController::setChannelLoadFailed(const bool value)
{
    if (m_channelLoadFailed == value) {
        return;
    }

    m_channelLoadFailed = value;
    emit channelLoadFailedChanged();
}

void PlayerController::updatePosition()
{
    auto *activePlayer = playbackPlayer();
    if (m_currentChannel.has_value()) {
        updateBitrateAverageBitsPerSecond(instantaneousBitrateBitsPerSecond(activePlayer));
    }
    struct StreamHealth
    {
        std::optional<double> cacheDurationSeconds;
        std::optional<double> cacheSpeedBytesPerSecond;
        double bufferTargetSeconds { 0.0 };
    };

    const auto sampleStreamHealth = [activePlayer]() -> StreamHealth {
        StreamHealth health;
        if (const auto cacheSpeed = activePlayer->cacheSpeedBytesPerSecond();
            cacheSpeed.has_value() && std::isfinite(cacheSpeed.value()) && cacheSpeed.value() >= 0.0) {
            health.cacheSpeedBytesPerSecond = cacheSpeed;
        }
        health.cacheDurationSeconds = activePlayer->demuxerCacheDurationSeconds();
        health.bufferTargetSeconds = activePlayer->bufferTargetSeconds();
        return health;
    };

    const auto sampleDisplayedFramePtsAdvanced = [this, activePlayer]() -> std::optional<bool> {
        const auto framePts = activePlayer->displayedVideoFramePtsSeconds();
        if (!framePts.has_value() || !std::isfinite(framePts.value())) {
            m_lastDisplayedVideoFramePtsSeconds = std::nullopt;
            return std::nullopt;
        }

        const auto normalizedPts = framePts.value();
        if (!m_lastDisplayedVideoFramePtsSeconds.has_value()) {
            m_lastDisplayedVideoFramePtsSeconds = normalizedPts;
            return std::nullopt;
        }

        const auto advanced =
            std::abs(normalizedPts - m_lastDisplayedVideoFramePtsSeconds.value()) > kVideoFramePtsEpsilonSeconds;
        m_lastDisplayedVideoFramePtsSeconds = normalizedPts;
        return advanced;
    };

    const auto maybeStartNoRefillReconnect =
        [this](const StreamHealth &health, const bool playbackAdvanced, const std::optional<bool> framePtsAdvanced) {
        if (inCatchupMode()) {
            m_noRefillConsecutiveCount = 0;
            m_lastObservedCacheDurationSeconds = std::nullopt;
            return;
        }
        if (!m_currentChannel.has_value()
            || m_channelLoadFailed
            || m_userPausedManually
            || m_resumePlaybackAfterLoad
            || m_reconnectActive) {
            m_noRefillConsecutiveCount = 0;
            m_lastObservedCacheDurationSeconds = std::nullopt;
            return;
        }

        const auto hasCacheSample = health.cacheDurationSeconds.has_value()
            && std::isfinite(health.cacheDurationSeconds.value())
            && health.cacheDurationSeconds.value() >= 0.0;
        const auto normalizedCacheDuration = hasCacheSample
            ? std::max(0.0, health.cacheDurationSeconds.value())
            : 0.0;
        const auto normalizedBufferTarget = normalizedBufferTargetSeconds(health.bufferTargetSeconds);
        const auto bufferNeedsRefill = hasCacheSample
            && std::isfinite(health.bufferTargetSeconds)
            && health.bufferTargetSeconds > 0.0
            && normalizedCacheDuration <= normalizedBufferTarget + kNoRefillCacheIncreaseEpsilonSeconds;
        const auto cacheCriticallyLow = hasCacheSample
            && normalizedCacheDuration <= kReconnectDepletedBufferThresholdSeconds;
        const auto hasCacheSpeedSample = health.cacheSpeedBytesPerSecond.has_value()
            && std::isfinite(health.cacheSpeedBytesPerSecond.value())
            && health.cacheSpeedBytesPerSecond.value() >= 0.0;
        const auto normalizedCacheSpeed = hasCacheSpeedSample
            ? std::max(0.0, health.cacheSpeedBytesPerSecond.value())
            : 0.0;
        const auto cacheSpeedRefilling = hasCacheSpeedSample
            && normalizedCacheSpeed > kNoRefillCacheSpeedThresholdBytesPerSecond;

        bool cacheRefilling = false;
        bool cacheBaselineOnly = false;
        bool cacheDraining = false;
        if (hasCacheSample) {
            if (m_lastObservedCacheDurationSeconds.has_value()) {
                cacheRefilling = normalizedCacheDuration
                    > m_lastObservedCacheDurationSeconds.value() + kNoRefillCacheIncreaseEpsilonSeconds;
                cacheDraining = normalizedCacheDuration + kNoRefillCacheIncreaseEpsilonSeconds
                    < m_lastObservedCacheDurationSeconds.value();
            } else {
                cacheBaselineOnly = true;
            }
            m_lastObservedCacheDurationSeconds = normalizedCacheDuration;
        } else {
            m_lastObservedCacheDurationSeconds = std::nullopt;
        }

        const auto frameHealthy = framePtsAdvanced.value_or(playbackAdvanced);
        if (!m_backendBuffering && !m_playbackStalled && playbackAdvanced && frameHealthy && !cacheDraining && !cacheCriticallyLow) {
            m_noRefillConsecutiveCount = 0;
            return;
        }

        if (m_reconnectWatchdogCooldownTimer.isValid()
            && m_reconnectWatchdogCooldownTimer.elapsed() < kSoftReconnectWatchdogCooldownMs) {
            m_noRefillConsecutiveCount = 0;
            return;
        }

        if (!bufferNeedsRefill && !cacheCriticallyLow) {
            m_noRefillConsecutiveCount = 0;
            return;
        }

        if (cacheSpeedRefilling || cacheRefilling) {
            m_noRefillConsecutiveCount = 0;
            return;
        }

        if (!hasCacheSample && !hasCacheSpeedSample) {
            m_noRefillConsecutiveCount = 0;
            return;
        }

        if (cacheBaselineOnly && !hasCacheSpeedSample) {
            m_noRefillConsecutiveCount = 0;
            return;
        }

        const auto starvationSignal = cacheCriticallyLow || m_backendBuffering || m_playbackStalled;
        if (!starvationSignal) {
            m_noRefillConsecutiveCount = 0;
            return;
        }

        m_noRefillConsecutiveCount += 1;
        if (playerTraceEnabled()) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "No-refill watchdog sample %1/%2: cache=%3 target=%4 cache-speed=%5B/s buffering=%6 stalled=%7 draining=%8.")
                    .arg(m_noRefillConsecutiveCount)
                    .arg(kNoRefillTickThreshold)
                    .arg(
                        hasCacheSample
                            ? QString::number(normalizedCacheDuration, 'f', 3)
                            : QStringLiteral("N/A"))
                    .arg(QString::number(normalizedBufferTarget, 'f', 3))
                    .arg(
                        hasCacheSpeedSample
                            ? QString::number(normalizedCacheSpeed, 'f', 0)
                            : QStringLiteral("N/A"))
                    .arg(m_backendBuffering ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(m_playbackStalled ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(cacheDraining ? QStringLiteral("true") : QStringLiteral("false")));
        }
        if (m_noRefillConsecutiveCount >= kNoRefillTickThreshold) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("No-refill watchdog threshold reached; triggering playback recovery."));
            if (m_timeshiftController && m_timeshiftController->isActive()) {
                if (m_timeshiftController->handlePlaybackStarvation(QStringLiteral("no-refill-watchdog"))) {
                    m_noRefillConsecutiveCount = 0;
                    m_lastObservedCacheDurationSeconds = std::nullopt;
                    return;
                }
                if (m_timeshiftController->handlePlaybackFailure(QStringLiteral("no-refill-watchdog"))) {
                    m_noRefillConsecutiveCount = 0;
                    m_lastObservedCacheDurationSeconds = std::nullopt;
                    return;
                }
            }
            startReconnectLoop(QStringLiteral("no-refill-watchdog"));
        }
    };

    const auto maybeStartVideoFreezeReconnect =
        [this, activePlayer](const StreamHealth &health, const bool playbackAdvanced, const std::optional<bool> framePtsAdvanced) {
            if (inCatchupMode()) {
                m_videoFreezeConsecutiveCount = 0;
                return;
            }
            if (!m_currentChannel.has_value()
                || m_channelLoadFailed
                || m_userPausedManually
                || m_resumePlaybackAfterLoad
                || m_reconnectActive) {
                m_videoFreezeConsecutiveCount = 0;
                return;
            }

            if (m_reconnectWatchdogCooldownTimer.isValid()
                && m_reconnectWatchdogCooldownTimer.elapsed() < kSoftReconnectWatchdogCooldownMs) {
                m_videoFreezeConsecutiveCount = 0;
                return;
            }

            const auto hasCacheSpeedSample = health.cacheSpeedBytesPerSecond.has_value()
                && std::isfinite(health.cacheSpeedBytesPerSecond.value())
                && health.cacheSpeedBytesPerSecond.value() >= 0.0;
            const auto normalizedCacheSpeed = hasCacheSpeedSample
                ? std::max(0.0, health.cacheSpeedBytesPerSecond.value())
                : 0.0;
            const auto hasCacheSample = health.cacheDurationSeconds.has_value()
                && std::isfinite(health.cacheDurationSeconds.value())
                && health.cacheDurationSeconds.value() >= 0.0;
            const auto normalizedCacheDuration =
                hasCacheSample ? std::max(0.0, health.cacheDurationSeconds.value()) : 0.0;
            const auto streamHealthyEnough = (hasCacheSpeedSample
                                              && normalizedCacheSpeed > kNoRefillCacheSpeedThresholdBytesPerSecond)
                || (hasCacheSample && normalizedCacheDuration > kReconnectDepletedBufferThresholdSeconds);
            if (!streamHealthyEnough) {
                m_videoFreezeConsecutiveCount = 0;
                return;
            }

            const auto hasVideoTrack = activePlayer->videoCodec().has_value();
            if (!hasVideoTrack) {
                m_videoFreezeConsecutiveCount = 0;
                return;
            }

            const auto frameFrozen = !framePtsAdvanced.has_value() || !framePtsAdvanced.value();
            const auto classicFreezeSignal = playbackAdvanced
                && framePtsAdvanced.has_value()
                && !framePtsAdvanced.value()
                && !m_backendBuffering
                && !m_playbackStalled;
            const auto decoderStallSignal = frameFrozen
                && (m_backendBuffering || m_playbackStalled || !playbackAdvanced);
            if (!classicFreezeSignal && !decoderStallSignal) {
                m_videoFreezeConsecutiveCount = 0;
                return;
            }

            m_videoFreezeConsecutiveCount += 1;
            const auto threshold = decoderStallSignal ? kDecoderStallTickThreshold : kVideoFreezeTickThreshold;
            const auto reason = decoderStallSignal
                ? QStringLiteral("decoder-stall-watchdog")
                : QStringLiteral("video-freeze-watchdog");
            if (playerTraceEnabled()) {
                Core::DebugLogger::instance().log(
                    QStringLiteral("player"),
                    QStringLiteral(
                        "Video-freeze watchdog sample %1/%2 (%3): cache=%4 cache-speed=%5B/s buffering=%6 stalled=%7 playback-advanced=%8 frame-frozen=%9.")
                        .arg(m_videoFreezeConsecutiveCount)
                        .arg(threshold)
                        .arg(reason)
                        .arg(
                            hasCacheSample
                                ? QString::number(normalizedCacheDuration, 'f', 3)
                                : QStringLiteral("N/A"))
                        .arg(
                            hasCacheSpeedSample
                                ? QString::number(normalizedCacheSpeed, 'f', 0)
                                : QStringLiteral("N/A"))
                        .arg(m_backendBuffering ? QStringLiteral("true") : QStringLiteral("false"))
                        .arg(m_playbackStalled ? QStringLiteral("true") : QStringLiteral("false"))
                        .arg(playbackAdvanced ? QStringLiteral("true") : QStringLiteral("false"))
                        .arg(frameFrozen ? QStringLiteral("true") : QStringLiteral("false")));
            }
            if (m_videoFreezeConsecutiveCount >= threshold) {
                Core::DebugLogger::instance().log(
                    QStringLiteral("player"),
                    QStringLiteral("Video-freeze watchdog threshold reached (%1); triggering playback recovery.")
                        .arg(reason));
                if (m_timeshiftController && m_timeshiftController->isActive()) {
                    if (m_timeshiftController->handlePlaybackStarvation(reason)) {
                        m_videoFreezeConsecutiveCount = 0;
                        return;
                    }
                    if (m_timeshiftController->handlePlaybackFailure(reason)) {
                        m_videoFreezeConsecutiveCount = 0;
                        return;
                    }
                }
                startReconnectLoop(reason);
            }
        };

    const auto updateReconnectRecoveryWindow = [this, activePlayer](const StreamHealth &health,
                                                   const bool playbackAdvanced,
                                                   const std::optional<bool> framePtsAdvanced) {
        if (!m_reconnectActive || !m_reconnectAttemptInFlight || !m_currentChannel.has_value() || m_resumePlaybackAfterLoad
            || m_channelLoadFailed) {
            m_reconnectStabilizing = false;
            m_reconnectRecoveryHealthyTickCount = 0;
            m_reconnectRecoveryUnhealthyTickCount = 0;
            m_reconnectStabilizationRefillTickCount = 0;
            m_lastReconnectStabilizationCacheDurationSeconds = std::nullopt;
            return;
        }

        if (inCatchupMode()) {
            const auto backendHealthy = !m_backendBuffering && !m_playbackStalled;
            const auto frameHealthy = framePtsAdvanced.value_or(playbackAdvanced);
            const auto stableTick = backendHealthy && playbackAdvanced && frameHealthy;
            const auto unstableTick = !backendHealthy || !frameHealthy;

            if (!m_reconnectStabilizing) {
                if (!stableTick) {
                    m_reconnectRecoveryHealthyTickCount = 0;
                    m_reconnectRecoveryUnhealthyTickCount = 0;
                    m_reconnectStabilizationRefillTickCount = 0;
                    return;
                }

                m_reconnectStabilizing = true;
                m_reconnectRecoveryHealthyTickCount = 1;
                m_reconnectRecoveryUnhealthyTickCount = 0;
                // Catch-up stabilization intentionally ignores cache-duration/cache-speed samples.
                m_reconnectStabilizationRefillTickCount = kReconnectStabilizationMinRefillTicks;
                Core::DebugLogger::instance().log(
                    QStringLiteral("player"),
                    QStringLiteral("Reconnect attempt entered catch-up stabilization phase."));
                return;
            }

            if (stableTick) {
                m_reconnectRecoveryHealthyTickCount += 1;
                m_reconnectRecoveryUnhealthyTickCount = 0;
            } else if (unstableTick) {
                m_reconnectRecoveryUnhealthyTickCount += 1;
                m_reconnectRecoveryHealthyTickCount = 0;
            } else {
                m_reconnectRecoveryHealthyTickCount = 0;
                m_reconnectRecoveryUnhealthyTickCount = 0;
            }

            if (playerTraceEnabled()) {
                Core::DebugLogger::instance().log(
                    QStringLiteral("player"),
                    QStringLiteral(
                        "Catch-up reconnect stabilization tick: stable=%1/%2 unstable=%3/%4 playback-advanced=%5 frame-healthy=%6 buffering=%7 stalled=%8.")
                        .arg(m_reconnectRecoveryHealthyTickCount)
                        .arg(kReconnectStabilizationStableTickThreshold)
                        .arg(m_reconnectRecoveryUnhealthyTickCount)
                        .arg(kReconnectStabilizationUnstableTickThreshold)
                        .arg(playbackAdvanced ? QStringLiteral("true") : QStringLiteral("false"))
                        .arg(frameHealthy ? QStringLiteral("true") : QStringLiteral("false"))
                        .arg(m_backendBuffering ? QStringLiteral("true") : QStringLiteral("false"))
                        .arg(m_playbackStalled ? QStringLiteral("true") : QStringLiteral("false")));
            }
            return;
        }

        const auto hasCacheSpeedSample = health.cacheSpeedBytesPerSecond.has_value()
            && std::isfinite(health.cacheSpeedBytesPerSecond.value())
            && health.cacheSpeedBytesPerSecond.value() >= 0.0;
        const auto normalizedCacheSpeed = hasCacheSpeedSample
            ? std::max(0.0, health.cacheSpeedBytesPerSecond.value())
            : 0.0;
        const auto hasCacheSample = health.cacheDurationSeconds.has_value()
            && std::isfinite(health.cacheDurationSeconds.value())
            && health.cacheDurationSeconds.value() >= 0.0;
        const auto normalizedCacheDuration = hasCacheSample ? std::max(0.0, health.cacheDurationSeconds.value()) : 0.0;
        const auto normalizedBufferTarget = normalizedBufferTargetSeconds(health.bufferTargetSeconds);

        bool cacheRefilling = false;
        bool cacheDraining = false;
        if (hasCacheSample) {
            if (m_lastReconnectStabilizationCacheDurationSeconds.has_value()) {
                cacheRefilling = normalizedCacheDuration
                    > m_lastReconnectStabilizationCacheDurationSeconds.value() + kNoRefillCacheIncreaseEpsilonSeconds;
                cacheDraining = normalizedCacheDuration + kNoRefillCacheIncreaseEpsilonSeconds
                    < m_lastReconnectStabilizationCacheDurationSeconds.value();
            }
            m_lastReconnectStabilizationCacheDurationSeconds = normalizedCacheDuration;
        } else {
            m_lastReconnectStabilizationCacheDurationSeconds = std::nullopt;
        }

        const auto backendHealthy = !m_backendBuffering && !m_playbackStalled;
        const auto frameHealthy = framePtsAdvanced.value_or(playbackAdvanced);
        const auto refillPositive = (hasCacheSpeedSample
                                     && normalizedCacheSpeed > kNoRefillCacheSpeedThresholdBytesPerSecond)
            || cacheRefilling;
        const auto cacheAtOrAboveTarget = hasCacheSample
            && normalizedCacheDuration + kNoRefillCacheIncreaseEpsilonSeconds >= normalizedBufferTarget;
        const auto cacheCriticallyLow = hasCacheSample
            && normalizedCacheDuration <= kReconnectDepletedBufferThresholdSeconds;

        if (!m_reconnectStabilizing) {
            const auto candidateHealthy = backendHealthy && playbackAdvanced && frameHealthy
                && (refillPositive || cacheAtOrAboveTarget);
            if (!candidateHealthy) {
                m_reconnectRecoveryHealthyTickCount = 0;
                m_reconnectRecoveryUnhealthyTickCount = 0;
                m_reconnectStabilizationRefillTickCount = 0;
                return;
            }

            m_reconnectStabilizing = true;
            m_reconnectRecoveryHealthyTickCount = 1;
            m_reconnectRecoveryUnhealthyTickCount = 0;
            m_reconnectStabilizationRefillTickCount = refillPositive ? 1 : 0;
            if (activePlayer != nullptr) {
                activePlayer->setStartupBufferingStrictMode(true);
            }
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Reconnect attempt entered stabilization phase: cache=%1 target=%2 cache-speed=%3B/s.")
                    .arg(hasCacheSample ? QString::number(normalizedCacheDuration, 'f', 3) : QStringLiteral("N/A"))
                    .arg(QString::number(normalizedBufferTarget, 'f', 3))
                    .arg(
                        hasCacheSpeedSample
                            ? QString::number(normalizedCacheSpeed, 'f', 0)
                            : QStringLiteral("N/A")));
            return;
        }

        if (refillPositive) {
            m_reconnectStabilizationRefillTickCount += 1;
        }

        const auto stableTick = backendHealthy && frameHealthy && !cacheCriticallyLow
            && (refillPositive || cacheAtOrAboveTarget);
        const auto unstableTick = cacheCriticallyLow
            || (!cacheAtOrAboveTarget && !refillPositive && (cacheDraining || m_backendBuffering || m_playbackStalled));

        if (stableTick) {
            m_reconnectRecoveryHealthyTickCount += 1;
            m_reconnectRecoveryUnhealthyTickCount = 0;
        } else if (unstableTick) {
            m_reconnectRecoveryUnhealthyTickCount += 1;
            m_reconnectRecoveryHealthyTickCount = 0;
        } else {
            m_reconnectRecoveryHealthyTickCount = 0;
            m_reconnectRecoveryUnhealthyTickCount = 0;
        }

        if (playerTraceEnabled()) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Reconnect stabilization tick: stable=%1/%2 unstable=%3/%4 refill=%5/%6 cache=%7 target=%8 cache-speed=%9B/s draining=%10 buffering=%11 stalled=%12.")
                    .arg(m_reconnectRecoveryHealthyTickCount)
                    .arg(kReconnectStabilizationStableTickThreshold)
                    .arg(m_reconnectRecoveryUnhealthyTickCount)
                    .arg(kReconnectStabilizationUnstableTickThreshold)
                    .arg(m_reconnectStabilizationRefillTickCount)
                    .arg(kReconnectStabilizationMinRefillTicks)
                    .arg(hasCacheSample ? QString::number(normalizedCacheDuration, 'f', 3) : QStringLiteral("N/A"))
                    .arg(QString::number(normalizedBufferTarget, 'f', 3))
                    .arg(
                        hasCacheSpeedSample
                            ? QString::number(normalizedCacheSpeed, 'f', 0)
                            : QStringLiteral("N/A"))
                    .arg(cacheDraining ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(m_backendBuffering ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(m_playbackStalled ? QStringLiteral("true") : QStringLiteral("false")));
        }
    };

    if (!m_isPlaying) {
        if (!m_currentChannel.has_value()) {
            clearPlaybackStallTracking();
            m_userPausedManually = false;
            refreshBufferingState();
            return;
        }

        bool playbackAdvanced = false;
        const auto seconds = activePlayer->position();
        if (seconds < 0) {
            m_stalledPlaybackTickCount += 1;
        } else {
            playbackAdvanced = m_lastPlaybackPositionSeconds < 0
                || std::abs(seconds - m_lastPlaybackPositionSeconds) > kPlaybackPositionEpsilon;
            m_lastPlaybackPositionSeconds = seconds;
            if (playbackAdvanced) {
                m_playbackStalled = false;
                m_stalledPlaybackTickCount = 0;
            } else {
                m_stalledPlaybackTickCount += 1;
            }
        }

        const auto health = sampleStreamHealth();
        maybeStartNoRefillReconnect(health, playbackAdvanced, std::nullopt);
        if (!m_reconnectActive) {
            m_reconnectRecoveryHealthyTickCount = 0;
            m_reconnectRecoveryUnhealthyTickCount = 0;
            m_reconnectStabilizing = false;
            m_reconnectStabilizationRefillTickCount = 0;
            m_lastReconnectStabilizationCacheDurationSeconds = std::nullopt;
        }
        m_videoFreezeConsecutiveCount = 0;
        m_lastDisplayedVideoFramePtsSeconds = std::nullopt;
        m_playbackStalled = !m_userPausedManually
            && m_stalledPlaybackTickCount >= kPlaybackStallTickThreshold;
        refreshBufferingState();
        return;
    }

    const auto paused = activePlayer->pauseState();
    if (paused.has_value()) {
        const auto backendPlaying = !paused.value();
        if (backendPlaying != m_isPlaying) {
            setIsPlaying(backendPlaying);
        }
    }
    const auto buffering = activePlayer->bufferingState();
    if (buffering.has_value()) {
        m_backendBuffering = buffering.value();
    }

    bool playbackAdvanced = false;
    const auto seconds = activePlayer->position();
    if (seconds < 0) {
        m_stalledPlaybackTickCount += 1;
        const auto health = sampleStreamHealth();
        maybeStartNoRefillReconnect(health, false, std::nullopt);
        // Decoder/freeze watchdog requires a valid playback clock signal.
        // Streams that do not expose time-pos yet must not be classified as decoder stalls.
        m_videoFreezeConsecutiveCount = 0;
        if (!m_reconnectActive) {
            m_reconnectRecoveryHealthyTickCount = 0;
            m_reconnectRecoveryUnhealthyTickCount = 0;
            m_reconnectStabilizing = false;
            m_reconnectStabilizationRefillTickCount = 0;
            m_lastReconnectStabilizationCacheDurationSeconds = std::nullopt;
        }
        m_lastDisplayedVideoFramePtsSeconds = std::nullopt;
        refreshBufferingState();
        return;
    }

    playbackAdvanced = m_lastPlaybackPositionSeconds < 0
        || std::abs(seconds - m_lastPlaybackPositionSeconds) > kPlaybackPositionEpsilon;
    m_lastPlaybackPositionSeconds = seconds;
    if (playbackAdvanced) {
        if (inCatchupMode()
            && m_catchupSeekSettleTimer.isValid()
            && m_catchupSeekSettleTimer.elapsed() >= 0
            && m_catchupSeekSettleTimer.elapsed() < kCatchupSeekSettleMs) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Catch-up seek settled: elapsedMs=%1 host=%2 streamPosition=%3s timelinePosition=%4s")
                    .arg(m_catchupSeekSettleTimer.elapsed())
                    .arg(QUrl(m_currentPlaybackUrl).host(QUrl::FullyDecoded))
                    .arg(seconds, 0, 'f', 3)
                    .arg(m_catchupStreamBaseOffsetSeconds + seconds, 0, 'f', 3));
            m_catchupSeekSettleTimer.invalidate();
        }
        if (m_playbackStalled) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Playback position advanced again; clearing stall indicator."));
        }
        m_playbackStalled = false;
        m_stalledPlaybackTickCount = 0;
    } else {
        m_stalledPlaybackTickCount += 1;
        if (m_stalledPlaybackTickCount >= kPlaybackStallTickThreshold) {
            if (!m_playbackStalled) {
                Core::DebugLogger::instance().log(
                    QStringLiteral("player"),
                    QStringLiteral(
                        "Playback position stalled for %1 ticks (backendBuffering=%2); surfacing buffering spinner.")
                        .arg(m_stalledPlaybackTickCount)
                        .arg(m_backendBuffering ? QStringLiteral("true") : QStringLiteral("false")));
            }
            m_playbackStalled = true;
        }
    }
    const auto framePtsAdvanced = sampleDisplayedFramePtsAdvanced();
    const auto health = sampleStreamHealth();
    maybeRetuneSteadyStateBuffering(health.cacheDurationSeconds, health.bufferTargetSeconds);
    maybeStartNoRefillReconnect(health, playbackAdvanced, framePtsAdvanced);
    maybeStartVideoFreezeReconnect(health, playbackAdvanced, framePtsAdvanced);
    updateReconnectRecoveryWindow(health, playbackAdvanced, framePtsAdvanced);
    refreshBufferingState();
    evaluateReconnectRecovery();

    if (inCatchupMode()) {
        m_catchupTimelinePositionSeconds = std::max(
            0.0,
            std::min(m_catchupTimelineAvailableSeconds, m_catchupStreamBaseOffsetSeconds + seconds));
        syncCatchupTimelineState();
    }

    const auto displaySeconds = inCatchupMode() ? m_catchupTimelinePositionSeconds : seconds;
    const auto duration = QTime(0, 0).addSecs(static_cast<int>(displaySeconds));
    const auto formatted =
        displaySeconds >= 3600.0 ? duration.toString(QStringLiteral("h:mm:ss")) : duration.toString(QStringLiteral("mm:ss"));
    if (formatted != m_positionText) {
        m_positionText = formatted;
        emit positionTextChanged();
    }
}

} // namespace OKILTV::App
