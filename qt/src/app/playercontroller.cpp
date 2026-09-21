#include "playercontroller.h"

#include "timeshiftcontroller.h"

#include "../core/appdatapaths.h"
#include "../core/catchupurlresolver.h"
#include "../core/debuglogger.h"
#include "../core/processutils.h"
#include "../core/redaction.h"
#include "../core/settingsmanager.h"
#include "../core/trackpreferences.h"

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

bool envFlagEnabled(const char *name)
{
    const auto value = qEnvironmentVariable(name).trimmed().toLower();
    return value == QStringLiteral("1")
        || value == QStringLiteral("true")
        || value == QStringLiteral("yes")
        || value == QStringLiteral("on");
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

constexpr double kPlaybackPositionEpsilon = 0.05;
constexpr int kLoadingIndicatorDelayMs = 1500;
constexpr int kReconnectAttemptIntervalMs = 1000;
constexpr int kReconnectMaxAttempts = 5;
constexpr int kSoftReconnectWatchdogCooldownMs = 15000;
constexpr int kCatchupSeekSettleMs = 3000;
constexpr double kMinimumBufferSeconds = 0.1;
constexpr double kMaximumBufferSeconds = 60.0;
constexpr auto kPlaybackSignalsConnectedProperty = "_okiltvPlaybackSignalsConnected";
constexpr double kCatchupActiveCacheHeadSeconds = 90.0;
constexpr double kCatchupDebugEffectiveCacheMaxSeconds = kCatchupActiveCacheHeadSeconds;
constexpr int kCatchupTimelineReloadAckTimeoutMs = 500;
constexpr int kCatchupTimelineNoticeAutoClearMs = 5000;
constexpr double kCatchupRollingOverlapBiasSeconds = 2.0;
constexpr double kCatchupRollingPredictiveTriggerSeconds = 45.0;
constexpr int kCatchupRollingRetryWindowMs = 10000;
constexpr int kCatchupRollingMaxRetries = 3;
constexpr int kCatchupSeamlessFallbackTimeoutMs = 2500;
constexpr int kCatchupSeamlessStandbyStopAckTimeoutMs = 500;
constexpr int kCatchupSeamlessStandbyVideoReadyTimeoutMs = 400;
constexpr int kCatchupSeamlessStandbyFastRetryWindowMs = 1200;
constexpr int kCatchupSeamlessPostCloseDelayMs = 750;
constexpr double kCatchupSeamlessCutoverRemainingSeconds = 0.35;

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

QString formatOwnedQueueBytes(const qsizetype bytes)
{
    return QStringLiteral("%1 MiB")
        .arg(static_cast<double>(std::max<qsizetype>(0, bytes)) / (1024.0 * 1024.0), 0, 'f', 1);
}

}

PlayerController::PlayerController(QObject *parent)
    : QObject(parent)
{
    m_loadingIndicatorDelayTimer.setInterval(kLoadingIndicatorDelayMs);
    m_loadingIndicatorDelayTimer.setSingleShot(true);
    connect(&m_loadingIndicatorDelayTimer, &QTimer::timeout, this, [this]() {
        if (!m_loadingIndicatorPending || !m_currentChannel.has_value() || m_userPausedManually) {
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
            refreshBufferingState();
            stopPauseStateResync();
            return;
        }

        m_pauseStateSyncRetriesRemaining -= 1;
        if (m_pauseStateSyncRetriesRemaining > 0) {
            m_pauseStateSyncTimer.start();
        }
    });
    connect(&m_buffering.startupFallbackTimer(), &QTimer::timeout, this, &PlayerController::handleStartupBufferFallbackTimeout);
    connect(&m_buffering.startupProbeTimer(), &QTimer::timeout, this, &PlayerController::evaluateStartupBufferAndResumeIfReady);
    m_recovery.attemptTimer().setInterval(reconnectAttemptIntervalMs());
    connect(&m_recovery.attemptTimer(), &QTimer::timeout, this, &PlayerController::handleReconnectAttemptTick);
    connect(&m_catchupSession.reloadAckTimer(), &QTimer::timeout, this, [this]() {
        if (!m_catchupSession.reloadInFlight()) {
            return;
        }
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up timeline reload teardown ack timed out after %1ms; proceeding with reload.")
                .arg(kCatchupTimelineReloadAckTimeoutMs));
        runCatchupTimelineReload();
        finishCatchupTimelineReload(QStringLiteral("timeout"));
    });
    m_catchupTimelineNoticeClearTimer.setSingleShot(true);
    connect(&m_catchupTimelineNoticeClearTimer, &QTimer::timeout, this, [this]() {
        if (m_catchupTimelineNoticeText.isEmpty()
            || m_catchupTimelineNoticeText != m_catchupTimelineNoticeAutoClearText) {
            return;
        }
        m_catchupTimelineNoticeText.clear();
        m_catchupTimelineNoticeAutoClearText.clear();
        emit catchupTimelineChanged();
    });
    connect(&m_catchupSession.standby().fallbackTimer(), &QTimer::timeout, this, [this]() {
        if (!m_catchupSession.standby().pending() || !m_catchupSession.standby().fallbackDeferred() || !inCatchupMode()) {
            return;
        }
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Catch-up seamless extension standby timeout after %1ms; falling back to standard recovery.")
                .arg(kCatchupSeamlessFallbackTimeoutMs));
        // Clear the pending attempt before recovery, otherwise it re-arms this
        // timer indefinitely when standby cannot cover the watched position.
        abortSeamlessCatchupRolling(QStringLiteral("standby-fallback-timeout"), true);
        if (handleCatchupPlaybackEndedRecovery()) {
            return;
        }
        startReconnectLoop(QStringLiteral("catchup-seamless-fallback-timeout"));
        refreshBufferingState();
    });
    connect(&m_catchupSession.standby().stopAckTimer(), &QTimer::timeout, this, [this]() {
        if (!m_catchupSession.standby().pending() || !m_catchupSession.standby().stopPending() || m_catchupSession.standby().loadIssued()) {
            return;
        }
        auto *standbyPlayer = m_catchupSeamlessStandbyPlayer.data();
        if (standbyPlayer == nullptr || standbyPlayer == playbackPlayer()) {
            m_catchupSession.standby().acknowledgeStop();
            return;
        }
        m_catchupSession.standby().acknowledgeStop();
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Catch-up seamless standby stop ack timed out after %1ms; proceeding with standby load.")
                .arg(kCatchupSeamlessStandbyStopAckTimeoutMs));
        launchSeamlessCatchupStandbyLoad(standbyPlayer);
    });
    connect(&m_catchupSession.standby().videoReadyTimer(), &QTimer::timeout, this, [this]() {
        if (!m_catchupSession.standby().pending() || !m_catchupSession.standby().loadIssued() || m_catchupSession.standby().videoReady()) {
            return;
        }
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Catch-up seamless standby video prewarm timed out after %1ms; falling back to hard restore.")
                .arg(kCatchupSeamlessStandbyVideoReadyTimeoutMs));
        abortSeamlessCatchupRolling(QStringLiteral("standby-video-prewarm-timeout"), true);
        hardRestoreCatchupAtCurrentTimelinePoint(QStringLiteral("standby-video-prewarm-timeout"));
    });
    connect(&m_catchupSession.standby().retryTimer(), &QTimer::timeout, this, [this]() {
        if (!m_catchupSession.standby().pending() || m_catchupSession.standby().loadIssued() || !canUseSeamlessCatchupRolling()) {
            return;
        }
        if (!m_catchupSession.standby().retryPending() || m_catchupSession.standby().retryBudget() <= 0) {
            return;
        }
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up seamless standby fast retry timer fired; attempting immediate retry."));
        startSeamlessCatchupStandbyLoad();
    });
    connect(&m_catchupSession.standby().delayTimer(), &QTimer::timeout, this, [this]() {
        if (!m_catchupSession.standby().pending() || !m_catchupSession.standby().delayPending() || !canUseSeamlessCatchupRolling()) {
            return;
        }
        m_catchupSession.standby().delayElapsed();
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Catch-up seamless standby delay elapsed (%1ms); starting standby warmup after EOF/provider-close handshake.")
                .arg(kCatchupSeamlessPostCloseDelayMs));
        startSeamlessCatchupStandbyLoad();
    });
    m_hwdecFallbackTimer.setSingleShot(true);
    m_hwdecFallbackTimer.setInterval(5000);
    connect(&m_hwdecFallbackTimer, &QTimer::timeout, this, &PlayerController::handleHwdecFallbackCheck);
    ensurePlaybackSignalConnections(&m_player);
    ensurePlaybackSignalConnections(&m_catchupStandbyPlayer);
    const auto bindSeamlessStandbyLifecycle = [this](Player::MpvPlayer *observedPlayer) {
        connect(observedPlayer, &Player::MpvPlayer::fileLoaded, this, [this, observedPlayer]() {
            if (!m_catchupSession.standby().pending() || !m_catchupSession.standby().loadIssued()) {
                return;
            }
            if (m_catchupSeamlessStandbyPlayer.data() != observedPlayer) {
                return;
            }
            observedPlayer->setStartupBufferingStrictMode(false);
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Catch-up seamless standby file-loaded."));
        });
        connect(observedPlayer, &Player::MpvPlayer::playbackRestarted, this, [this, observedPlayer]() {
            if (!m_catchupSession.standby().pending() || !m_catchupSession.standby().loadIssued()) {
                return;
            }
            if (m_catchupSeamlessStandbyPlayer.data() != observedPlayer) {
                return;
            }
            if (!standbyCatchupSessionHealthyForCutover()) {
                markSeamlessStandbyAttemptFailed(QStringLiteral("standby owned stream became unavailable before readiness"));
                return;
            }
            m_catchupSession.standby().markReady();
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Catch-up seamless standby playback restarted; standby ready."));
            if (!m_catchupSession.standby().videoReady() && !m_catchupSession.standby().videoReadyTimer().isActive()) {
                m_catchupSession.standby().videoReadyTimer().start();
            }
            maybeCommitSeamlessCatchupCutover(QStringLiteral("standby-ready"));
        });
        connect(observedPlayer, &Player::MpvPlayer::videoReconfigured, this, [this, observedPlayer]() {
            if (!m_catchupSession.standby().pending() || !m_catchupSession.standby().loadIssued()) {
                return;
            }
            if (m_catchupSeamlessStandbyPlayer.data() != observedPlayer) {
                return;
            }
            if (!standbyCatchupSessionHealthyForCutover()) {
                markSeamlessStandbyAttemptFailed(QStringLiteral("standby owned stream became unavailable before video-ready"));
                return;
            }
            m_catchupSession.standby().markVideoReady();
            m_catchupSession.standby().videoReadyTimer().stop();
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Catch-up seamless standby video reconfigured; standby video ready."));
            maybeCommitSeamlessCatchupCutover(QStringLiteral("standby-video-ready"));
        });
        connect(observedPlayer, &Player::MpvPlayer::playbackStopped, this, [this, observedPlayer]() {
            if (!m_catchupSession.standby().pending() || !m_catchupSession.standby().stopPending() || m_catchupSession.standby().loadIssued()) {
                return;
            }
            auto *standbyPlayer = m_catchupSeamlessStandbyPlayer.data();
            if (standbyPlayer == nullptr || standbyPlayer != observedPlayer || standbyPlayer == playbackPlayer()) {
                m_catchupSession.standby().acknowledgeStop();
                return;
            }
            m_catchupSession.standby().stopAckTimer().stop();
            m_catchupSession.standby().acknowledgeStop();
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Catch-up seamless standby stop acknowledged; starting standby load."));
            launchSeamlessCatchupStandbyLoad(standbyPlayer);
        });
        connect(observedPlayer, &Player::MpvPlayer::errorOccurred, this, [this, observedPlayer](const QString &message) {
            if (!m_catchupSession.standby().pending() || !m_catchupSession.standby().loadIssued()) {
                return;
            }
            if (m_catchupSeamlessStandbyPlayer.data() != observedPlayer) {
                return;
            }
            markSeamlessStandbyAttemptFailed(message);
        });
    };
    bindSeamlessStandbyLifecycle(&m_player);
    bindSeamlessStandbyLifecycle(&m_catchupStandbyPlayer);

    m_liveDeliveryClock.start();
    m_liveDeliveryTimer.setInterval(250);
    connect(&m_liveDeliveryTimer, &QTimer::timeout, this, &PlayerController::sampleLiveDelivery);
    m_liveDeliveryTimer.start();

    m_positionTimer.setInterval(1000);
    connect(&m_positionTimer, &QTimer::timeout, this, &PlayerController::updatePosition);
    m_positionTimer.start();
}

void PlayerController::setTrackPreferenceSettings(Core::SettingsManager *settings)
{
    m_trackPreferenceSettings = settings;
}

void PlayerController::configurePlaybackTrackPreferences(Player::MpvPlayer *player, const bool discardMissing)
{
    if (!m_trackPreferenceSettings || !m_currentChannel || !player) {
        return;
    }
    const auto profileId = Core::guidToString(m_currentChannel->profileId);
    const auto channelKey = Core::trackPreferenceChannelKey(*m_currentChannel);
    player->configureTrackPreferences(profileId, channelKey,
        m_trackPreferenceSettings->channelTrackPreferences(profileId, channelKey),
        discardMissing && !timeshiftActive() && !timeshiftPreparing());
}

void PlayerController::ensurePlaybackSignalConnections(Player::MpvPlayer *player)
{
    if (player == nullptr || player->property(kPlaybackSignalsConnectedProperty).toBool()) {
        return;
    }

    player->setProperty(kPlaybackSignalsConnectedProperty, true);
    connect(player, &Player::MpvPlayer::trackPreferenceChanged, this,
        [this](const QString &profileId, const QString &channelKey, const QString &type, const QJsonObject &preference) {
            if (m_trackPreferenceSettings) {
                m_trackPreferenceSettings->setChannelTrackPreference(profileId, channelKey, type, preference);
            }
        });
    connect(player, &Player::MpvPlayer::liveMediaPeriodChanged, this, [this, player]() {
        if (playbackPlayer() != player) {
            return;
        }
        clearPlaybackStallTracking();
        clearLiveBufferState();
        resetBitrateAverageWindow();
        resetAdaptiveSteadyStateBufferingState();
    });
    connect(player, &Player::MpvPlayer::playbackRestarted, this, [this, player]() {
        if (playbackPlayer() != player || !m_loadingIndicatorPending
            || !m_loadingPlaybackFileLoaded) {
            return;
        }
        // An unpaused backend is not evidence that the new stream is ready.
        // PLAYBACK_RESTART confirms completion of loading/seeking, also for audio-only streams.
        m_loadingPlaybackReady = true;
        refreshBufferingState();
    });
    connect(player, &Player::MpvPlayer::fileLoaded, this, [this, player]() {
        if (playbackPlayer() != player) {
            return;
        }

        m_loadingPlaybackFileLoaded = true;
        Core::DebugLogger::instance().log(QStringLiteral("player"), QStringLiteral("mpv signaled file-loaded."));
        if (m_recovery.active()) {
            m_recovery.loaded(m_liveDeliveryClock.elapsed());
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Reconnect attempt reached file-loaded; waiting for actual playback recovery."));
        }
        setChannelLoadFailed(false);
        if (m_buffering.startupPending()) {
            startStartupBufferFallbackWatchdog();
            startStartupBufferProbe();
            evaluateStartupBufferAndResumeIfReady();
        }
        if (inCatchupMode()) {
            playbackPlayer()->setStartupBufferingStrictMode(false);
            m_catchupSession.setProgressTransportReady(true);
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Catch-up file-loaded: disabled strict startup cache-pause for responsive seeks."));
            m_catchupSession.setActiveEofObserved(false);
            if (m_catchupSession.reconnectResumeStreamRelativeSeconds().has_value()) {
                const auto resumeTarget = std::max(0.0, m_catchupSession.reconnectResumeStreamRelativeSeconds().value());
                if (m_catchupSession.endless() || m_catchupSession.continuousFallback()) {
                    m_catchupSession.setAlignmentActive(true);
                    m_catchupSession.resetAlignmentSeek();
                    playbackPlayer()->setPaused(true);
                    refreshBufferingState();
                } else {
                    m_catchupSession.setReconnectResumeStreamRelativeSeconds({});
                    m_catchupSession.setProgressSeekTargetSeconds(m_catchupSession.streamBaseOffsetSeconds() + resumeTarget);
                    m_catchupSession.seekStarted(m_liveDeliveryClock.elapsed());
                    playbackPlayer()->seekAbsoluteFast(resumeTarget);
                }
                Core::DebugLogger::instance().log(
                    QStringLiteral("player"),
                    QStringLiteral("Catch-up reconnect loaded; restoring stream-relative position to %1s.")
                        .arg(resumeTarget, 0, 'f', 3));
            }
            if (m_catchupSession.pendingStreamRelativeSeekSeconds().has_value()) {
                const auto residualSeekSeconds = std::max(0.0, m_catchupSession.pendingStreamRelativeSeekSeconds().value());
                m_catchupSession.setPendingStreamRelativeSeekSeconds(std::nullopt);
                if (residualSeekSeconds > kPlaybackPositionEpsilon) {
                    Core::DebugLogger::instance().log(
                        QStringLiteral("player"),
                        QStringLiteral(
                            "Catch-up regenerated URL loaded; skipping residual stream seek (%1s) and starting from minute anchor.")
                            .arg(residualSeekSeconds, 0, 'f', 3));
                }
            }
            if (m_catchupSession.pendingInitialSeekSeconds().has_value()) {
                const auto requestedSeekSeconds = std::max(0.0, m_catchupSession.pendingInitialSeekSeconds().value());
                m_catchupSession.setPendingInitialSeekSeconds(std::nullopt);
                if (requestedSeekSeconds > kPlaybackPositionEpsilon) {
                    seekCatchupToTimelinePosition(requestedSeekSeconds);
                }
            }
            m_catchupSession.resetRolling(false);
            m_catchupSession.setProgramBoundaryReached(false);
            syncCatchupTimelineState();
            m_catchupSession.setTransportEndTimelineSeconds(std::max(m_catchupSession.streamBaseOffsetSeconds(), m_catchupSession.timelineAvailableSeconds()));
            setCatchupTimelineNoticeText(QString {});
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
            m_userPausedManually = m_userPausedManually || manualToggleRequested;
        } else {
            m_userPausedManually = false;
        }

        if (paused) {
            clearPlaybackStallTracking();
            refreshBufferingState();
        }
        stopPauseStateResync();
        setIsPlaying(!paused);
        refreshBufferingState();
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
    connect(player, &Player::MpvPlayer::playbackStopped, this, [this, player]() {
        if (playbackPlayer() != player) {
            return;
        }
        if (!m_catchupSession.reloadInFlight()) {
            return;
        }
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up timeline reload teardown acknowledged by mpv stop event."));
        runCatchupTimelineReload();
        finishCatchupTimelineReload(QStringLiteral("stop-ack"));
    });
    connect(player, &Player::MpvPlayer::playbackEnded, this, [this, player]() {
        if (playbackPlayer() != player) {
            return;
        }

        Core::DebugLogger::instance().log(QStringLiteral("player"), QStringLiteral("mpv signaled playback ended."));
        clearReconnectAttemptInFlight(QStringLiteral("playback-ended"));
        stopStartupBufferProbe();
        stopStartupBufferFallbackWatchdog(true);
        if (m_buffering.startupPending()) {
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
            m_buffering.setStartupPending(false);
            setChannelLoadFailed(false);
            setIsLoading(false);
            m_backendBuffering = false;
            clearPlaybackStallTracking();
            setIsPlaying(false);
            if (inCatchupMode()) {
                if (handleCatchupPlaybackEndedRecovery()) {
                    return;
                }
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
        m_buffering.setStartupPending(false);
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
        if (m_buffering.startupPending() && m_currentChannel.has_value()) {
            recoverPendingPlaybackLoadFailure(
                QStringLiteral("pending-mpv-error"),
                QStringLiteral("mpv error while tune was pending: %1. Starting recovery.").arg(message));
            return;
        }
        if (!m_buffering.startupPending() && m_currentChannel.has_value()) {
            stopPauseStateResync();
            stopDeferredLoadingIndicator();
            m_buffering.setStartupPending(false);
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
        m_buffering.setStartupPending(false);
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

void PlayerController::copyVolumeStateFrom(const PlayerController &other)
{
    m_lastNonZeroVolume = other.m_lastNonZeroVolume;
    setVolume(other.volume());
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

void PlayerController::setDateTimeFormat(const Core::DateTimeFormatOptions options)
{
    if (options == m_dateTimeFormat) {
        return;
    }
    m_dateTimeFormat = options;
    emit catchupTimelineChanged();
    emit catchupProgramLabelChanged();
}

QString PlayerController::catchupProgramLabel() const
{
    return m_catchupSession.programLabel();
}

QVariantMap PlayerController::catchupCurrentProgram() const
{
    return m_catchupSession.catchupCurrentProgram(inCatchupMode(), m_dateTimeFormat);
}

bool PlayerController::catchupTimelineActive() const
{
    return m_catchupSession.catchupTimelineActive(inCatchupMode());
}

qint64 PlayerController::catchupTimelineStartEpochMs() const
{
    return m_catchupSession.catchupTimelineStartEpochMs();
}

qint64 PlayerController::catchupTimelineAvailableEdgeEpochMs() const
{
    return m_catchupSession.timelineAvailableEdgeEpochMs();
}

double PlayerController::catchupTimelineAvailableSeconds() const
{
    return m_catchupSession.timelineAvailableSeconds();
}

double PlayerController::catchupTimelinePositionSeconds() const
{
    return std::max(0.0, m_catchupSession.timelinePositionSeconds()
        - static_cast<double>(catchupTimelineStartEpochMs() - m_catchupSession.timelineStartEpochMs()) / 1000.0);
}

bool PlayerController::catchupTimelineAtLiveEdge() const
{
    return m_catchupSession.timelineAtLiveEdge();
}

QString PlayerController::catchupTimelineNoticeText() const
{
    return m_catchupTimelineNoticeText;
}

void PlayerController::setCatchupTimelineNoticeText(const QString &noticeText, const int autoClearMs)
{
    const auto normalized = noticeText.trimmed();
    if (!normalized.isEmpty() && autoClearMs > 0) {
        m_catchupTimelineNoticeAutoClearText = normalized;
        m_catchupTimelineNoticeClearTimer.start(std::max(1, autoClearMs));
    } else {
        m_catchupTimelineNoticeClearTimer.stop();
        m_catchupTimelineNoticeAutoClearText.clear();
    }

    if (m_catchupTimelineNoticeText == normalized) {
        return;
    }

    m_catchupTimelineNoticeText = normalized;
    emit catchupTimelineChanged();
}

bool PlayerController::liveBufferActive() const
{
    return m_liveBufferActive;
}

qint64 PlayerController::liveBufferWindowStartEpochMs() const
{
    return m_liveBufferWindowStartEpochMs;
}

qint64 PlayerController::liveBufferLiveEdgeEpochMs() const
{
    return m_liveBufferLiveEdgeEpochMs;
}

double PlayerController::liveBufferAvailableSeconds() const
{
    return m_liveBufferAvailableSeconds;
}

double PlayerController::liveBufferPositionSeconds() const
{
    return m_liveBufferPositionSeconds;
}

double PlayerController::liveBufferBehindLiveSeconds() const
{
    return m_liveBufferBehindLiveSeconds;
}

bool PlayerController::liveBufferAtLiveEdge() const
{
    return m_liveBufferAtLiveEdge;
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

    const auto minBufferNeeded = inCatchupMode() || timeshiftActiveNow
        ? activePlayer->bufferTargetSeconds() : effectiveLiveBufferTargetSeconds();
    const auto minBufferNeededSeconds = std::isfinite(minBufferNeeded) && minBufferNeeded > 0.0 ? minBufferNeeded : 0.0;

    const auto averageBitrateBitsPerSecond = m_buffering.averageBitrate().has_value()
        ? m_buffering.averageBitrate()
        : instantaneousBitrateBitsPerSecond(activePlayer);
    const auto bitrateText = averageBitrateBitsPerSecond.has_value()
        ? formatDebugBitrate(averageBitrateBitsPerSecond.value())
        : QStringLiteral("N/A");
    const auto liveReserveTargetSeconds = inCatchupMode() ? activePlayer->bufferTargetSeconds() : effectiveLiveBufferTargetSeconds();
    const auto liveForwardTargetSeconds = adaptiveSteadyStateCacheLimitSeconds(liveReserveTargetSeconds);
    const auto liveBackTargetSeconds = Player::MpvPlayer::steadyStateBackBufferSeconds();
    const auto liveMaxBytes = adaptiveSteadyStateMaxBytes(liveReserveTargetSeconds, averageBitrateBitsPerSecond);
    const auto liveMaxBackBytes = adaptiveSteadyStateMaxBackBytes(averageBitrateBitsPerSecond);
    const auto activeOwnedQueueBytes = m_catchupActiveStreamSession ? m_catchupActiveStreamSession->bufferedBytes() : 0;
    const auto standbyOwnedQueueBytes = m_catchupStandbyStreamSession ? m_catchupStandbyStreamSession->bufferedBytes() : 0;
    const auto activeOwnedQueuePeakBytes = m_catchupActiveStreamSession ? m_catchupActiveStreamSession->peakBufferedBytes() : 0;
    const auto standbyOwnedQueuePeakBytes = m_catchupStandbyStreamSession ? m_catchupStandbyStreamSession->peakBufferedBytes() : 0;
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
        { QStringLiteral("liveForwardTargetSeconds"), liveForwardTargetSeconds },
        { QStringLiteral("liveConfiguredTargetSeconds"), activePlayer->bufferTargetSeconds() },
        { QStringLiteral("liveDeliveryIntervalSeconds"), m_buffering.detectedIntervalSeconds() },
        { QStringLiteral("liveForwardTargetText"), formatDebugBufferDuration(liveForwardTargetSeconds) },
        { QStringLiteral("liveBackTargetSeconds"), liveBackTargetSeconds },
        { QStringLiteral("liveBackTargetText"), formatDebugBufferDuration(liveBackTargetSeconds) },
        { QStringLiteral("liveMaxBytes"), liveMaxBytes },
        { QStringLiteral("liveMaxBackBytes"), liveMaxBackBytes },
        { QStringLiteral("catchupOwnedQueueActiveBytes"), activeOwnedQueueBytes },
        { QStringLiteral("catchupOwnedQueueActiveText"), formatOwnedQueueBytes(activeOwnedQueueBytes) },
        { QStringLiteral("catchupOwnedQueueActivePeakBytes"), activeOwnedQueuePeakBytes },
        { QStringLiteral("catchupOwnedQueueActivePeakText"), formatOwnedQueueBytes(activeOwnedQueuePeakBytes) },
        { QStringLiteral("catchupOwnedQueueStandbyBytes"), standbyOwnedQueueBytes },
        { QStringLiteral("catchupOwnedQueueStandbyText"), formatOwnedQueueBytes(standbyOwnedQueueBytes) },
        { QStringLiteral("catchupOwnedQueueStandbyPeakBytes"), standbyOwnedQueuePeakBytes },
        { QStringLiteral("catchupOwnedQueueStandbyPeakText"), formatOwnedQueueBytes(standbyOwnedQueuePeakBytes) },
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
        entry[QStringLiteral("selected")] = tm.value(QStringLiteral("selected")).toBool();
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
    const auto tracks = activePlayer->trackList();
    none[QStringLiteral("selected")] = Core::selectedTrackId(tracks, QStringLiteral("sub")) == 0;
    QVariantList result;
    result.append(none);
    int displayIndex = 1;
    for (const auto &t : tracks) {
        const auto tm = t.toMap();
        if (tm.value(QStringLiteral("type")).toString() != QLatin1String("sub")) {
            continue;
        }
        QVariantMap entry;
        entry[QStringLiteral("id")]       = tm.value(QStringLiteral("id"));
        entry[QStringLiteral("name")]     = QStringLiteral("Subtitle #%1").arg(displayIndex++);
        entry[QStringLiteral("subtitle")] = trackSubtitle(tm);
        entry[QStringLiteral("selected")] = tm.value(QStringLiteral("selected")).toBool();
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
    activePlayer->selectAudioTrack(id, true);
}

void PlayerController::selectSubtitleTrack(const int id)
{
    auto *activePlayer = playbackPlayer();
    if (!activePlayer->isAvailable()) {
        return;
    }
    activePlayer->selectSubtitleTrack(id, true);
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
    return Playback::PlaybackBuffering::adaptiveSteadyStateCacheLimitSeconds(bufferTargetSeconds);
}

double PlayerController::adaptiveSteadyStateCacheHysteresisSeconds(const double bufferTargetSeconds)
{
    return Playback::PlaybackBuffering::adaptiveSteadyStateCacheHysteresisSeconds(bufferTargetSeconds);
}

qint64 PlayerController::adaptiveSteadyStateMaxBytes(
    const double bufferTargetSeconds,
    const std::optional<double> averageBitsPerSecond)
{
    return Playback::PlaybackBuffering::adaptiveSteadyStateMaxBytes(bufferTargetSeconds, averageBitsPerSecond);
}

qint64 PlayerController::adaptiveSteadyStateMaxBackBytes(const std::optional<double> averageBitsPerSecond)
{
    return Playback::PlaybackBuffering::adaptiveSteadyStateMaxBackBytes(averageBitsPerSecond);
}

qint64 PlayerController::adaptiveCatchupMaxBytes(const std::optional<double> averageBitsPerSecond)
{
    return Playback::PlaybackBuffering::adaptiveCatchupMaxBytes(averageBitsPerSecond);
}

qint64 PlayerController::adaptiveCatchupMaxBackBytes(const std::optional<double> averageBitsPerSecond)
{
    return Playback::PlaybackBuffering::adaptiveCatchupMaxBackBytes(averageBitsPerSecond);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int PlayerController::startupBufferFallbackTimeoutMs(const double bufferTargetSeconds, const int segmentSecondsHint)
{
    return Playback::PlaybackBuffering::startupBufferFallbackTimeoutMs(bufferTargetSeconds, segmentSecondsHint);
}

int PlayerController::reconnectDepletionTimeoutMsForWaitSeconds(const double waitForDataStreamSeconds)
{
    return Playback::PlaybackRecovery::reconnectDepletionTimeoutMsForWaitSeconds(waitForDataStreamSeconds);
}

bool PlayerController::shouldStartPreemptiveReconnect(
    const std::optional<double> cacheDurationSeconds,
    const double bufferTargetSeconds,
    const std::optional<double> throughputBitsPerSecond,
    const bool playbackAdvanced,
    const bool backendBuffering)
{
    return Playback::PlaybackRecovery::shouldStartPreemptiveReconnect(cacheDurationSeconds, bufferTargetSeconds, throughputBitsPerSecond, playbackAdvanced, backendBuffering);
}

bool PlayerController::deadStreamLikelyDisconnected(
    const std::optional<double> cacheDurationSeconds,
    const std::optional<double> throughputBitsPerSecond)
{
    return Playback::PlaybackRecovery::deadStreamLikelyDisconnected(cacheDurationSeconds, throughputBitsPerSecond);
}

int PlayerController::reconnectAttemptIntervalMs()
{
    return kReconnectAttemptIntervalMs;
}

void PlayerController::startReconnectLoop(const QString &reason)
{
    auto *activePlayer = playbackPlayer();
    if (activePlayer == nullptr || !m_currentChannel.has_value() || m_buffering.startupPending() || m_channelLoadFailed
        || m_userPausedManually || m_catchupSession.reloadInFlight()) {
        return;
    }
    if (m_timeshiftController && m_timeshiftController->isActive()) {
        return;
    }

    if (m_recovery.active()) {
        return;
    }
    if (inCatchupMode()) {
        abortSeamlessCatchupRolling(QStringLiteral("reconnect-start"), true);
    }

    const auto reconnectUrl = recoveryPlaybackUrl();
    const auto reconnectLoadfileOptions = recoveryLoadfileOptions();
    if (reconnectUrl.isEmpty()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Reconnect loop aborted: no recovery URL for current playback mode."));
        return;
    }

    m_recovery.start();

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
    m_recovery.attemptTimer().start();
    handleReconnectAttemptTick();
}

void PlayerController::stopReconnectLoop(const QString &reason)
{
    const auto wasActive = m_recovery.active();
    auto *activePlayer = playbackPlayer();
    m_recovery.stop(!inCatchupMode() && activePlayer != nullptr, m_liveDeliveryClock.elapsed());
    if (wasActive && activePlayer != nullptr && !inCatchupMode()) {
        activePlayer->setStartupBufferingStrictMode(true);
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
    if (!m_recovery.attemptInFlight()) {
        return;
    }

    m_recovery.clearAttempt();
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Reconnect attempt resolved: %1.").arg(reason));
}

// Preserve distinct diagnostic reason and log-message parameters.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
    m_buffering.setStartupPending(false);
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
    if (!m_recovery.active() || m_userPausedManually || m_catchupSession.reloadInFlight()) return;
    if (!activePlayer || !m_currentChannel || m_buffering.startupPending()) {
        stopReconnectLoop(QStringLiteral("channel-or-mode-changed"));
        return;
    }
    const auto nowMs = m_liveDeliveryClock.elapsed();
    const auto waitMs = static_cast<int>(std::lround(normalizedWaitForDataStreamSeconds(m_waitForDataStreamSeconds) * 1000.0));
    if (m_recovery.attemptInFlight() && !inCatchupMode()) {
        if (m_recovery.totalExpired(waitMs, nowMs)) {
            activePlayer->stop();
            clearReconnectAttemptInFlight(QStringLiteral("reserve-total-timeout"));
        } else if (advanceReconnectReserve()) return;
    }
    if (m_recovery.attemptInFlight()) {
        const auto timeout = m_recovery.attemptTimeout(waitMs, m_positionTimer.interval(), nowMs);
        if (timeout.isEmpty()) return;
        activePlayer->stop();
        clearReconnectAttemptInFlight(timeout);
    }
    if (m_recovery.attempts() >= 5) {
        failReconnect(QStringLiteral("attempt-limit"));
        return;
    }
    const auto url = recoveryPlaybackUrl();
    const auto options = recoveryLoadfileOptions();
    if (url.isEmpty()) {
        failReconnect(QStringLiteral("empty-stream-url"));
        return;
    }
    const auto command = m_recovery.nextAttempt(nowMs);
    if (command == Playback::PlaybackRecovery::Command::Stop) {
        if (inCatchupMode()) {
            const auto position = activePlayer->position();
            if (position >= 0.0) m_catchupSession.setReconnectResumeStreamRelativeSeconds(position);
        }
        activePlayer->stop();
        return;
    }
    if (command != Playback::PlaybackRecovery::Command::Load) return;
    activePlayer->setStartupBufferingStrictMode(false);
    ++m_playbackGeneration;
    m_recovery.beginLoad(!inCatchupMode() && !m_sharedPlaybackPlayer, effectiveLiveBufferTargetSeconds(), nowMs);
    if (inCatchupMode()) {
        if (!m_catchupSession.reconnectResumeStreamRelativeSeconds()) {
            const auto position = activePlayer->position();
            if (position >= 0.0) m_catchupSession.setReconnectResumeStreamRelativeSeconds(position);
        }
        resetCatchupUrlLoadGuardState(false);
    } else m_catchupSession.setReconnectResumeStreamRelativeSeconds({});
    activePlayer->setPaused(m_recovery.reservePending());
    const auto playbackUrl = inCatchupMode() ? prepareCatchupStreamPlaybackUrl(activePlayer, url, false) : url;
    m_catchupSession.setProgressTransportReady(false);
    configurePlaybackTrackPreferences(activePlayer);
    activePlayer->play(playbackUrl, options);
}

void PlayerController::failReconnect(const QString &reason)
{
    if (reason == QStringLiteral("attempt-limit") && inCatchupMode()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Reconnect attempt-limit reached in catch-up; escalating to hard restore."));
        if (hardRestoreCatchupAtCurrentTimelinePoint(QStringLiteral("catchup-reconnect-attempt-limit"))) {
            stopReconnectLoop(QStringLiteral("catchup-hard-restore-escalation"));
            return;
        }
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up hard-restore escalation failed; falling back to channel load failure."));
    }
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Reconnect failed: %1. Marking channel as load failure.").arg(reason));
    stopReconnectLoop(reason);
    playbackPlayer()->stop();
    stopPauseStateResync();
    stopDeferredLoadingIndicator();
    m_buffering.setStartupPending(false);
    setChannelLoadFailed(true);
    setIsLoading(false);
    m_backendBuffering = false;
    clearPlaybackStallTracking();
    refreshBufferingState();
    setIsPlaying(false);
    emit playbackError(QStringLiteral("Channel couldn't be loaded"));
}

bool PlayerController::advanceReconnectReserve()
{
    if (inCatchupMode() || m_sharedPlaybackPlayer) return false;
    auto *player = playbackPlayer();
    const auto state = player->cacheReadState();
    const auto decision = m_recovery.reserve(player->demuxerCacheDurationSeconds(),
        state.has_value() && state->idle, state.has_value() && state->eof, m_backendBuffering,
        m_waitForDataStreamSeconds, m_liveDeliveryClock.elapsed());
    using Action = Playback::PlaybackRecovery::ReserveResult::Action;
    switch (decision.action) {
    case Action::Pause: player->setPaused(true); break;
    case Action::Resume:
        clearPlaybackStallTracking();
        player->setStartupBufferingStrictMode(true);
        if (!m_userPausedManually) player->setPaused(false);
        break;
    case Action::Stop: player->stop(); break;
    case Action::None: break;
    }
    if (decision.holdSample) refreshBufferingState();
    return decision.holdSample;
}

void PlayerController::evaluateReconnectRecovery()
{
    if (!m_recovery.active() || !m_currentChannel || m_buffering.startupPending()) return;
    if (m_recovery.recovered()) {
        clearReconnectAttemptInFlight(QStringLiteral("stabilization-healthy"));
        stopReconnectLoop(QStringLiteral("playback-recovered"));
    } else if (m_recovery.unstable()) {
        playbackPlayer()->stop();
        clearReconnectAttemptInFlight(QStringLiteral("stabilization-failed"));
        handleReconnectAttemptTick();
    }
}

void PlayerController::resetBitrateAverageWindow()
{
    m_buffering.resetAverage();
}

void PlayerController::resetAdaptiveSteadyStateBufferingState()
{
    resetLiveReserve(false);
    m_buffering.resetAdaptation();
}

std::optional<double> PlayerController::updateBitrateAverageBitsPerSecond(
    const std::optional<double> instantaneousBitsPerSecond)
{
    return m_buffering.observeBitrate(instantaneousBitsPerSecond, m_liveDeliveryClock.elapsed());
}

void PlayerController::resetLiveReserve(const bool resumePlayback, const bool preserveDeliveryObservation)
{
    const auto pending = m_buffering.liveReservePending();
    const auto decision = m_buffering.resetLiveReserve(resumePlayback, m_userPausedManually, preserveDeliveryObservation);
    if (decision.pause == Playback::PauseCommand::Resume) playbackPlayer()->setPaused(false);
    if (pending) {
        clearPlaybackStallTracking();
        refreshBufferingState();
    }
}

bool PlayerController::evaluateLiveReserve()
{
    if (!m_buffering.liveReservePending()) {
        return false;
    }
    if (!m_currentChannel.has_value() || inCatchupMode() || m_sharedPlaybackPlayer
        || timeshiftActive() || timeshiftPreparing() || m_recovery.active() || m_channelLoadFailed
        || m_userPausedManually) {
        resetLiveReserve(false);
        return false;
    }
    const auto state = playbackPlayer()->cacheReadState();
    if (state.has_value() && !state->eof) {
        // App-owned refill pauses still consume provider deliveries. Keep
        // measuring unmet demuxer demand; manual pauses are excluded above.
        observeLiveDelivery(*state);
    }
    m_buffering.raiseLiveTarget(playbackPlayer()->bufferTargetSeconds());
    const auto cache = playbackPlayer()->demuxerCacheDurationSeconds();
    // Retry throttled capacity changes while the normal position ticker is
    // suspended by this hold; otherwise a newly learned target could be capped.
    maybeRetuneSteadyStateBuffering(cache, playbackPlayer()->bufferTargetSeconds());
    return advanceLiveReserve(cache, state, m_buffering.liveReserveElapsed(m_liveDeliveryClock.elapsed()));
}

bool PlayerController::advanceLiveReserve(const std::optional<double> cacheSeconds,
                                        const std::optional<Player::MpvPlayer::CacheReadState> &readState,
                                        const qint64 elapsedMs)
{
    const auto wasPending = m_buffering.liveReservePending();
    const auto decision = m_buffering.advanceLiveReserve(cacheSeconds,
        readState.has_value() && readState->idle, readState.has_value() && readState->eof,
        elapsedMs, m_liveDeliveryClock.elapsed());
    if (decision.pause == Playback::PauseCommand::Resume && !m_userPausedManually) playbackPlayer()->setPaused(false);
    if (wasPending && !decision.pending) {
        clearPlaybackStallTracking();
        refreshBufferingState();
    }
    return decision.pending;
}

bool PlayerController::beginLiveReserve(const std::optional<double> cacheSeconds, const bool readerIdle)
{
    const auto decision = m_buffering.beginLiveReserve(cacheSeconds, readerIdle,
        playbackPlayer()->bufferTargetSeconds(), m_liveDeliveryClock.elapsed());
    if (decision.pause != Playback::PauseCommand::Pause) return false;
    playbackPlayer()->setPaused(true);
    refreshBufferingState();
    return true;
}

double PlayerController::effectiveLiveBufferTargetSeconds() const
{
    return m_buffering.targetSeconds(playbackPlayer()->bufferTargetSeconds());
}

void PlayerController::sampleLiveDelivery()
{
    if (m_buffering.liveReservePending()) {
        evaluateLiveReserve();
        // Pause telemetry can still reflect the hold until mpv acknowledges
        // resume. Don't treat that same sample as a manual observation break.
        return;
    }
    auto *activePlayer = playbackPlayer();
    if (!m_currentChannel.has_value() || inCatchupMode() || m_sharedPlaybackPlayer
        || (m_timeshiftController && (m_timeshiftController->isActive() || m_timeshiftController->isPreparing()))
        || m_recovery.active() || m_buffering.startupPending() || m_channelLoadFailed || m_isLoading
        || m_userPausedManually || activePlayer->pauseState().value_or(true)) {
        m_buffering.interruptDelivery();
        return;
    }
    const auto state = activePlayer->cacheReadState();
    if (!state.has_value() || state->eof) {
        m_buffering.interruptDelivery();
        return;
    }
    const auto cache = activePlayer->demuxerCacheDurationSeconds();
    observeLiveDelivery(*state);
    beginLiveReserve(cache, state->idle);
}

void PlayerController::observeLiveDelivery(const Player::MpvPlayer::CacheReadState &readState)
{
    auto *activePlayer = playbackPlayer();
    const auto previous = effectiveLiveBufferTargetSeconds();
    m_buffering.observeDelivery(static_cast<double>(m_liveDeliveryClock.elapsed()) / 1000.0,
        readState.endSeconds, readState.idle);
    const auto target = effectiveLiveBufferTargetSeconds();
    if (target != previous) {
        Core::DebugLogger::instance().log(QStringLiteral("player"),
            QStringLiteral("Live delivery adaptation: interval=%1s configured=%2s forward=%3s refill=0s.")
                .arg(m_buffering.detectedIntervalSeconds(), 0, 'f', 2)
                .arg(activePlayer->bufferTargetSeconds(), 0, 'f', 1)
                .arg(target, 0, 'f', 1));
        maybeRetuneSteadyStateBuffering(activePlayer->demuxerCacheDurationSeconds(), activePlayer->bufferTargetSeconds());
    }
}

void PlayerController::maybeRetuneSteadyStateBuffering(
    const std::optional<double> cacheDurationSeconds,
    const double bufferTargetSeconds)
{
    if ((!m_isPlaying && !m_buffering.liveReservePending())
        || inCatchupMode()
        || !m_currentChannel.has_value()
        || m_buffering.startupPending()
        || m_recovery.active()
        || m_sharedPlaybackPlayer
        || m_channelLoadFailed
        || m_userPausedManually) {
        return;
    }

    const auto policy = m_buffering.retune(false, bufferTargetSeconds, cacheDurationSeconds,
        startupPolicyForPlaybackRequest(playbackPlayer()) == StartupPolicy::FastLive, m_liveDeliveryClock.elapsed());
    if (policy) playbackPlayer()->setSteadyStateBufferingPolicy(*policy);
}

void PlayerController::maybeRetuneCatchupBuffering(const std::optional<double> cacheDurationSeconds)
{
    auto *activePlayer = playbackPlayer();
    if (!m_isPlaying
        || !inCatchupMode()
        || activePlayer == nullptr
        || !m_currentChannel.has_value()
        || m_buffering.startupPending()
        || m_recovery.active()
        || m_channelLoadFailed
        || m_userPausedManually) {
        return;
    }

    const auto policy = m_buffering.retune(true, playbackPlayer()->bufferTargetSeconds(), cacheDurationSeconds,
        startupPolicyForPlaybackRequest(playbackPlayer()) == StartupPolicy::FastLive, m_liveDeliveryClock.elapsed());
    if (policy) playbackPlayer()->setSteadyStateBufferingPolicy(*policy);
}

void PlayerController::applyActiveCatchupBufferingPolicy(Player::MpvPlayer *player)
{
    if (player) player->setSteadyStateBufferingPolicy(m_buffering.activeCatchupPolicy(m_liveDeliveryClock.elapsed()));
}

Player::MpvPlayer *PlayerController::player()
{
    return playbackPlayer();
}

QObject *PlayerController::playbackPlayerObject() const
{
    return const_cast<Player::MpvPlayer *>(playbackPlayer());
}

QObject *PlayerController::seamlessStandbyPlayerObject() const
{
    return m_catchupSeamlessPrewarmActive
        ? static_cast<QObject *>(m_catchupSeamlessPrewarmPlayer.data())
        : nullptr;
}

bool PlayerController::seamlessStandbyPrewarmActive() const
{
    return m_catchupSeamlessPrewarmActive && m_catchupSeamlessPrewarmPlayer.data() != nullptr;
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

void PlayerController::setSharedPlaybackPlayer(Player::MpvPlayer *player, const bool protectedSession)
{
    auto *previousPlayer = playbackPlayer();
    m_sharedPlaybackPlayer = (player != nullptr && player != &m_player) ? player : nullptr;
    m_sharedPlaybackProtected = m_sharedPlaybackPlayer ? protectedSession : false;
    auto *nextPlayer = playbackPlayer();
    if (previousPlayer == nextPlayer) {
        return;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Visible playback player changed: previous=%1 next=%2 protected=%3 mode=%4.")
            .arg(reinterpret_cast<quintptr>(previousPlayer), 0, 16)
            .arg(reinterpret_cast<quintptr>(nextPlayer), 0, 16)
            .arg(m_sharedPlaybackProtected ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(m_playbackMode));
    emit playbackPlayerObjectChanged();
}

void PlayerController::attachSharedPlayback(
    Player::MpvPlayer *sharedPlayer,
    const Channel &channel,
    const bool protectedSession,
    const bool stopBasePlayerOnInitialAttach)
{
    ++m_playbackGeneration;
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
    setSharedPlaybackPlayer(sharedPlayer, protectedSession);
    clearCatchupState();
    m_sharedPlaybackPlayer->setAudioEnabled(true);
    const auto effectiveVolume = (m_muted || m_volume <= 0.0)
        ? 0
        : static_cast<int>(std::round(std::clamp(m_volume, 0.0, 100.0)));
    m_sharedPlaybackPlayer->setVolume(effectiveVolume);
    m_currentChannel = channel;
    const auto previousPlaybackUrl = m_currentPlaybackUrl;
    configurePlaybackTrackPreferences(playbackPlayer());
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
    ++m_playbackGeneration;
    const auto previousName = m_nowPlayingName;
    stopReconnectLoop(QStringLiteral("adopt-existing-playback"));
    stopPauseStateResync();
    stopDeferredLoadingIndicator();
    stopStartupBufferProbe();
    stopStartupBufferFallbackWatchdog(true);
    m_buffering.setStartupPending(false);
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
    configurePlaybackTrackPreferences(playbackPlayer());
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
    ++m_playbackGeneration;
    if (!m_sharedPlaybackPlayer) {
        return;
    }

    const auto sharedPlayer = m_sharedPlaybackPlayer;
    const auto protectedSession = m_sharedPlaybackProtected;
    setSharedPlaybackPlayer(nullptr, false);
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
    m_buffering.setStartupPending(false);
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
    const bool remuxRecordingsToMkv,
    const bool imageSmoothingEnabled,
    const QString &picturePreset)
{
    m_remuxToMkv = remuxRecordingsToMkv && Core::ffmpegToolsAvailable();
    m_waitForDataStreamSeconds = normalizedWaitForDataStreamSeconds(waitForDataStreamSeconds);
    m_player.configureLibraryPath(mpvDllPath);
    m_player.configureOptions(mpvOptions);
    m_player.configurePlaybackTuning(m_waitForDataStreamSeconds, deinterlaceEnabled, bufferSizeSeconds);
    m_player.configureUserAgent(playerUserAgent);
    m_player.configureImageSmoothing(imageSmoothingEnabled);
    m_player.configurePicturePreset(picturePreset);
    if (m_sharedPlaybackPlayer) {
        m_sharedPlaybackPlayer->configureImageSmoothing(imageSmoothingEnabled);
        m_sharedPlaybackPlayer->configurePicturePreset(picturePreset);
    }
    m_catchupRequestHeaders = Player::CatchupStreamSession::requestHeadersFromOptions(playerUserAgent, mpvOptions);
    m_catchupStandbyPlayer.configureLibraryPath(mpvDllPath);
    m_catchupStandbyPlayer.configureOptions(mpvOptions);
    m_catchupStandbyPlayer.configurePlaybackTuning(m_waitForDataStreamSeconds, deinterlaceEnabled, bufferSizeSeconds);
    m_catchupStandbyPlayer.configureUserAgent(playerUserAgent);
    m_catchupStandbyPlayer.configureImageSmoothing(imageSmoothingEnabled);
    m_catchupStandbyPlayer.configurePicturePreset(picturePreset);
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

PlayerController::StartupPolicy PlayerController::startupPolicyForPlaybackRequest(Player::MpvPlayer *activePlayer) const
{
    return Playback::PlaybackBuffering::startupPolicy(inCatchupMode(), activePlayer && activePlayer == &m_player,
        !m_sharedPlaybackPlayer.isNull(), m_recovery.active(), m_hwdecFallbackApplied,
        timeshiftActive() || timeshiftPreparing());
}

QString PlayerController::startupPolicyLabel(const StartupPolicy policy)
{
    switch (policy) {
    case StartupPolicy::StrictBuffered:
        return QStringLiteral("strict-buffered");
    case StartupPolicy::FastLive:
        return QStringLiteral("fast-live");
    case StartupPolicy::BestEffort:
        return QStringLiteral("best-effort");
    }

    return QStringLiteral("unknown");
}

void PlayerController::startPlaybackRequest(
    Player::MpvPlayer *activePlayer,
    const QString &url,
    const bool pauseWhenReady,
    const QString &loadfileOptions)
{
    ++m_playbackGeneration;
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
    if (!inCatchupMode()) {
        resetCatchupDegradationRecoveryState();
    }
    m_tuneAttemptTimer.restart();
    stopStartupBufferProbe();
    stopStartupBufferFallbackWatchdog(true);
    resetBitrateAverageWindow();
    resetAdaptiveSteadyStateBufferingState();
    const auto startupPolicy = startupPolicyForPlaybackRequest(activePlayer);
    switch (startupPolicy) {
    case StartupPolicy::BestEffort:
        activePlayer->setStartupBufferingStrictMode(false);
        break;
    case StartupPolicy::StrictBuffered:
    case StartupPolicy::FastLive:
        activePlayer->resetSteadyStateBuffering();
        activePlayer->setStartupBufferingStrictMode(startupPolicy == StartupPolicy::StrictBuffered);
        break;
    }
    stopPauseStateResync();
    setChannelLoadFailed(false);
    beginDeferredLoadingIndicator();
    m_buffering.setStartupPending(startupPolicy == StartupPolicy::StrictBuffered);
    m_backendBuffering = false;
    clearPlaybackStallTracking();
    refreshBufferingState();
    setIsPlaying(false);
    activePlayer->setPaused(startupPolicy == StartupPolicy::StrictBuffered ? true : pauseWhenReady);
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Startup policy selected for tune: %1.")
            .arg(startupPolicyLabel(startupPolicy)));
    m_catchupSession.setProgressTransportReady(false);
    configurePlaybackTrackPreferences(activePlayer);
    activePlayer->play(url, loadfileOptions);
}

QString PlayerController::catchupLoadfileOptions(const double streamBaseOffsetSeconds, const bool standby) const
{
    auto sharedOptions = Playback::PlaybackBuffering::catchupBufferOptions(standby);
    if (m_catchupSession.endless() || !m_catchupSession.programStartUtc().isValid() || !m_catchupSession.programStopUtc().isValid() || m_catchupSession.programStopUtc() <= m_catchupSession.programStartUtc()) {
        return sharedOptions;
    }
    const auto baseDurationSeconds = std::max<qint64>(1, m_catchupSession.programStartUtc().secsTo(m_catchupSession.programStopUtc()));
    const auto effectiveBaseOffset = streamBaseOffsetSeconds >= 0.0
        ? streamBaseOffsetSeconds
        : m_catchupSession.streamBaseOffsetSeconds();
    const auto remainingSeconds = std::max<qint64>(
        1,
        baseDurationSeconds - static_cast<qint64>(std::floor(std::max(0.0, effectiveBaseOffset))));
    const auto boundedDurationSeconds = std::max<qint64>(1, remainingSeconds + 60);
    return QStringLiteral("%1,length=%2").arg(sharedOptions).arg(boundedDurationSeconds);
}

void PlayerController::setCatchupState(const QString &liveUrl, const QString &programLabel)
{
    const auto previousMode = m_playbackMode;
    const auto previousLabel = m_catchupSession.programLabel();

    m_catchupSession.setLivePlaybackUrlBeforeCatchup(liveUrl.trimmed());
    m_playbackMode = QStringLiteral("catchup");
    m_catchupSession.setProgramLabel(programLabel.trimmed());

    if (m_playbackMode != previousMode) {
        emit playbackModeChanged();
    }
    if (m_catchupSession.programLabel() != previousLabel) {
        emit catchupProgramLabelChanged();
    }
    syncCatchupTimelineState();
}

void PlayerController::clearCatchupState()
{
    if (inCatchupMode()) {
        emit catchupProgressFlushRequested();
    }
    m_catchupSession.setProgressTransportReady(false);
    m_catchupSession.setProgressSeekTargetSeconds({});
    m_catchupSession.setAlignmentActive(false);
    m_catchupSession.setPeriodReload(false);
    m_catchupSession.setContinuousRecoveryTarget({});
    m_catchupSession.setEndless(false);
    m_catchupSession.resetContinuation();
    m_catchupSession.setDisplayProgram({});
    m_catchupSession.setValidatedProgram({});
    resetCatchupRebuffering(false);
    const auto previousMode = m_playbackMode;
    const auto previousLabel = m_catchupSession.programLabel();

    m_playbackMode = QStringLiteral("live");
    m_catchupSession.setProgramLabel({});
    m_catchupSession.setLivePlaybackUrlBeforeCatchup({});
    m_catchupSession.setCanonicalPlaybackUrl({});
    m_catchupSession.setProgramStartUtc({});
    m_catchupSession.setProgramStopUtc({});
    m_catchupSession.setProgramBoundaryReached(false);
    m_catchupSession.setActiveEofObserved(false);
    m_catchupSession.setStreamBaseOffsetSeconds(0.0);
    m_catchupSession.setDesiredDelaySeconds(0.0);
    m_catchupSession.setTransportEndTimelineSeconds(0.0);
    m_catchupSession.setPendingStreamRelativeSeekSeconds(std::nullopt);
    m_catchupSession.setReconnectResumeStreamRelativeSeconds(std::nullopt);
    m_catchupSession.setPendingInitialSeekSeconds(std::nullopt);
    m_catchupSession.cancelReload();
    abortSeamlessCatchupRolling(QStringLiteral("clear-catchup-state"), false);
    if (m_catchupActiveStreamSession) {
        m_catchupActiveStreamSession->closeProviderConnection(QStringLiteral("clear-catchup-state"));
        m_catchupActiveStreamSession.reset();
    }
    if (m_catchupStandbyStreamSession) {
        m_catchupStandbyStreamSession->closeProviderConnection(QStringLiteral("clear-catchup-state"));
        m_catchupStandbyStreamSession.reset();
    }
    if (m_sharedPlaybackPlayer && m_sharedPlaybackPlayer.data() == &m_catchupStandbyPlayer) {
        setSharedPlaybackPlayer(nullptr, false);
    }
    m_catchupStandbyPlayer.setAudioEnabled(false);
    m_catchupStandbyPlayer.setVolume(0);
    m_catchupStandbyPlayer.stop();
    resetCatchupDegradationRecoveryState();
    m_catchupSession.clearRollbackGuard();
    m_catchupSession.standby().resetBackoff();
    m_catchupSession.setTimelineStartEpochMs(0);
    m_catchupSession.setTimelineAvailableEdgeEpochMs(0);
    m_catchupSession.setTimelineAvailableSeconds(0.0);
    m_catchupSession.setTimelinePositionSeconds(0.0);
    m_catchupSession.setTimelineAtLiveEdge(true);
    setCatchupTimelineNoticeText(QString {});

    if (m_playbackMode != previousMode) {
        emit playbackModeChanged();
    }
    if (m_catchupSession.programLabel() != previousLabel) {
        emit catchupProgramLabelChanged();
    }
    emit catchupTimelineChanged();
    emit timeshiftStateChanged();
}

void PlayerController::resetCatchupDegradationRecoveryState()
{
    m_catchupSession.resetRecovery();
}

qint64 PlayerController::catchupTimelineEndEpochMs() const
{
    return m_catchupSession.catchupTimelineEndEpochMs(inCatchupMode(), QDateTime::currentDateTimeUtc());
}

double PlayerController::catchupTimelineDurationSeconds() const
{
    return m_catchupSession.catchupTimelineDurationSeconds(inCatchupMode(), QDateTime::currentDateTimeUtc());
}

void PlayerController::syncCatchupTimelineState()
{
    if (!m_catchupSession.syncTimeline(inCatchupMode(), QDateTime::currentDateTimeUtc())) return;
    if (m_catchupSession.endless())
        emit catchupPlaybackTimeChanged(m_catchupSession.programStartUtc().addMSecs(static_cast<qint64>(m_catchupSession.timelinePositionSeconds() * 1000.0)));
    emit catchupTimelineChanged();
    emit timeshiftStateChanged();
}

void PlayerController::updateCatchupProgramme(const std::optional<Core::EpgEntry> &program)
{
    if (m_catchupSession.updateProgramme(inCatchupMode(), program)) emit catchupProgramLabelChanged();
}

void PlayerController::resetCatchupUrlLoadGuardState(bool initialLoadContext)
{
    m_catchupSession.resetRollbackGuard(initialLoadContext, m_liveDeliveryClock.elapsed());
}
void PlayerController::maybeCorrectUnexpectedCatchupRollback(double currentStreamSeconds)
{
    auto *backend = playbackPlayer();
    const auto seek = m_catchupSession.correctRollback(currentStreamSeconds, inCatchupMode(), backend != nullptr,
        backend ? backend->demuxerSeekableRangeSeconds() : std::nullopt, m_liveDeliveryClock.elapsed());
    if (seek) backend->seekAbsoluteFast(*seek);
}

bool PlayerController::seekCatchupToTimelinePosition(const double targetSeconds)
{
    if (!inCatchupMode()) {
        return false;
    }
    syncCatchupTimelineState();
    if (targetSeconds > m_catchupSession.timelineAvailableSeconds()) {
        setCatchupTimelineNoticeText(
            QStringLiteral("The last %1 minutes are not yet available in the archive.")
                .arg(m_catchupSession.safetySeconds() / 60),
            kCatchupTimelineNoticeAutoClearMs);
        return false;
    }
    m_catchupSession.resetContinuation();
    setChannelLoadFailed(false);
    setCatchupTimelineNoticeText(QString {});
    const auto bounded = std::max(0.0, std::min(m_catchupSession.timelineAvailableSeconds(), targetSeconds));
    if (m_catchupSession.alignmentActive()) {
        m_catchupSession.setAlignmentActive(false);
        m_catchupSession.setReconnectResumeStreamRelativeSeconds({});
        playbackPlayer()->setPaused(m_userPausedManually);
    }
    resetCatchupRebuffering(true);
    m_catchupSession.setTimelinePositionSeconds(bounded);
    m_catchupSession.setDesiredDelaySeconds(std::max(0.0, m_catchupSession.timelineAvailableSeconds() - bounded));
    m_catchupSession.setTimelineAtLiveEdge((m_catchupSession.timelineAvailableSeconds() - bounded) <= 0.75);
    syncCatchupTimelineState();
    if (playbackPlayer() == nullptr) {
        return false;
    }
    if (shouldReloadCatchupForSeek(bounded) && reloadCatchupForTimelineSeek(bounded)) {
        return true;
    }

    const auto streamRelativeTarget = std::max(0.0, bounded - m_catchupSession.streamBaseOffsetSeconds());
    m_catchupSession.setProgressSeekTargetSeconds(bounded);
    m_catchupSession.seekStarted(m_liveDeliveryClock.elapsed());
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up timeline seek: mode=absolute+keyframes target=%1s streamTarget=%2s streamBase=%3s current=%4s available=%5s host=%6")
            .arg(bounded, 0, 'f', 3)
            .arg(streamRelativeTarget, 0, 'f', 3)
            .arg(m_catchupSession.streamBaseOffsetSeconds(), 0, 'f', 3)
            .arg(m_recovery.lastPosition(), 0, 'f', 3)
            .arg(m_catchupSession.timelineAvailableSeconds(), 0, 'f', 3)
            .arg(QUrl(m_currentPlaybackUrl).host(QUrl::FullyDecoded)));
    playbackPlayer()->seekAbsoluteFast(streamRelativeTarget);
    return true;
}

bool PlayerController::shouldReloadCatchupForSeek(double targetSeconds) const
{
    const auto *backend = playbackPlayer();
    return backend && inCatchupMode() && m_catchupSession.shouldReloadForSeek(targetSeconds,
        backend->position(), backend->demuxerSeekableRangeSeconds(), m_currentChannel);
}

bool PlayerController::shouldExtendCatchupRollingWindowPredictively(const double currentStreamSeconds) const
{
    if (m_catchupActiveStreamSession
        && (m_catchupActiveStreamSession->continuous() || m_catchupActiveStreamSession->failedMediaTransport()
            || m_catchupActiveStreamSession->nextPeriodBaseSeconds().has_value())) {
        return false;
    }
    if (!inCatchupMode()
        || !m_currentChannel.has_value()
        || m_catchupSession.programBoundaryReached()
        || m_channelLoadFailed
        || m_buffering.startupPending()
        || m_catchupSession.reloadInFlight()
        || m_recovery.active()) {
        return false;
    }
    if (!m_catchupSession.programStartUtc().isValid()
        || !m_catchupSession.programStopUtc().isValid()
        || m_catchupSession.programStopUtc() <= m_catchupSession.programStartUtc()) {
        return false;
    }
    if (!m_catchupSession.endless() && QDateTime::currentDateTimeUtc() >= m_catchupSession.programStopUtc().addSecs(-1)) {
        return false;
    }
    if (currentStreamSeconds < 0.0
        || m_catchupSession.canonicalPlaybackUrl().trimmed().isEmpty()
        || !canRegenerateCatchupUrl()) {
        return false;
    }
    if (m_catchupSession.rollingBackoff(m_liveDeliveryClock.elapsed())) {
        return false;
    }

    const auto remainingSeconds = m_catchupSession.transportEndTimelineSeconds() - m_catchupSession.timelinePositionSeconds();
    return std::isfinite(remainingSeconds)
        && remainingSeconds > kPlaybackPositionEpsilon
        && remainingSeconds <= kCatchupRollingPredictiveTriggerSeconds;
}

bool PlayerController::seamlessCatchupRollingEnabled() const
{
    return !envFlagEnabled("OKILTV_DISABLE_CATCHUP_SEAMLESS_ROLLING");
}

bool PlayerController::canUseSeamlessCatchupRolling() const
{
    if (m_catchupActiveStreamSession
        && (m_catchupActiveStreamSession->continuous() || m_catchupActiveStreamSession->failedMediaTransport()
            || m_catchupActiveStreamSession->nextPeriodBaseSeconds().has_value())) {
        return false;
    }
    if (!seamlessCatchupRollingEnabled()) {
        return false;
    }
    if (!inCatchupMode()
        || !m_currentChannel.has_value()
        || m_catchupSession.programBoundaryReached()
        || m_channelLoadFailed
        || m_buffering.startupPending()
        || m_catchupSession.reloadInFlight()
        || m_recovery.active()) {
        return false;
    }
    if (!m_catchupSession.programStartUtc().isValid()
        || !m_catchupSession.programStopUtc().isValid()
        || m_catchupSession.programStopUtc() <= m_catchupSession.programStartUtc()) {
        return false;
    }
    if (!m_catchupSession.endless() && QDateTime::currentDateTimeUtc() >= m_catchupSession.programStopUtc().addSecs(-1)) {
        return false;
    }
    if (m_catchupSession.canonicalPlaybackUrl().trimmed().isEmpty()
        || !canRegenerateCatchupUrl()) {
        return false;
    }
    if (m_sharedPlaybackPlayer && m_sharedPlaybackPlayer.data() != &m_catchupStandbyPlayer) {
        return false;
    }
    return true;
}

Player::MpvPlayer *PlayerController::seamlessCatchupStandbyPlayer()
{
    auto *activePlayer = playbackPlayer();
    if (activePlayer == &m_player) {
        return &m_catchupStandbyPlayer;
    }
    return &m_player;
}

bool PlayerController::armSeamlessCatchupRollingExtension(
    const QString &reason,
    const double targetSeconds,
    const QString &regeneratedUrl,
    const double streamBaseOffsetSeconds)
{
    if (!canUseSeamlessCatchupRolling()) {
        return false;
    }
    if (regeneratedUrl.trimmed().isEmpty()) {
        return false;
    }

    m_catchupSession.standby().arm(regeneratedUrl, streamBaseOffsetSeconds);
    m_catchupSeamlessStandbyPlayer = seamlessCatchupStandbyPlayer();
    setSeamlessStandbyPrewarmState(false);

    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up seamless extension armed (reason=%1 target=%2s streamBase=%3s player=%4).")
            .arg(reason)
            .arg(targetSeconds, 0, 'f', 3)
            .arg(streamBaseOffsetSeconds, 0, 'f', 3)
            .arg(m_catchupSeamlessStandbyPlayer.data() == &m_player ? QStringLiteral("base") : QStringLiteral("standby")));
    return true;
}

bool PlayerController::startSeamlessCatchupStandbyLoad()
{
    if (!m_catchupSession.standby().pending() || m_catchupSession.standby().loadIssued() || m_catchupSession.standby().ready()) {
        return false;
    }
    if (!canUseSeamlessCatchupRolling()) {
        return false;
    }

    auto *standbyPlayer = m_catchupSeamlessStandbyPlayer.data();
    if (standbyPlayer == nullptr || standbyPlayer == playbackPlayer()) {
        return false;
    }
    if (m_catchupSession.standby().stopPending()) {
        return true;
    }
    if (!m_catchupSession.standby().allowAttempt(m_liveDeliveryClock.elapsed())) return false;

    // Standby remains dormant during ordinary playback/cleanup. A real prewarm
    // attempt needs its backend before entering the stop-acknowledgement flow.
    if (!standbyPlayer->ensureInitialized()) {
        return false;
    }
    m_catchupSession.standby().beginStop();
    standbyPlayer->stop();
    m_catchupSession.standby().stopAckTimer().start();
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up seamless standby stop-first requested before next standby load."));
    return true;
}

bool PlayerController::launchSeamlessCatchupStandbyLoad(Player::MpvPlayer *standbyPlayer)
{
    if (!m_catchupSession.standby().pending() || m_catchupSession.standby().loadIssued() || m_catchupSession.standby().ready()) {
        return false;
    }
    if (!canUseSeamlessCatchupRolling()) {
        return false;
    }
    if (standbyPlayer == nullptr || standbyPlayer == playbackPlayer()) {
        return false;
    }
    if (!refreshSeamlessCatchupStandbyRetryUrl()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up seamless standby load skipped: failed to refresh retry URL."));
        return false;
    }

    standbyPlayer->setAudioEnabled(true);
    standbyPlayer->setVolume(0);
    standbyPlayer->setStartupBufferingStrictMode(false);
    standbyPlayer->setPaused(false);
    const auto standbyPlaybackUrl =
        prepareCatchupStreamPlaybackUrl(standbyPlayer, m_catchupSession.standby().url(), true);
    configurePlaybackTrackPreferences(standbyPlayer, false);
    standbyPlayer->play(
        standbyPlaybackUrl,
        catchupLoadfileOptions(m_catchupSession.standby().baseOffset(), true));
    m_catchupSession.standby().loaded(m_liveDeliveryClock.elapsed());
    setSeamlessStandbyPrewarmState(true, standbyPlayer);

    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up seamless standby load started (host=%1).")
            .arg(QUrl(m_catchupSession.standby().url()).host(QUrl::FullyDecoded)));
    return true;
}

bool PlayerController::refreshSeamlessCatchupStandbyRetryUrl()
{
    if (!m_catchupSession.standby().pending() || m_catchupSession.standby().url().trimmed().isEmpty()) {
        return false;
    }
    if (!canRegenerateCatchupUrl()) {
        return true;
    }

    syncCatchupTimelineState();
    if (!m_catchupSession.programStartUtc().isValid()
        || !m_catchupSession.programStopUtc().isValid()
        || m_catchupSession.programStopUtc() <= m_catchupSession.programStartUtc()) {
        return false;
    }

    const auto availableSeconds = std::max(0.0, m_catchupSession.timelineAvailableSeconds());
    const auto desiredDelaySeconds = std::clamp(m_catchupSession.desiredDelaySeconds(), 0.0, availableSeconds);
    auto targetTimelineSeconds = (m_catchupSession.endless() || m_catchupSession.continuousFallback())
        ? std::clamp(m_catchupSession.timelinePositionSeconds(), 0.0, availableSeconds)
        : std::max(0.0, availableSeconds - desiredDelaySeconds);
    const auto totalDurationSeconds =
        m_catchupSession.endless() ? static_cast<qint64>(std::ceil(availableSeconds)) + 1
                        : std::max<qint64>(1, m_catchupSession.programStartUtc().secsTo(m_catchupSession.programStopUtc()));
    targetTimelineSeconds =
        std::min(targetTimelineSeconds, std::max(0.0, static_cast<double>(totalDurationSeconds) - 1.0));

    double streamBaseOffsetSeconds = 0.0;
    const auto refreshedUrl = regeneratedCatchupUrl(targetTimelineSeconds, &streamBaseOffsetSeconds);
    if (refreshedUrl.trimmed().isEmpty()) {
        return false;
    }

    const auto previousUrl = m_catchupSession.standby().url();
    const auto previousStreamBase = m_catchupSession.standby().baseOffset();
    m_catchupSession.standby().setUrl(refreshedUrl);
    m_catchupSession.standby().setBaseOffset(streamBaseOffsetSeconds);

    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral(
            "Catch-up seamless standby retry URL refreshed: changed=%1 target=%2s streamBase=%3s previousBase=%4s previous=%5 refreshed=%6.")
            .arg(previousUrl != refreshedUrl ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(targetTimelineSeconds, 0, 'f', 3)
            .arg(streamBaseOffsetSeconds, 0, 'f', 3)
            .arg(previousStreamBase, 0, 'f', 3)
            .arg(Core::redactSensitiveUrl(previousUrl))
            .arg(Core::redactSensitiveUrl(refreshedUrl)));
    return true;
}

QString PlayerController::prepareCatchupStreamPlaybackUrl(
    Player::MpvPlayer *targetPlayer,
    const QString &sourceUrl,
    const bool standby)
{
    if (!standby) {
        resetCatchupRebuffering(true);
    }
    auto trimmedSource = sourceUrl.trimmed();
    if (trimmedSource.isEmpty()
        || targetPlayer == nullptr
        || envFlagEnabled("OKILTV_HEADLESS_TEST")
        || envFlagEnabled("OKILTV_DISABLE_CATCHUP_OWNED_STREAM")) {
        return trimmedSource;
    }
    if (!targetPlayer->ensureInitialized() || !targetPlayer->catchupStreamProtocolAvailable()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up owned stream unavailable; using direct mpv URL for %1.")
                .arg(standby ? QStringLiteral("standby") : QStringLiteral("active")));
        return trimmedSource;
    }

    if (!standby && m_catchupActiveStreamSession) {
        if (m_catchupActiveStreamSession->hasNetworkError() && !m_catchupSession.continuousRecoveryTarget().has_value()) {
            m_catchupSession.setContinuousFallback(true);
        }
        m_catchupActiveStreamSession->closeProviderConnection(QStringLiteral("replace-active-session"));
    }
    auto policy = Playback::PlaybackBuffering::catchupOwnedStreamPolicy(standby);
    const auto continuous = !standby && !m_catchupSession.continuousFallback()
        && !envFlagEnabled("OKILTV_DISABLE_CATCHUP_CONTINUOUS")
        && QUrl(m_catchupSession.canonicalPlaybackUrl()).path().endsWith(QStringLiteral(".ts"), Qt::CaseInsensitive)
        && canRegenerateCatchupUrl();
    if (continuous) {
        policy.queueHighWaterBytes = 32LL * 1024 * 1024;
        policy.queueLowWaterBytes = 16LL * 1024 * 1024;
        policy.normalizeMpegTsTimestamps = true;
        policy.transferTimeoutMs = 15000;
    }
    const auto finitePeriods = !continuous && !standby
        && QUrl(trimmedSource).path().endsWith(QStringLiteral(".ts"), Qt::CaseInsensitive)
        && canRegenerateCatchupUrl();
    if (finitePeriods) {
        policy.normalizeMpegTsTimestamps = true;
    }
    auto session = Player::CatchupStreamSession::create(
        trimmedSource,
        m_catchupRequestHeaders,
        policy);
    if (!standby && (m_catchupSession.continuousRecoveryTarget() || m_recovery.active()) && m_catchupActiveStreamSession) {
        session->allowRetriedForwardGap(m_catchupActiveStreamSession->failedForwardGap());
    }
    if (finitePeriods) {
        session->configureMediaPeriods(m_catchupSession.streamBaseOffsetSeconds());
    }
    if (continuous) {
        const auto archiveComplete = Core::CatchupUrlResolver::availableEdge(
            m_catchupSession.programStopUtc(), m_catchupSession.safetySeconds()) >= m_catchupSession.programStopUtc();
        session->configureContinuous({
            .canonicalUrl = m_catchupSession.canonicalPlaybackUrl(),
            .programStartUtc = m_catchupSession.programStartUtc(),
            .programStopUtc = m_catchupSession.programStopUtc(),
            .streamBaseSeconds = m_catchupSession.streamBaseOffsetSeconds(),
            .safetySeconds = m_catchupSession.safetySeconds(),
            .initialBufferSeconds = archiveComplete ? targetPlayer->bufferTargetSeconds() : 30.0,
            .allowContinuation = m_catchupSession.endless() || !archiveComplete,
            .endless = m_catchupSession.endless(),
            .templateChannel = m_currentChannel && m_currentChannel->source == Core::ChannelSource::M3U
                && !m_currentChannel->catchupSourceTemplate.trimmed().isEmpty()
                ? m_currentChannel : std::nullopt,
        });
    }
    if (!session->start()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up owned stream failed to start; using direct mpv URL."));
        return trimmedSource;
    }
    const auto virtualUrl = session->virtualUrl();
    if (standby) {
        if (m_catchupStandbyStreamSession) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Replacing catch-up standby stream session: previous=%1 next-source=%2.")
                    .arg(m_catchupStandbyStreamSession->virtualUrl(), Core::redactSensitiveUrl(trimmedSource)));
            m_catchupStandbyStreamSession->closeProviderConnection(QStringLiteral("replace-standby-session"));
        }
        m_catchupStandbyStreamSession = std::move(session);
    } else {
        if (m_catchupActiveStreamSession) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Replacing catch-up active stream session: previous=%1 next-source=%2.")
                    .arg(m_catchupActiveStreamSession->virtualUrl(), Core::redactSensitiveUrl(trimmedSource)));
            m_catchupActiveStreamSession->closeProviderConnection(QStringLiteral("replace-active-session"));
        }
        m_catchupActiveStreamSession = std::move(session);
    }
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up owned stream prepared: %1 source=%2 virtual=%3.")
            .arg(standby ? QStringLiteral("standby") : QStringLiteral("active"))
            .arg(Core::redactSensitiveUrl(trimmedSource), virtualUrl));
    return virtualUrl;
}

bool PlayerController::activeCatchupProviderConnectionClosed() const
{
    if (!m_catchupActiveStreamSession) {
        return false;
    }
    return m_catchupActiveStreamSession->providerConnectionClosed();
}

bool PlayerController::standbyCatchupSessionHealthyForCutover() const
{
    if (!m_catchupStandbyStreamSession) {
        return true;
    }
    if (m_catchupStandbyStreamSession->hasNetworkError()) {
        return false;
    }
    if (m_catchupStandbyStreamSession->providerConnectionClosed()
        && !m_catchupStandbyStreamSession->closeRequestedByApp()) {
        // A finite HTTP response can finish while playable media remains in
        // the application queue or mpv cache. Empty responses remain failures.
        const auto *standbyPlayer = m_catchupSeamlessStandbyPlayer.data();
        return m_catchupStandbyStreamSession->peakBufferedBytes() > 0
            && (m_catchupStandbyStreamSession->bufferedBytes() > 0
                || (standbyPlayer && standbyPlayer->demuxerCacheDurationSeconds().value_or(0.0) > 0.0));
    }
    return true;
}

void PlayerController::markSeamlessStandbyAttemptFailed(const QString &reason)
{
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up seamless standby load failed: %1").arg(reason));
    m_catchupSession.standby().failed(m_liveDeliveryClock.elapsed());
}

void PlayerController::abortSeamlessCatchupRolling(const QString &reason, const bool stopStandbyPlayer)
{
    auto *standbyPlayer = m_catchupSeamlessStandbyPlayer.data();
    const auto hadState = m_catchupSession.standby().pending()
        || m_catchupSession.standby().loadIssued()
        || m_catchupSession.standby().ready()
        || m_catchupSession.standby().fallbackDeferred()
        || !m_catchupSession.standby().url().isEmpty()
        || m_catchupSeamlessStandbyPlayer;
    m_catchupSession.standby().cancel();
    m_catchupSeamlessStandbyPlayer = nullptr;
    setSeamlessStandbyPrewarmState(false);
    if (m_catchupStandbyStreamSession) {
        m_catchupStandbyStreamSession->closeProviderConnection(reason);
        m_catchupStandbyStreamSession.reset();
    }
    if (stopStandbyPlayer && standbyPlayer != nullptr && standbyPlayer != playbackPlayer()) {
        standbyPlayer->stop();
        standbyPlayer->setAudioEnabled(false);
        standbyPlayer->setVolume(0);
    }
    if (hadState && !reason.trimmed().isEmpty()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up seamless extension cleared: %1.").arg(reason));
    }
}

bool PlayerController::maybeCommitSeamlessCatchupCutover(const QString &reason, const bool forceWithoutNearEdge)
{
    if (!m_catchupSession.standby().pending() || !m_catchupSession.standby().ready()) {
        return false;
    }
    if (!standbySeamlessVideoReady()) {
        return false;
    }
    auto *standbyPlayer = m_catchupSeamlessStandbyPlayer.data();
    auto *activePlayer = playbackPlayer();
    if (standbyPlayer == nullptr || activePlayer == nullptr || standbyPlayer == activePlayer) {
        return false;
    }
    if (!standbyCatchupSessionHealthyForCutover()) {
        markSeamlessStandbyAttemptFailed(QStringLiteral("standby owned stream unavailable at cutover"));
        return false;
    }
    if (!m_catchupSession.endless() && QDateTime::currentDateTimeUtc() >= m_catchupSession.programStopUtc().addSecs(-1)) {
        return false;
    }

    const auto remainingSeconds = m_catchupSession.transportEndTimelineSeconds() - m_catchupSession.timelinePositionSeconds();
    const auto activeCacheDuration = activePlayer->demuxerCacheDurationSeconds();
    const auto decision = m_catchupSession.standby().evaluateCutover({
        .activeCache = activeCacheDuration, .remainingSeconds = remainingSeconds,
        .force = forceWithoutNearEdge,
        .alignToWatched = m_catchupSession.endless() || m_catchupSession.continuousFallback(),
        .watchedSeconds = m_catchupSession.timelinePositionSeconds(),
        .standbyPosition = standbyPlayer->position(),
        .standbyRange = standbyPlayer->demuxerSeekableRangeSeconds(),
        .standbyExhausted = m_catchupStandbyStreamSession && m_catchupStandbyStreamSession->providerConnectionClosed()
            && m_catchupStandbyStreamSession->bufferedBytes() == 0,
    });
    using Cutover = Playback::CatchupStandbyTransition::CutoverDecision::Action;
    if (decision.action == Cutover::Reject)
        markSeamlessStandbyAttemptFailed(QStringLiteral("standby snapshot has no media beyond watched position"));
    if (decision.action == Cutover::Seek) {
        standbyPlayer->seekAbsoluteExact(decision.target);
        standbyPlayer->setPaused(false);
    }
    if (decision.action != Cutover::Commit) return false;

    const auto previousPlaybackUrl = m_currentPlaybackUrl;
    const auto effectiveVolume = (m_muted || m_volume <= 0.0)
        ? 0
        : static_cast<int>(std::round(std::clamp(m_volume, 0.0, 100.0)));
    standbyPlayer->setAudioEnabled(true);
    standbyPlayer->setVolume(effectiveVolume);

    const auto standbyCacheDuration = standbyPlayer->demuxerCacheDurationSeconds();
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral(
            "Catch-up seamless cutover committing (%1): active=%2 standby=%3 remaining=%4s active-cache=%5 "
            "standby-cache=%6 active-provider-closed=%7 standby-url=%8 forced=%9.")
            .arg(reason)
            .arg(reinterpret_cast<quintptr>(activePlayer), 0, 16)
            .arg(reinterpret_cast<quintptr>(standbyPlayer), 0, 16)
            .arg(remainingSeconds, 0, 'f', 3)
            .arg(
                activeCacheDuration.has_value()
                    ? QString::number(activeCacheDuration.value(), 'f', 2)
                    : QStringLiteral("N/A"))
            .arg(
                standbyCacheDuration.has_value()
                    ? QString::number(standbyCacheDuration.value(), 'f', 2)
                    : QStringLiteral("N/A"))
            .arg(activeCatchupProviderConnectionClosed() ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(Core::redactSensitiveUrl(m_catchupSession.standby().url()))
            .arg(forceWithoutNearEdge ? QStringLiteral("yes") : QStringLiteral("no")));

    m_backendBuffering = false;
    clearPlaybackStallTracking();
    setIsBuffering(false);
    setIsLoading(false);
    setChannelSwitchInProgress(false);
    applyActiveCatchupBufferingPolicy(standbyPlayer);
    configurePlaybackTrackPreferences(standbyPlayer);
    setSharedPlaybackPlayer(standbyPlayer, false);

    const QPointer<Player::MpvPlayer> oldActivePlayer = activePlayer;
    if (activePlayer != standbyPlayer) {
        activePlayer->setAudioEnabled(false);
        activePlayer->setVolume(0);
        const auto generation = m_playbackGeneration;
        QMetaObject::invokeMethod(this, [this, oldActivePlayer, generation]() {
            if (generation != m_playbackGeneration) return;
            auto *oldPlayer = oldActivePlayer.data();
            if (oldPlayer == nullptr || oldPlayer == playbackPlayer()) {
                return;
            }
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Stopping old playback player after seamless surface handoff: player=%1.")
                    .arg(reinterpret_cast<quintptr>(oldPlayer), 0, 16));
            oldPlayer->stop();
        }, Qt::QueuedConnection);
    }

    m_currentPlaybackUrl = m_catchupSession.standby().url();
    m_catchupSession.setActiveEofObserved(false);
    m_catchupSession.setStreamBaseOffsetSeconds(m_catchupSession.standby().baseOffset());
    // Standby already passed file-loaded and video-ready before this handoff.
    m_catchupSession.setProgressTransportReady(true);
    m_catchupSession.setProgressSeekTargetSeconds({});
    resetCatchupUrlLoadGuardState(false);
    if (m_catchupActiveStreamSession) {
        m_catchupActiveStreamSession->closeProviderConnection(QStringLiteral("seamless-cutover"));
    }
    m_catchupActiveStreamSession = std::move(m_catchupStandbyStreamSession);
    const auto standbyPosition = standbyPlayer->position();
    if (std::isfinite(standbyPosition) && standbyPosition >= 0.0) {
        m_catchupSession.setTimelinePositionSeconds(m_catchupSession.streamBaseOffsetSeconds() + standbyPosition);
    }
    syncCatchupTimelineState();
    m_catchupSession.setTransportEndTimelineSeconds(std::max(m_catchupSession.streamBaseOffsetSeconds(), m_catchupSession.timelineAvailableSeconds()));
    setCatchupTimelineNoticeText(QString {});
    abortSeamlessCatchupRolling(QStringLiteral("cutover"), false);
    if (previousPlaybackUrl != m_currentPlaybackUrl) {
        emit currentPlaybackUrlChanged();
    }

    refreshBufferingState();
    syncIsPlayingFromBackend();
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up seamless extension cutover committed (%1).").arg(reason));
    return true;
}

void PlayerController::setSeamlessStandbyPrewarmState(const bool active, Player::MpvPlayer *standbyPlayer)
{
    auto *nextPlayer = active ? standbyPlayer : nullptr;
    if (m_catchupSeamlessPrewarmPlayer == nextPlayer && m_catchupSeamlessPrewarmActive == active) {
        return;
    }
    m_catchupSeamlessPrewarmPlayer = nextPlayer;
    m_catchupSeamlessPrewarmActive = active;
    emit seamlessStandbyPlayerObjectChanged();
    emit seamlessStandbyPrewarmActiveChanged();
}

bool PlayerController::standbySeamlessVideoReady() const
{
    return m_catchupSession.standby().videoReady();
}

bool PlayerController::maybeStopCatchupAtProgrammeBoundary(
    const std::optional<double> currentStreamSeconds,
    const std::optional<double> remainingBufferedSeconds,
    const QString &reason)
{
    if (m_catchupSession.endless() || !inCatchupMode()
        || !m_catchupSession.programStartUtc().isValid()
        || !m_catchupSession.programStopUtc().isValid()
        || m_catchupSession.programStopUtc() <= m_catchupSession.programStartUtc()) {
        return false;
    }

    syncCatchupTimelineState();
    const auto programmeDurationSeconds =
        static_cast<double>(std::max<qint64>(0, m_catchupSession.programStartUtc().secsTo(m_catchupSession.programStopUtc())));
    const auto availableEdgeAtProgrammeStop =
        m_catchupSession.timelineAvailableEdgeEpochMs() >= m_catchupSession.programStopUtc().toMSecsSinceEpoch();
    const auto observedTimelinePositionSeconds = std::clamp(
        currentStreamSeconds.has_value() && std::isfinite(currentStreamSeconds.value())
            ? m_catchupSession.streamBaseOffsetSeconds() + currentStreamSeconds.value()
            : m_catchupSession.timelinePositionSeconds(),
        0.0,
        programmeDurationSeconds);
    const auto atProgrammeEnd =
        availableEdgeAtProgrammeStop && (programmeDurationSeconds - observedTimelinePositionSeconds) <= 1.0;
    if (!atProgrammeEnd) {
        return false;
    }
    if (remainingBufferedSeconds.has_value()
        && std::isfinite(remainingBufferedSeconds.value())
        && std::max(0.0, remainingBufferedSeconds.value()) >= 1.0) {
        return false;
    }
    if (m_catchupSession.programBoundaryReached()) {
        return true;
    }

    m_catchupSession.setProgramBoundaryReached(true);
    abortSeamlessCatchupRolling(QStringLiteral("programme-boundary"), true);
    refreshBufferingState();
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up playback reached programme boundary (%1); stopping playback.")
            .arg(reason.trimmed().isEmpty() ? QStringLiteral("unspecified") : reason.trimmed()));
    const auto generation = m_playbackGeneration;
    QMetaObject::invokeMethod(this, [this, generation]() {
        if (generation == m_playbackGeneration) stop();
    }, Qt::QueuedConnection);
    return true;
}

bool PlayerController::handleCatchupPlaybackEndedRecovery()
{
    if (!inCatchupMode()) {
        return false;
    }

    if (m_catchupSession.endless()) {
        syncCatchupTimelineState();
        if (m_channelLoadFailed || m_catchupSession.publicationWaiting() || m_userPausedManually) {
            return true;
        }
        if (m_catchupSession.timelineAvailableSeconds() - m_catchupSession.timelinePositionSeconds() <= 1.0) {
            m_catchupSession.waitForPublication(m_liveDeliveryClock.elapsed());
            setCatchupTimelineNoticeText(QStringLiteral("Waiting for the next archive segment..."));
            return true;
        }
    }

    if (maybeStopCatchupAtProgrammeBoundary(
            std::nullopt,
            std::nullopt,
            QStringLiteral("playback-ended-boundary"))) {
        return true;
    }

    if (advanceCatchupMediaPeriod()) {
        return true;
    }

    if (recoverFailedContinuousCatchup()) {
        return true;
    }

    if (m_catchupSession.standby().pending()) {
        if (m_catchupSession.standby().ready() && maybeCommitSeamlessCatchupCutover(QStringLiteral("playback-ended"))) {
            refreshBufferingState();
            return true;
        }
        if (m_catchupSession.standby().loadIssued()) {
            m_catchupSession.standby().deferFallback();
            if (!m_catchupSession.standby().fallbackTimer().isActive()) {
                m_catchupSession.standby().fallbackTimer().start();
            }
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up seamless extension waiting for standby readiness (fallback timeout=%1ms).")
                    .arg(kCatchupSeamlessFallbackTimeoutMs));
            refreshBufferingState();
            return true;
        }
    }

    if (extendCatchupRollingWindow(QStringLiteral("playback-ended"), false)) {
        refreshBufferingState();
        return true;
    }
    if (hardRestoreCatchupAtCurrentTimelinePoint(QStringLiteral("catchup-eof-fallback"))) {
        refreshBufferingState();
        return true;
    }
    // A bounded continuation failure remains a catch-up error for user retry;
    // do not fall through to the generic live reconnect loop.
    return m_catchupSession.endless() && m_channelLoadFailed;
}

bool PlayerController::extendCatchupRollingWindow(const QString &reason, const bool fromPredictiveTrigger)
{
    if (!inCatchupMode()
        || !m_currentChannel.has_value()
        || m_catchupSession.programBoundaryReached()
        || m_channelLoadFailed
        || m_buffering.startupPending()
        || m_catchupSession.reloadInFlight()
        || m_recovery.active()) {
        return false;
    }
    if (!m_catchupSession.programStartUtc().isValid()
        || !m_catchupSession.programStopUtc().isValid()
        || m_catchupSession.programStopUtc() <= m_catchupSession.programStartUtc()) {
        return false;
    }
    if (!m_catchupSession.endless() && QDateTime::currentDateTimeUtc() >= m_catchupSession.programStopUtc().addSecs(-1)) {
        return false;
    }
    if (m_catchupSession.canonicalPlaybackUrl().trimmed().isEmpty()
        || !canRegenerateCatchupUrl()) {
        return false;
    }
    if (m_sharedPlaybackPlayer && m_sharedPlaybackPlayer.data() != &m_catchupStandbyPlayer) {
        return false;
    }
    if (fromPredictiveTrigger && m_catchupSession.standby().pending()) {
        return false;
    }
    if (m_catchupSession.rollingBackoff(m_liveDeliveryClock.elapsed())) {
        return false;
    }

    if (m_catchupSession.rollingExhausted(m_liveDeliveryClock.elapsed())) {
        const auto retryNotice =
            QStringLiteral("Catch-up extension failed. Retry or return to live.");
        setCatchupTimelineNoticeText(retryNotice);
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up rolling extension aborted after %1 attempts in %2ms (%3).")
                .arg(m_catchupSession.rollingAttempts())
                .arg(kCatchupRollingRetryWindowMs)
                .arg(reason));
        return false;
    }

    syncCatchupTimelineState();
    const auto availableSeconds = std::max(0.0, m_catchupSession.timelineAvailableSeconds());
    const auto totalDurationSeconds =
        m_catchupSession.endless() ? static_cast<qint64>(std::ceil(availableSeconds)) + 1
                        : std::max<qint64>(1, m_catchupSession.programStartUtc().secsTo(m_catchupSession.programStopUtc()));
    if (availableSeconds <= 0.0 || totalDurationSeconds <= 0) {
        return false;
    }
    if (fromPredictiveTrigger && availableSeconds <= m_catchupSession.transportEndTimelineSeconds() + 1.0) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Catch-up rolling extension skipped (predictive): no new provider window yet (available=%1s transportEnd=%2s).")
                .arg(availableSeconds, 0, 'f', 3)
                .arg(m_catchupSession.transportEndTimelineSeconds(), 0, 'f', 3));
        return false;
    }

    const auto desiredDelaySeconds = std::clamp(m_catchupSession.desiredDelaySeconds(), 0.0, availableSeconds);
    auto targetTimelineSeconds = (m_catchupSession.endless() || m_catchupSession.continuousFallback())
        ? std::clamp(m_catchupSession.timelinePositionSeconds(), 0.0, availableSeconds)
        : std::max(0.0, availableSeconds - desiredDelaySeconds);
    if (fromPredictiveTrigger) {
        targetTimelineSeconds = std::max(0.0, targetTimelineSeconds - kCatchupRollingOverlapBiasSeconds);
    }
    targetTimelineSeconds = std::min(targetTimelineSeconds, std::max(0.0, static_cast<double>(totalDurationSeconds) - 1.0));

    double streamBaseOffsetSeconds = 0.0;
    const auto regeneratedUrl = regeneratedCatchupUrl(targetTimelineSeconds, &streamBaseOffsetSeconds);
    if (regeneratedUrl.trimmed().isEmpty()) {
        m_catchupSession.recordRollingAttempt(m_liveDeliveryClock.elapsed());
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Catch-up rolling extension URL regeneration failed (attempt %1/%2, reason=%3, target=%4s).")
                .arg(m_catchupSession.rollingAttempts())
                .arg(kCatchupRollingMaxRetries)
                .arg(reason)
                .arg(targetTimelineSeconds, 0, 'f', 3));
        return false;
    }

    m_catchupSession.recordRollingAttempt(m_liveDeliveryClock.elapsed());
    bool started = false;
    if (fromPredictiveTrigger && canUseSeamlessCatchupRolling()) {
        started = armSeamlessCatchupRollingExtension(
            reason,
            targetTimelineSeconds,
            regeneratedUrl,
            streamBaseOffsetSeconds);
    } else {
        if (m_catchupSession.endless() || m_catchupSession.continuousFallback()) {
            m_catchupSession.setContinuousRecoveryTarget(targetTimelineSeconds);
        }
        started = m_catchupSession.endless()
            ? reloadCatchupForTimelineSeek(targetTimelineSeconds)
            : beginCatchupTimelineReload(targetTimelineSeconds, regeneratedUrl, streamBaseOffsetSeconds);
    }
    if (!started) {
        m_catchupSession.setContinuousRecoveryTarget({});
        return false;
    }

    if (!fromPredictiveTrigger) {
        const auto rollingNotice = QStringLiteral("Extending catch-up window...");
        setCatchupTimelineNoticeText(rollingNotice);
    }
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up rolling extension started (reason=%1, predictive=%2, target=%3s, delay=%4s, attempt=%5/%6).")
            .arg(reason)
            .arg(fromPredictiveTrigger ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(targetTimelineSeconds, 0, 'f', 3)
            .arg(desiredDelaySeconds, 0, 'f', 3)
            .arg(m_catchupSession.rollingAttempts())
            .arg(kCatchupRollingMaxRetries));
    return true;
}

bool PlayerController::advanceCatchupRecoveryAlignment()
{
    auto *backend = playbackPlayer();
    const auto eof = backend->demuxerCacheReaderEof().value_or(false);
    const auto nextBase = m_catchupActiveStreamSession ? m_catchupActiveStreamSession->nextPeriodBaseSeconds() : std::nullopt;
    const auto failed = m_catchupActiveStreamSession && m_catchupActiveStreamSession->hasNetworkError();
    const auto decision = m_catchupSession.alignRecovery(backend->position(), backend->demuxerSeekableRangeSeconds(),
        failed, eof, nextBase, m_liveDeliveryClock.elapsed());
    using Action = Playback::CatchupPlaybackSession::AlignmentDecision::Action;
    switch (decision.action) {
    case Action::Inactive: return false;
    case Action::Complete:
        backend->setPaused(m_userPausedManually);
        refreshBufferingState();
        return false;
    case Action::Seek: backend->seekAbsoluteExact(decision.target); break;
    case Action::NextPeriod:
        if (advanceCatchupMediaPeriod()) break;
        [[fallthrough]];
    case Action::Restore:
        m_catchupSession.setAlignmentActive(false);
        m_catchupSession.setReconnectResumeStreamRelativeSeconds({});
        hardRestoreCatchupAtCurrentTimelinePoint(QStringLiteral("resume-point-unavailable"));
        break;
    case Action::Wait: break;
    }
    refreshBufferingState();
    return true;
}

bool PlayerController::recoverCatchupAtAutomaticEof(const bool readerEof)
{
    // keep-open pauses mpv at EOF without an end-file event. Recovery must run
    // before the paused-player early return, including a ready standby cutover.
    auto *activePlayer = playbackPlayer();
    return inCatchupMode() && !m_userPausedManually && !m_catchupSession.reloadInFlight()
        && readerEof && activePlayer->pauseState().value_or(false)
        && activePlayer->demuxerCacheDurationSeconds().value_or(0.0) <= kCatchupSeamlessCutoverRemainingSeconds
        && handleCatchupPlaybackEndedRecovery();
}

bool PlayerController::advanceCatchupMediaPeriod()
{
    if (!inCatchupMode() || !m_catchupActiveStreamSession) {
        return false;
    }
    const auto base = m_catchupActiveStreamSession->nextPeriodBaseSeconds();
    if (!base) {
        return false;
    }
    if (m_userPausedManually || m_catchupSession.reloadInFlight()) {
        return true;
    }
    m_catchupSession.setAlignmentActive(false);
    m_catchupSession.resetAlignmentSeek();
    m_catchupSession.setReconnectResumeStreamRelativeSeconds({});
    m_catchupSession.setContinuousRecoveryTarget({});
    m_catchupSession.setPeriodReload(true);
    Core::DebugLogger::instance().log(QStringLiteral("player"),
        QStringLiteral("Catch-up replacing demuxer at MPEG-TS media boundary: base=%1s; no HTTP reconnect or minute rewind.")
            .arg(*base, 0, 'f', 3));
    return beginCatchupTimelineReload(*base, m_currentPlaybackUrl, *base);
}

bool PlayerController::recoverFailedContinuousCatchup()
{
    if (!inCatchupMode() || !m_catchupActiveStreamSession
        || !m_catchupActiveStreamSession->failedMediaTransport() || m_catchupSession.reloadInFlight()
        || m_userPausedManually || m_channelLoadFailed) {
        return false;
    }
    auto *activePlayer = playbackPlayer();
    const auto cache = activePlayer->demuxerCacheDurationSeconds();
    if (cache.has_value() && std::isfinite(*cache) && *cache > 1.0) {
        return false; // Play the verified tail before replacing the failed transport.
    }
    const auto position = activePlayer->position();
    if (std::isfinite(position) && position >= 0.0) {
        m_catchupSession.setTimelinePositionSeconds(m_catchupSession.streamBaseOffsetSeconds() + position);
    }
    syncCatchupTimelineState();
    if (maybeStopCatchupAtProgrammeBoundary(position, cache, QStringLiteral("failed-continuous-tail"))) {
        return true;
    }
    if (m_catchupSession.endless() && m_catchupSession.timelineAvailableSeconds() - m_catchupSession.timelinePositionSeconds() <= 1.0) {
        return handleCatchupPlaybackEndedRecovery();
    }
    const auto target = std::clamp(m_catchupSession.timelinePositionSeconds(), 0.0, m_catchupSession.timelineAvailableSeconds());
    m_catchupSession.setContinuousRecoveryTarget(target);
    m_catchupSession.setContinuousFallback(true);
    Core::DebugLogger::instance().log(QStringLiteral("player"),
        QStringLiteral("Recovering failed catch-up transport at watched position %1s; stop-first, finite transport fallback.")
            .arg(target, 0, 'f', 3));
    if (reloadCatchupForTimelineSeek(target)) {
        return true;
    }
    m_catchupSession.setContinuousRecoveryTarget({});
    return false;
}

bool PlayerController::reloadCatchupForTimelineSeek(const double targetSeconds)
{
    auto *activePlayer = playbackPlayer();
    if (activePlayer == nullptr) {
        return false;
    }

    double streamBaseOffsetSeconds = 0.0;
    const auto regeneratedUrl = regeneratedCatchupUrl(targetSeconds, &streamBaseOffsetSeconds);
    if (regeneratedUrl.trimmed().isEmpty()) {
        return false;
    }

    if (m_recovery.active()) {
        stopReconnectLoop(QStringLiteral("catchup-seek-url-regenerate"));
    }

    if (const auto target = m_catchupSession.continuousRecoveryTarget(); target && m_catchupSession.endless()) {
        const auto watched = *target;
        if (!m_catchupSession.allowContinuation(watched)) {
            setChannelLoadFailed(true);
            setCatchupTimelineNoticeText(QStringLiteral("Catch-up continuation failed. Retry or return to live."));
            return false;
        }
    }
    if (!m_catchupSession.reloadInFlight()) {
        m_catchupSession.setPeriodReload(false);
    }
    m_catchupSession.setAlignmentActive(false);
    m_catchupSession.setReconnectResumeStreamRelativeSeconds({});
    const auto residualSeekSeconds = std::max(0.0, targetSeconds - streamBaseOffsetSeconds);
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral(
            "Catch-up timeline seek: mode=url-regenerate target=%1s streamBase=%2s residual=%3s current=%4s cachedDuration=%5s host=%6")
            .arg(targetSeconds, 0, 'f', 3)
            .arg(streamBaseOffsetSeconds, 0, 'f', 3)
            .arg(residualSeekSeconds, 0, 'f', 3)
            .arg(m_recovery.lastPosition(), 0, 'f', 3)
            .arg([activePlayer]() -> QString {
                const auto cacheDuration = activePlayer->demuxerCacheDurationSeconds();
                if (!cacheDuration.has_value()) {
                    return QStringLiteral("N/A");
                }
                    return QString::number(cacheDuration.value(), 'f', 3);
            }())
            .arg(QUrl(m_currentPlaybackUrl).host(QUrl::FullyDecoded)));
    return beginCatchupTimelineReload(targetSeconds, regeneratedUrl, streamBaseOffsetSeconds);
}

bool PlayerController::beginCatchupTimelineReload(
    const double targetSeconds,
    const QString &regeneratedUrl,
    const double streamBaseOffsetSeconds)
{
    auto *activePlayer = playbackPlayer();
    if (activePlayer == nullptr) {
        return false;
    }
    if (m_catchupSession.standby().pending()) {
        abortSeamlessCatchupRolling(QStringLiteral("timeline-reload"), true);
    }

    if (!m_catchupSession.beginReload(targetSeconds, regeneratedUrl, streamBaseOffsetSeconds)) return true;
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up timeline reload started: stop-first teardown wait <=%1ms.")
            .arg(kCatchupTimelineReloadAckTimeoutMs));
    activePlayer->stop();
    return true;
}

void PlayerController::runCatchupTimelineReload()
{
    ++m_playbackGeneration;
    auto *activePlayer = playbackPlayer();
    if (activePlayer == nullptr || m_catchupSession.reloadUrl().trimmed().isEmpty()) {
        return;
    }

    m_catchupSession.reloadStarted();
    const auto previousPlaybackUrl = m_currentPlaybackUrl;
    m_catchupSession.setProgressTransportReady(false);
    m_catchupSession.setProgressSeekTargetSeconds({});
    m_currentPlaybackUrl = m_catchupSession.reloadUrl();
    m_catchupSession.setActiveEofObserved(false);
    m_catchupSession.setStreamBaseOffsetSeconds(m_catchupSession.reloadBase());
    // Deliberate compromise: long seek starts from regenerated minute anchor without residual second-precision seek.
    m_catchupSession.setPendingStreamRelativeSeekSeconds(std::nullopt);
    m_catchupSession.seekStarted(m_liveDeliveryClock.elapsed());
    resetCatchupUrlLoadGuardState(true);
    m_buffering.setStartupPending(false);
    m_pauseAfterLoad = false;
    stopDeferredLoadingIndicator();
    setIsLoading(false);
    m_backendBuffering = true;
    clearPlaybackStallTracking();
    refreshBufferingState();

    activePlayer->setStartupBufferingStrictMode(false);
    activePlayer->setPaused(false);
    QString playbackUrl;
    if (m_catchupSession.periodReload() && m_catchupActiveStreamSession
        && m_catchupActiveStreamSession->advancePeriod()) {
        playbackUrl = m_catchupActiveStreamSession->virtualUrl();
        m_catchupSession.setTimelinePositionSeconds(m_catchupSession.streamBaseOffsetSeconds());
        m_catchupSession.setReconnectResumeStreamRelativeSeconds({});
    } else {
        if (m_catchupSession.periodReload()) {
            // A simultaneous delivery failure retains the normal anchor recovery.
            double reloadBase = 0.0;
            m_catchupSession.setReloadUrl(regeneratedCatchupUrl(m_catchupSession.streamBaseOffsetSeconds(), &reloadBase));
            m_catchupSession.setReloadBase(reloadBase);
            m_catchupSession.setStreamBaseOffsetSeconds(m_catchupSession.reloadBase());
            m_currentPlaybackUrl = m_catchupSession.reloadUrl();
        }
        playbackUrl = prepareCatchupStreamPlaybackUrl(activePlayer, m_catchupSession.reloadUrl(), false);
    }
    m_catchupSession.setPeriodReload(false);
    if (const auto target = m_catchupSession.continuousRecoveryTarget()) {
        const auto watched = *target;
        if (m_catchupSession.endless()) {
            // Transport replacement must not replay the minute before the
            // watched point. file-loaded waits for cache and aligns while paused.
            m_catchupSession.setReconnectResumeStreamRelativeSeconds(std::max(0.0, watched - m_catchupSession.streamBaseOffsetSeconds()));
            m_catchupSession.setTimelinePositionSeconds(watched);
        } else {
            m_catchupSession.setReconnectResumeStreamRelativeSeconds({});
            m_catchupSession.setTimelinePositionSeconds(m_catchupSession.streamBaseOffsetSeconds());
        }
        m_catchupSession.setContinuousRecoveryTarget({});
    }
    configurePlaybackTrackPreferences(activePlayer);
    activePlayer->play(
        playbackUrl,
        catchupLoadfileOptions(m_catchupSession.reloadBase()));
    if (previousPlaybackUrl != m_currentPlaybackUrl) {
        emit currentPlaybackUrlChanged();
    }
}

void PlayerController::processPendingCatchupTimelineReload()
{
    const auto queuedSeek = m_catchupSession.queuedSeek();
    if (!queuedSeek.has_value()) {
        return;
    }
    const auto targetSeconds = *queuedSeek;
    m_catchupSession.clearQueuedSeek();
    const auto boundedTarget = std::max(0.0, std::min(m_catchupSession.timelineAvailableSeconds(), targetSeconds));
    m_catchupSession.setTimelinePositionSeconds(boundedTarget);
    m_catchupSession.setDesiredDelaySeconds(std::max(0.0, m_catchupSession.timelineAvailableSeconds() - boundedTarget));
    m_catchupSession.setTimelineAtLiveEdge((m_catchupSession.timelineAvailableSeconds() - boundedTarget) <= 0.75);
    emit catchupTimelineChanged();
    if (shouldReloadCatchupForSeek(boundedTarget)) {
        reloadCatchupForTimelineSeek(boundedTarget);
        return;
    }
    const auto streamRelativeTarget = std::max(0.0, boundedTarget - m_catchupSession.streamBaseOffsetSeconds());
    m_catchupSession.setProgressSeekTargetSeconds(boundedTarget);
    m_catchupSession.seekStarted(m_liveDeliveryClock.elapsed());
    playbackPlayer()->seekAbsoluteFast(streamRelativeTarget);
}

void PlayerController::finishCatchupTimelineReload(const QString &reason)
{
    m_catchupSession.finishReload();
    m_catchupSession.recoveryStarted(m_liveDeliveryClock.elapsed());
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up timeline reload finished: %1.").arg(reason));
    processPendingCatchupTimelineReload();
}

bool PlayerController::hardRestoreCatchupAtCurrentTimelinePoint(const QString &reason)
{
    if (!inCatchupMode() || playbackPlayer() == nullptr || m_channelLoadFailed) {
        return false;
    }
    if (m_catchupSession.reloadInFlight()) {
        return false;
    }
    abortSeamlessCatchupRolling(QStringLiteral("hard-restore"), true);

    const auto targetSeconds = std::max(0.0, std::min(m_catchupSession.timelineAvailableSeconds(), m_catchupSession.timelinePositionSeconds()));
    if (canRegenerateCatchupUrl()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up hard restore (%1): regenerate provider range at timeline=%2s.")
                .arg(reason)
                .arg(targetSeconds, 0, 'f', 3));
        if (m_catchupSession.endless() || m_catchupSession.continuousFallback()
            || (m_catchupActiveStreamSession && m_catchupActiveStreamSession->failedForwardGap())) {
            m_catchupSession.setContinuousRecoveryTarget(targetSeconds);
        }
        const auto reloading = reloadCatchupForTimelineSeek(targetSeconds);
        if (!reloading) {
            m_catchupSession.setContinuousRecoveryTarget({});
        }
        return reloading;
    }

    auto *activePlayer = playbackPlayer();
    if (activePlayer == nullptr) {
        return false;
    }
    if (m_recovery.active()) {
        stopReconnectLoop(QStringLiteral("catchup-hard-restore"));
    }

    const auto restoreUrl = m_currentPlaybackUrl.trimmed();
    if (restoreUrl.isEmpty()) {
        return false;
    }

    const auto previousPlaybackUrl = m_currentPlaybackUrl;
    m_currentPlaybackUrl = restoreUrl;
    m_buffering.setStartupPending(false);
    m_pauseAfterLoad = false;
    m_backendBuffering = true;
    clearPlaybackStallTracking();
    refreshBufferingState();

    activePlayer->setStartupBufferingStrictMode(false);
    activePlayer->setPaused(false);
    const auto playbackUrl = prepareCatchupStreamPlaybackUrl(activePlayer, restoreUrl, false);
    m_catchupSession.setProgressTransportReady(false);
    configurePlaybackTrackPreferences(activePlayer);
    activePlayer->play(playbackUrl, catchupLoadfileOptions(m_catchupSession.streamBaseOffsetSeconds()));
    if (previousPlaybackUrl != m_currentPlaybackUrl) {
        emit currentPlaybackUrlChanged();
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up hard restore (%1): reloaded current catch-up URL.")
            .arg(reason));
    m_catchupSession.recoveryStarted(m_liveDeliveryClock.elapsed());
    return true;
}

void PlayerController::evaluateCatchupDegradationRecovery(
    const std::optional<double> cacheDurationSeconds,
    const std::optional<double> cacheSpeedBytesPerSecond,
    const bool playbackAdvanced,
    const std::optional<bool> framePtsAdvanced)
{
    if (m_catchupActiveStreamSession
        && (m_catchupActiveStreamSession->continuous() || m_catchupActiveStreamSession->failedMediaTransport()
            || m_catchupActiveStreamSession->nextPeriodBaseSeconds().has_value())) {
        return; // The session owns continuation/waits; do not reset its mpv clock.
    }
    if (!inCatchupMode() || !m_currentChannel.has_value() || m_channelLoadFailed || m_buffering.startupPending()
        || m_catchupSession.reloadInFlight() || m_recovery.active()) {
        return;
    }
    if (m_catchupSession.timelineAvailableSeconds() <= 0.0) {
        return;
    }
    Q_UNUSED(framePtsAdvanced);
    Q_UNUSED(playbackAdvanced);
    if (m_catchupSession.nearZeroRecoveryDue(cacheDurationSeconds, cacheSpeedBytesPerSecond,
        m_backendBuffering || m_recovery.stalled(), m_liveDeliveryClock.elapsed())) {
        const auto remainingSeconds = std::max(0.0, m_catchupSession.timelineAvailableSeconds() - m_catchupSession.timelinePositionSeconds());
        const auto cacheSeconds = std::max(0.0, cacheDurationSeconds.value_or(0.0));
        const auto hasCacheSpeed = cacheSpeedBytesPerSecond && std::isfinite(*cacheSpeedBytesPerSecond) && *cacheSpeedBytesPerSecond >= 0.0;
        const auto cacheSpeed = hasCacheSpeed ? *cacheSpeedBytesPerSecond : -1.0;
        const auto fastRetryWindowOpen = m_catchupSession.standby().fastRetryWindowOpen(m_liveDeliveryClock.elapsed());
        const auto fastRetryInProgress = m_catchupSession.standby().pending()
            && !m_catchupSession.standby().loadIssued()
            && m_catchupSession.standby().retryPending()
            && m_catchupSession.standby().retryBudget() > 0
            && fastRetryWindowOpen;
        if (fastRetryInProgress) {
            const auto remainingMs = std::max<qint64>(
                0,
                static_cast<qint64>(kCatchupSeamlessStandbyFastRetryWindowMs)
                    - m_catchupSession.standby().fastRetryElapsed(m_liveDeliveryClock.elapsed()));
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up degradation watchdog (near-zero only): deferring hard restore while fast standby retry window is active (%1ms left, retries=%2).")
                    .arg(remainingMs)
                    .arg(m_catchupSession.standby().retryBudget()));
            return;
        }
        m_catchupSession.resetNearZero();
        if (m_catchupSession.standby().pending() && m_catchupSession.standby().ready()) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up degradation watchdog (near-zero only): standby is ready, attempting seamless cutover before hard restore (cache=%1s speed=%2B/s remaining=%3s).")
                    .arg(cacheSeconds, 0, 'f', 3)
                    .arg(hasCacheSpeed ? QString::number(cacheSpeed, 'f', 0) : QStringLiteral("N/A"))
                    .arg(remainingSeconds, 0, 'f', 3));
            if (maybeCommitSeamlessCatchupCutover(QStringLiteral("degradation-near-zero"), true)) {
                m_catchupSession.recoveryStarted(m_liveDeliveryClock.elapsed());
                return;
            }
            if (m_catchupSession.continuousFallback() && m_catchupSession.standby().pending()
                && (m_catchupSession.standby().loadIssued() || m_catchupSession.standby().retryPending())) {
                return; // Allow cache fill/alignment or retry; EOF fallback timeout remains the bound.
            }
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up degradation watchdog (near-zero only): seamless cutover unavailable, falling back to hard restore."));
        }
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Catch-up degradation watchdog (near-zero only): triggering hard restore (cache=%1s speed=%2B/s remaining=%3s).")
                .arg(cacheSeconds, 0, 'f', 3)
                .arg(hasCacheSpeed ? QString::number(cacheSpeed, 'f', 0) : QStringLiteral("N/A"))
                .arg(remainingSeconds, 0, 'f', 3));
        if (hardRestoreCatchupAtCurrentTimelinePoint(QStringLiteral("catchup-near-zero-watchdog"))) {
            m_catchupSession.recoveryStarted(m_liveDeliveryClock.elapsed());
        }
    }
}

void PlayerController::clearLiveBufferState()
{
    const auto changed = m_liveBufferActive
        || m_liveBufferWindowStartEpochMs != 0
        || m_liveBufferLiveEdgeEpochMs != 0
        || std::abs(m_liveBufferAvailableSeconds) > 0.0001
        || std::abs(m_liveBufferPositionSeconds) > 0.0001
        || std::abs(m_liveBufferBehindLiveSeconds) > 0.0001
        || !m_liveBufferAtLiveEdge;
    m_liveBufferActive = false;
    m_liveBufferWindowStartEpochMs = 0;
    m_liveBufferLiveEdgeEpochMs = 0;
    m_liveBufferAvailableSeconds = 0.0;
    m_liveBufferPositionSeconds = 0.0;
    m_liveBufferBehindLiveSeconds = 0.0;
    m_liveBufferAtLiveEdge = true;
    if (changed) {
        emit liveBufferStateChanged();
    }
}

void PlayerController::syncLiveBufferState()
{
    if (!m_currentChannel.has_value() || inCatchupMode() || timeshiftActive() || playbackPlayer() == nullptr) {
        clearLiveBufferState();
        return;
    }

    const auto currentPosition = playbackPlayer()->position();
    const auto seekableRange = playbackPlayer()->demuxerSeekableRangeSeconds();
    if (!seekableRange.has_value() || !std::isfinite(currentPosition) || currentPosition < 0.0) {
        clearLiveBufferState();
        return;
    }

    const auto rangeStart = std::max(0.0, seekableRange->first);
    const auto rangeEnd = std::max(rangeStart, seekableRange->second);
    const auto availableSeconds = std::max(0.0, rangeEnd - rangeStart);
    if (!std::isfinite(availableSeconds) || availableSeconds <= 0.05) {
        clearLiveBufferState();
        return;
    }

    const auto positionSeconds = std::clamp(currentPosition - rangeStart, 0.0, availableSeconds);
    const auto behindLiveSeconds = std::max(0.0, rangeEnd - currentPosition);
    const auto liveEdgeEpochMs = QDateTime::currentDateTimeUtc().addMSecs(
        -static_cast<qint64>(std::llround(behindLiveSeconds * 1000.0))).toMSecsSinceEpoch();
    const auto windowStartEpochMs = liveEdgeEpochMs - static_cast<qint64>(std::llround(availableSeconds * 1000.0));
    const auto atLiveEdge = behindLiveSeconds <= 0.75;

    const auto changed = !m_liveBufferActive
        || m_liveBufferWindowStartEpochMs != windowStartEpochMs
        || m_liveBufferLiveEdgeEpochMs != liveEdgeEpochMs
        || std::abs(m_liveBufferAvailableSeconds - availableSeconds) > 0.02
        || std::abs(m_liveBufferPositionSeconds - positionSeconds) > 0.02
        || std::abs(m_liveBufferBehindLiveSeconds - behindLiveSeconds) > 0.02
        || m_liveBufferAtLiveEdge != atLiveEdge;

    m_liveBufferActive = true;
    m_liveBufferWindowStartEpochMs = windowStartEpochMs;
    m_liveBufferLiveEdgeEpochMs = liveEdgeEpochMs;
    m_liveBufferAvailableSeconds = availableSeconds;
    m_liveBufferPositionSeconds = positionSeconds;
    m_liveBufferBehindLiveSeconds = behindLiveSeconds;
    m_liveBufferAtLiveEdge = atLiveEdge;
    if (changed) {
        emit liveBufferStateChanged();
    }
}

bool PlayerController::seekLiveBufferToPosition(const double targetSeconds)
{
    resetLiveReserve(true);
    if (!m_liveBufferActive || playbackPlayer() == nullptr) {
        return false;
    }
    const auto bounded = std::clamp(targetSeconds, 0.0, m_liveBufferAvailableSeconds);
    const auto seekableRange = playbackPlayer()->demuxerSeekableRangeSeconds();
    if (!seekableRange.has_value()) {
        return false;
    }
    const auto absoluteTarget = std::max(0.0, seekableRange->first + bounded);
    playbackPlayer()->seekAbsoluteFast(absoluteTarget);
    return true;
}

bool PlayerController::canRegenerateCatchupUrl() const
{
    return m_catchupSession.canRegenerateCatchupUrl(m_currentChannel);
}

QString PlayerController::regeneratedCatchupUrl(
    const double targetSeconds,
    double *streamBaseOffsetSeconds) const
{
    return m_catchupSession.regeneratedCatchupUrl(targetSeconds, streamBaseOffsetSeconds, m_currentChannel, QDateTime::currentDateTimeUtc());
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
    ++m_playbackGeneration;
    checkpointCatchupProgress();
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
    abortSeamlessCatchupRolling(QStringLiteral("channel-switch"), true);
    m_buffering.setStartupPending(false);
    m_pauseAfterLoad = false;
    setChannelLoadFailed(false);
    setIsLoading(false);
    m_backendBuffering = false;
    clearPlaybackStallTracking();
    clearLiveBufferState();
    refreshBufferingState();
    setIsPlaying(false);
    stopRecording();
    if (m_timeshiftController) {
        m_timeshiftController->handleUserChannelSwitchRequest(channel);
    }
    if (activePlayer == &m_catchupStandbyPlayer) {
        activePlayer->stop();
        setSharedPlaybackPlayer(nullptr, false);
        activePlayer = &m_player;
    } else if (m_sharedPlaybackPlayer && m_sharedPlaybackPlayer.data() == activePlayer) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Channel switch requested while shared playback is attached; stop-first teardown retained."));
        activePlayer->stop();
    } else {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Channel switch requested on primary live player; skipping explicit stop and relying on loadfile replace."));
    }

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
        if (!timeshiftPreparing() && !m_buffering.startupPending()) {
            setChannelSwitchInProgress(false);
        }
        return;
    }
    startPlaybackRequest(activePlayer, channel.streamUrl, false);
}

void PlayerController::playCatchupChannel(
    const Channel &channel,
// Preserve the established positional contract; parameter names identify their roles.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const QString &catchupUrl,
    const QString &programLabel,
    const QDateTime &programStartUtc,
    const QDateTime &programStopUtc,
    const QString &canonicalCatchupUrl,
// Preserve the established positional contract; parameter names identify their roles.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    std::optional<double> initialProgramSeekSeconds,
    std::optional<double> initialStreamBaseOffsetSeconds,
    std::optional<double> initialTimelinePositionSeconds,
    const int safetySeconds,
    const bool endless,
    std::optional<Core::EpgEntry> program)
{
    ++m_playbackGeneration;
    auto *activePlayer = playbackPlayer();
    if (activePlayer == nullptr) {
        return;
    }

    checkpointCatchupProgress();
    setChannelSwitchInProgress(true);
    stopPauseStateResync();
    stopDeferredLoadingIndicator();
    stopStartupBufferProbe();
    stopStartupBufferFallbackWatchdog(true);
    stopReconnectLoop(QStringLiteral("catchup-switch"));
    abortSeamlessCatchupRolling(QStringLiteral("catchup-switch"), true);
    m_buffering.setStartupPending(false);
    m_pauseAfterLoad = false;
    setChannelLoadFailed(false);
    setIsLoading(false);
    m_backendBuffering = false;
    clearPlaybackStallTracking();
    clearLiveBufferState();
    refreshBufferingState();
    setIsPlaying(false);
    stopRecording();
    if (m_timeshiftController) {
        m_timeshiftController->handleUserStopRequest();
    }
    activePlayer->stop();
    if (m_sharedPlaybackPlayer && activePlayer != &m_catchupStandbyPlayer) {
        // A provider session owns its transport and recovery players independently
        // of the legacy live PiP player that was previously attached here.
        detachSharedPlayback(false);
        activePlayer = &m_player;
    }

    if (activePlayer == &m_catchupStandbyPlayer) {
        setSharedPlaybackPlayer(nullptr, false);
        activePlayer = &m_player;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("catchup.play.start"),
        QStringLiteral("Switching to provider catch-up playback for %1.").arg(channel.name));
    m_catchupSession.setProgramStartUtc(programStartUtc.toUTC());
    m_catchupSession.setProgressTransportReady(false);
    m_catchupSession.setProgressSeekTargetSeconds({});
    m_catchupSession.setProgramStopUtc(programStopUtc.toUTC());
    m_catchupSession.setEndless(endless);
    m_catchupSession.resetContinuation();
    m_catchupSession.setValidatedProgram(program);
    m_catchupSession.setDisplayProgram(std::move(program));
    if (m_catchupActiveStreamSession) {
        m_catchupActiveStreamSession->closeProviderConnection(QStringLiteral("new-catchup-programme"));
        m_catchupActiveStreamSession.reset();
    }
    m_catchupSession.setSafetySeconds(std::clamp(safetySeconds, 0, 1800));
    m_catchupSession.setContinuousFallback(false);
    m_catchupSession.setAlignmentActive(false);
    m_catchupSession.setPeriodReload(false);
    m_catchupSession.setContinuousRecoveryTarget({});
    m_catchupSession.setProgramBoundaryReached(false);
    m_catchupSession.setActiveEofObserved(false);
    m_catchupSession.setStreamBaseOffsetSeconds(std::max(0.0, initialStreamBaseOffsetSeconds.value_or(0.0)));
    m_catchupSession.setTimelinePositionSeconds(std::max(0.0, initialTimelinePositionSeconds.value_or(0.0)));
    m_catchupSession.setPendingStreamRelativeSeekSeconds(std::nullopt);
    m_catchupSession.setReconnectResumeStreamRelativeSeconds(std::nullopt);
    m_catchupSession.setPendingInitialSeekSeconds(initialProgramSeekSeconds);
    m_catchupSession.setCanonicalPlaybackUrl(canonicalCatchupUrl.trimmed().isEmpty()
        ? catchupUrl.trimmed()
        : canonicalCatchupUrl.trimmed());
    resetCatchupUrlLoadGuardState(true);
    m_currentChannel = channel;
    setCatchupState(channel.streamUrl, programLabel);
    syncCatchupTimelineState();
    m_catchupSession.setDesiredDelaySeconds(std::max(0.0, m_catchupSession.timelineAvailableSeconds() - m_catchupSession.timelinePositionSeconds()));
    m_catchupSession.setTransportEndTimelineSeconds(std::max(m_catchupSession.streamBaseOffsetSeconds(), m_catchupSession.timelineAvailableSeconds()));
    m_catchupSession.resetRolling(true);
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
    const auto playbackUrl = prepareCatchupStreamPlaybackUrl(activePlayer, catchupUrl, false);
    startPlaybackRequest(activePlayer, playbackUrl, false, catchupLoadfileOptions());
}

void PlayerController::playCatchupChannel(const Channel &channel, const QString &catchupUrl, const QString &programLabel)
{
    const auto fallbackStart = QDateTime::currentDateTimeUtc();
    playCatchupChannel(channel, catchupUrl, programLabel, fallbackStart, fallbackStart.addSecs(60), catchupUrl, std::nullopt);
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
    if (!inCatchupMode()) {
        abortSeamlessCatchupRolling(QStringLiteral("play-current-url"), true);
        if (m_sharedPlaybackPlayer && m_sharedPlaybackPlayer.data() == &m_catchupStandbyPlayer) {
            setSharedPlaybackPlayer(nullptr, false);
        }
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
    checkpointCatchupProgress();
    if (!inCatchupMode() || !m_currentChannel.has_value()) {
        return;
    }

    const auto liveUrl = !m_catchupSession.livePlaybackUrlBeforeCatchup().trimmed().isEmpty()
        ? m_catchupSession.livePlaybackUrlBeforeCatchup().trimmed()
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
    abortSeamlessCatchupRolling(QStringLiteral("catchup-return-live"), true);
    m_buffering.setStartupPending(false);
    m_pauseAfterLoad = false;
    setChannelLoadFailed(false);
    setIsLoading(false);
    m_backendBuffering = false;
    clearPlaybackStallTracking();
    clearLiveBufferState();
    refreshBufferingState();
    setIsPlaying(false);
    stopRecording();
    if (m_timeshiftController) {
        m_timeshiftController->handleUserStopRequest();
    }
    activePlayer->stop();
    if (activePlayer == &m_catchupStandbyPlayer) {
        setSharedPlaybackPlayer(nullptr, false);
        activePlayer = &m_player;
    }

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
    checkpointCatchupProgress();
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
    ++m_playbackGeneration;
    checkpointCatchupProgress();
    if (m_sharedPlaybackPlayer && m_sharedPlaybackProtected) {
        detachSharedPlayback();
        return;
    }
    auto *activePlayer = playbackPlayer();
    m_pauseToggleRequested = false;
    m_userPausedManually = false;
    if (m_timeshiftController) {
        m_timeshiftController->handleUserStopRequest();
    }
    clearCatchupState();
    clearLiveBufferState();
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
    m_buffering.setStartupPending(false);
    m_pauseAfterLoad = false;
    setChannelSwitchInProgress(false);
    setChannelLoadFailed(false);
    if (activePlayer != nullptr) {
        activePlayer->stop();
    }
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
    checkpointCatchupProgress();
    // Promoted PiP/grid players remain externally owned, but the primary
    // transport still controls their pause state.
    if (m_recovery.active() && !inCatchupMode()) {
        stopReconnectLoop(QStringLiteral("manual-pause"));
        m_userPausedManually = true;
        m_pauseToggleRequested = true;
        playbackPlayer()->setPaused(true);
        refreshBufferingState();
        return;
    }
    if (m_buffering.liveReservePending()) {
        m_userPausedManually = true;
        m_pauseToggleRequested = true;
        resetLiveReserve(false);
        playbackPlayer()->setPaused(true);
        return;
    }
    if (m_catchupSession.alignmentActive()) {
        m_userPausedManually = !m_userPausedManually;
        refreshBufferingState();
        return;
    }
    if (m_buffering.catchupRefilling()) {
        resetCatchupRebuffering(false);
        m_userPausedManually = true;
        m_pauseToggleRequested = true;
        refreshBufferingState();
        return; // The user takes ownership of the pause; never auto-resume it.
    }
    if (!inCatchupMode()
        && m_timeshiftController
        && !m_timeshiftController->isActive()
        && m_timeshiftController->handlePauseRequest()) {
        return;
    }
    Core::DebugLogger::instance().log(QStringLiteral("player"), QStringLiteral("Toggle pause requested."));
    m_pauseToggleRequested = true;
    m_userPausedManually = !playbackPlayer()->pauseState().value_or(false);
    if (m_loadingIndicatorPending) {
        m_pauseAfterLoad = m_userPausedManually;
    }
    refreshBufferingState();
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
    resetLiveReserve(true);
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
        seekCatchupToTimelinePosition(m_catchupSession.timelineAvailableSeconds());
        return;
    }
    if (timeshiftActive() && m_timeshiftController) {
        m_timeshiftController->jumpToLiveEdge();
        return;
    }
    if (m_liveBufferActive) {
        // Leave the configured playback reserve instead of seeking into an empty cache.
        seekLiveBufferToPosition(std::max(0.0,
            m_liveBufferAvailableSeconds - playbackPlayer()->bufferTargetSeconds()));
    }
}

void PlayerController::seekTimeshiftRelative(const double seconds)
{
    if (inCatchupMode()) {
        seekCatchupToTimelinePosition(m_catchupSession.timelinePositionSeconds() + seconds);
        return;
    }
    if (timeshiftActive() && m_timeshiftController) {
        m_timeshiftController->seekRelative(seconds);
        return;
    }
    if (m_liveBufferActive) {
        seekLiveBufferToPosition(m_liveBufferPositionSeconds + seconds);
    }
}

void PlayerController::seekTimeshiftToFraction(const double fraction)
{
    if (inCatchupMode()) {
        const auto clamped = std::max(0.0, std::min(1.0, fraction));
        syncCatchupTimelineState();
        const auto targetMs = static_cast<double>(catchupTimelineStartEpochMs())
            + clamped * catchupTimelineDurationSeconds() * 1000.0;
        if (targetMs > static_cast<double>(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch())) {
            returnToLiveFromCatchup();
            return;
        }
        seekCatchupToTimelinePosition((targetMs - static_cast<double>(m_catchupSession.timelineStartEpochMs())) / 1000.0);
        return;
    }
    if (timeshiftActive() && m_timeshiftController) {
        m_timeshiftController->seekToFraction(fraction);
        return;
    }
    if (m_liveBufferActive) {
        const auto clamped = std::max(0.0, std::min(1.0, fraction));
        seekLiveBufferToPosition(clamped * m_liveBufferAvailableSeconds);
    }
}

void PlayerController::beginDeferredLoadingIndicator()
{
    m_loadingIndicatorPending = true;
    m_loadingPlaybackFileLoaded = false;
    m_loadingPlaybackReady = false;
    setIsLoading(false);
    m_loadingIndicatorDelayTimer.start();
}

void PlayerController::startStartupBufferFallbackWatchdog()
{
    if (!m_currentChannel.has_value() || inCatchupMode()) {
        return;
    }

    auto segmentSecondsHint = 0;
    if (timeshiftActive() && m_timeshiftController) {
        segmentSecondsHint = m_timeshiftController->configuredSegmentSeconds();
    }
    m_buffering.armStartup(playbackPlayer()->bufferTargetSeconds(), segmentSecondsHint);
    const auto timeoutMs = m_buffering.startupFallbackTimer().interval();
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Startup buffer watchdog armed: target=%1s timeout=%2ms segment-hint=%3s.")
            .arg(m_buffering.startupTarget(), 0, 'f', 1)
            .arg(timeoutMs)
            .arg(segmentSecondsHint > 0 ? QString::number(segmentSecondsHint) : QStringLiteral("N/A")));
}

void PlayerController::stopStartupBufferFallbackWatchdog(bool resetTuneState)
{
    m_buffering.resetStartupWatchdog(resetTuneState);
}

void PlayerController::startStartupBufferProbe()
{
    if (!m_buffering.startupProbeTimer().isActive()) {
        m_buffering.startupProbeTimer().start();
    }
}

void PlayerController::stopStartupBufferProbe()
{
    if (m_buffering.startupProbeTimer().isActive()) {
        m_buffering.startupProbeTimer().stop();
    }
}

void PlayerController::evaluateStartupBufferAndResumeIfReady()
{
    if (!m_currentChannel.has_value() || !m_buffering.startupPending() || inCatchupMode()) {
        stopStartupBufferProbe();
        return;
    }

    const auto cacheDuration = playbackPlayer()->demuxerCacheDurationSeconds();
    if (!cacheDuration.has_value() || !std::isfinite(cacheDuration.value())) {
        return;
    }

    const auto normalizedCacheDuration = std::max(0.0, cacheDuration.value());
    if (!m_buffering.startupReady(cacheDuration)) {
        return;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Startup buffer target reached (%1s >= %2s). Starting playback.")
            .arg(normalizedCacheDuration, 0, 'f', 2)
            .arg(m_buffering.startupTarget(), 0, 'f', 2));
    stopStartupBufferProbe();
    stopStartupBufferFallbackWatchdog(true);
    m_buffering.setStartupPending(false);
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
    if (!m_currentChannel.has_value() || m_buffering.startupFallbackApplied() || inCatchupMode()) {
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
    if (m_buffering.startupReady(cacheDuration)) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Startup buffer watchdog reached target naturally (%1s >= %2s); starting playback.")
                .arg(normalizedCacheDuration, 0, 'f', 2)
                .arg(m_buffering.startupTarget(), 0, 'f', 2));
        evaluateStartupBufferAndResumeIfReady();
        return;
    }

    const auto timeshiftActiveNow = timeshiftActive();
    const auto fallbackReason = hasCacheDuration
        ? QStringLiteral("cacheDuration=%1s < target=%2s")
              .arg(normalizedCacheDuration, 0, 'f', 2)
              .arg(m_buffering.startupTarget(), 0, 'f', 2)
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
    m_buffering.applyStartupFallback();
    m_buffering.setStartupPending(false);
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
    if (!m_isPlaying || !m_currentChannel.has_value() || m_recovery.active() || m_hwdecFallbackApplied
        || playbackPlayer() == nullptr) {
        return;
    }
    if (inCatchupMode()
        && m_catchupSession.seekStartedMs().has_value()
        && m_catchupSession.seekElapsed(m_liveDeliveryClock.elapsed()) < kCatchupSeekSettleMs) {
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
    } else {
        playbackPlayer()->setStartupBufferingStrictMode(true);
    }
    beginDeferredLoadingIndicator();
    m_buffering.setStartupPending(!catchupRetry);
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
    const auto playbackUrl = catchupRetry
        ? prepareCatchupStreamPlaybackUrl(playbackPlayer(), retryUrl, false)
        : retryUrl;
    m_catchupSession.setProgressTransportReady(false);
    configurePlaybackTrackPreferences(playbackPlayer());
    playbackPlayer()->play(playbackUrl, retryLoadfileOptions);
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
    m_recovery.clearProgress();
    m_recovery.clearWatchdogs();
}

void PlayerController::refreshBufferingState()
{
    const auto userPaused = m_userPausedManually;
    if (m_loadingIndicatorPending) {
        if (m_loadingPlaybackReady && !m_buffering.startupPending()) {
            stopDeferredLoadingIndicator();
            setIsLoading(false);
            setChannelSwitchInProgress(false);
        } else {
            setIsLoading(!userPaused && !m_loadingIndicatorDelayTimer.isActive());
        }
    }
    const auto reconnectNeedsSpinner = m_recovery.active()
        && (!m_recovery.stabilizing() || m_backendBuffering || m_recovery.stalled() || !m_isPlaying);
    setIsBuffering(!userPaused && (reconnectNeedsSpinner || m_buffering.liveReservePending() || m_catchupSession.alignmentActive() || m_buffering.catchupRefilling() || m_backendBuffering || m_recovery.stalled()));
}

void PlayerController::setIsPlaying(const bool value)
{
    if (m_isPlaying == value) {
        return;
    }

    m_isPlaying = value;
    if (m_isPlaying) {
        m_buffering.setStartupPending(false);
        if (!m_loadingIndicatorPending) {
            setChannelSwitchInProgress(false);
        }
        stopStartupBufferProbe();
        stopStartupBufferFallbackWatchdog(false);
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

    Core::DebugLogger::instance().log(
        QStringLiteral("player.state"),
        QStringLiteral("isLoading %1 -> %2 mode=%3 player=%4 url=%5.")
            .arg(m_isLoading ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(value ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(m_playbackMode)
            .arg(reinterpret_cast<quintptr>(playbackPlayer()), 0, 16)
            .arg(Core::redactSensitiveUrl(m_currentPlaybackUrl)));
    m_isLoading = value;
    emit isLoadingChanged();
}

void PlayerController::setIsBuffering(const bool value)
{
    if (m_isBuffering == value) {
        return;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("player.state"),
        QStringLiteral("isBuffering %1 -> %2 mode=%3 backend=%4 stalled=%5 player=%6 url=%7.")
            .arg(m_isBuffering ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(value ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(m_playbackMode)
            .arg(m_backendBuffering ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(m_recovery.stalled() ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(reinterpret_cast<quintptr>(playbackPlayer()), 0, 16)
            .arg(Core::redactSensitiveUrl(m_currentPlaybackUrl)));
    m_isBuffering = value;
    emit isBufferingChanged();
}

void PlayerController::setChannelSwitchInProgress(const bool value)
{
    if (m_channelSwitchInProgress == value) {
        return;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("player.state"),
        QStringLiteral("channelSwitchInProgress %1 -> %2 mode=%3 player=%4 url=%5.")
            .arg(m_channelSwitchInProgress ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(value ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(m_playbackMode)
            .arg(reinterpret_cast<quintptr>(playbackPlayer()), 0, 16)
            .arg(Core::redactSensitiveUrl(m_currentPlaybackUrl)));
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

void PlayerController::resetCatchupRebuffering(const bool resume)
{
    const auto decision = m_buffering.resetCatchupRefill(resume, m_userPausedManually);
    if (decision.pause == Playback::PauseCommand::Resume) playbackPlayer()->setPaused(false);
}

bool PlayerController::evaluateCatchupRebuffering(const std::optional<double> cacheSeconds, const bool readerEof)
{
    const auto healthySession = inCatchupMode() && m_catchupActiveStreamSession
        && m_catchupActiveStreamSession->continuous();
    const auto decision = m_buffering.catchupRefill(cacheSeconds, healthySession, readerEof, m_userPausedManually,
        m_isPlaying && !m_channelSwitchInProgress && !m_recovery.active(), m_liveDeliveryClock.elapsed());
    if (decision.pause != Playback::PauseCommand::None)
        playbackPlayer()->setPaused(decision.pause == Playback::PauseCommand::Pause);
    return decision.pending;
}

void PlayerController::checkpointCatchupProgress()
{
    if (!inCatchupMode()) {
        return;
    }
    auto *activePlayer = playbackPlayer();
    if (activePlayer != nullptr) {
        publishCatchupProgress(activePlayer->position());
    }
    emit catchupProgressFlushRequested();
}

void PlayerController::publishCatchupProgress(const double streamSeconds)
{
    // Only the active, settled transport may replace a saved bookmark. Timeline
    // targets are also set optimistically during URL resolution and seeks.
    if (!inCatchupMode() || !m_currentChannel.has_value() || !m_isPlaying || !m_catchupSession.progressTransportReady()
        || m_isLoading || m_channelSwitchInProgress || m_channelLoadFailed
        || m_catchupSession.reloadInFlight() || m_catchupSession.alignmentActive()
        || m_catchupSession.pendingInitialSeekSeconds().has_value()
        || m_catchupSession.pendingStreamRelativeSeekSeconds().has_value()
        || m_catchupSession.reconnectResumeStreamRelativeSeconds().has_value()
        || m_catchupSession.rollbackDeferred()
        || (m_catchupSession.seekStartedMs().has_value() && m_catchupSession.seekElapsed(m_liveDeliveryClock.elapsed()) < kCatchupSeekSettleMs)
        || !std::isfinite(streamSeconds) || streamSeconds < 0.0) {
        return;
    }
    const auto watched = m_catchupSession.observeProgress(streamSeconds, QDateTime::currentDateTimeUtc());
    if (watched) emit catchupProgressObserved({m_currentChannel.value(), m_catchupSession.programStartUtc(),
        m_catchupSession.programStopUtc(), *watched, m_catchupSession.endless()});
}

void PlayerController::updatePosition()
{
    if (advanceReconnectReserve() || m_recovery.reservePending()) {
        refreshBufferingState();
        return;
    }
    if (m_buffering.liveReservePending()) {
        refreshBufferingState();
        return;
    }
    if (inCatchupMode() && m_catchupSession.reloadInFlight()) {
        return;
    }
    auto *activePlayer = playbackPlayer();
    if (inCatchupMode() && m_catchupSession.endless() && (m_channelLoadFailed || m_catchupSession.publicationWaiting())) {
        syncCatchupTimelineState();
        if (!m_channelLoadFailed && !m_userPausedManually && m_catchupSession.publicationDue(m_liveDeliveryClock.elapsed())) {
            m_catchupSession.clearPublicationWait();
            handleCatchupPlaybackEndedRecovery();
        }
        return;
    }
    if (advanceCatchupRecoveryAlignment()) {
        return;
    }
    if (recoverFailedContinuousCatchup()) {
        return;
    }
    if (recoverCatchupAtAutomaticEof(activePlayer->demuxerCacheReaderEof().value_or(false))) {
        return;
    }
    if (evaluateCatchupRebuffering(activePlayer->demuxerCacheDurationSeconds(),
                                  activePlayer->demuxerCacheReaderEof().value_or(false))) {
        syncCatchupTimelineState();
        refreshBufferingState();
        return;
    }
    if (m_currentChannel.has_value()) {
        updateBitrateAverageBitsPerSecond(instantaneousBitrateBitsPerSecond(activePlayer));
    }
    syncLiveBufferState();
    const auto sampleStreamHealth = [activePlayer]() -> Playback::StreamHealth {
        Playback::StreamHealth health;
        if (const auto cacheSpeed = activePlayer->cacheSpeedBytesPerSecond();
            cacheSpeed.has_value() && std::isfinite(cacheSpeed.value()) && cacheSpeed.value() >= 0.0) {
            health.cacheSpeedBytesPerSecond = cacheSpeed;
        }
        health.cacheDurationSeconds = activePlayer->demuxerCacheDurationSeconds();
        health.bufferTargetSeconds = activePlayer->bufferTargetSeconds();
        return health;
    };

    const auto recoveryContext = [this, activePlayer]() {
        return Playback::RecoveryContext {
            .catchup = inCatchupMode(), .hasChannel = m_currentChannel.has_value(),
            .failed = m_channelLoadFailed, .manuallyPaused = m_userPausedManually,
            .startupPending = m_buffering.startupPending(), .buffering = m_backendBuffering,
            .stalled = m_recovery.stalled(), .hasVideo = activePlayer->videoCodec().has_value(),
            .tuneElapsedMs = m_tuneAttemptTimer.isValid() ? std::optional<qint64>(m_tuneAttemptTimer.elapsed()) : std::nullopt,
            .waitSeconds = m_waitForDataStreamSeconds,
        };
    };
    const auto sampleDisplayedFramePtsAdvanced = [this, activePlayer]() {
        return m_recovery.observeFrame(activePlayer->displayedVideoFramePtsSeconds());
    };
    const auto recover = [this](const QString &reason, bool noRefill) {
        if (reason.isEmpty()) return;
        if (m_timeshiftController && m_timeshiftController->isActive()
            && (m_timeshiftController->handlePlaybackStarvation(reason)
                || m_timeshiftController->handlePlaybackFailure(reason))) {
            if (noRefill) m_recovery.resetNoRefill();
            else m_recovery.resetVideoFreeze();
            return;
        }
        startReconnectLoop(reason);
    };
    const auto maybeStartNoRefillReconnect = [&](const Playback::StreamHealth &health, bool advanced, std::optional<bool> frame) {
        recover(m_recovery.noRefill(health, recoveryContext(), advanced, frame, m_liveDeliveryClock.elapsed()), true);
    };
    const auto maybeStartVideoFreezeReconnect = [&](const Playback::StreamHealth &health, bool advanced, std::optional<bool> frame) {
        recover(m_recovery.videoFreeze(health, recoveryContext(), advanced, frame, m_liveDeliveryClock.elapsed()), false);
    };
    const auto updateReconnectRecoveryWindow = [&](const Playback::StreamHealth &health, bool advanced, std::optional<bool> frame) {
        if (m_recovery.observeRecovery(health, recoveryContext(), advanced, frame, m_liveDeliveryClock.elapsed()))
            activePlayer->setStartupBufferingStrictMode(true);
    };

    if (!m_isPlaying) {
        if (!m_currentChannel.has_value()) {
            clearPlaybackStallTracking();
            m_userPausedManually = false;
            refreshBufferingState();
            return;
        }

        const auto seconds = activePlayer->position();
        const auto playbackAdvanced = m_recovery.observePosition(seconds, false);

        const auto health = sampleStreamHealth();
        maybeStartNoRefillReconnect(health, playbackAdvanced, std::nullopt);
        if (!m_recovery.active()) {
            m_recovery.resetStabilization();
        }
        m_recovery.resetVideoFreeze();
        m_recovery.observeFrame(std::nullopt);
        m_recovery.markPausedStall(m_userPausedManually);
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
        m_recovery.observePosition(seconds, false);
        const auto health = sampleStreamHealth();
        maybeStartNoRefillReconnect(health, false, std::nullopt);
        // Decoder/freeze watchdog requires a valid playback clock signal.
        // Streams that do not expose time-pos yet must not be classified as decoder stalls.
        m_recovery.resetVideoFreeze();
        if (!m_recovery.active()) {
            m_recovery.resetStabilization();
        }
        m_recovery.observeFrame(std::nullopt);
        refreshBufferingState();
        return;
    }

    if (inCatchupMode()) {
        maybeCorrectUnexpectedCatchupRollback(seconds);
    }
    playbackAdvanced = m_recovery.observePosition(seconds, true);
    if (playbackAdvanced) {
        if (inCatchupMode()
            && m_catchupSession.seekStartedMs().has_value()
            && m_catchupSession.seekElapsed(m_liveDeliveryClock.elapsed()) >= 0
            && m_catchupSession.seekElapsed(m_liveDeliveryClock.elapsed()) < kCatchupSeekSettleMs) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Catch-up seek settled: elapsedMs=%1 host=%2 streamPosition=%3s timelinePosition=%4s")
                    .arg(m_catchupSession.seekElapsed(m_liveDeliveryClock.elapsed()))
                    .arg(QUrl(m_currentPlaybackUrl).host(QUrl::FullyDecoded))
                    .arg(seconds, 0, 'f', 3)
                    .arg(m_catchupSession.streamBaseOffsetSeconds() + seconds, 0, 'f', 3));
            m_catchupSession.clearSeekSettling();
        }
    }
    const auto framePtsAdvanced = sampleDisplayedFramePtsAdvanced();
    const auto health = sampleStreamHealth();
    if (inCatchupMode()) {
        m_catchupSession.setTimelinePositionSeconds(m_catchupSession.streamBaseOffsetSeconds() + seconds);
        syncCatchupTimelineState();
        publishCatchupProgress(seconds);
        if (m_catchupActiveStreamSession && m_catchupActiveStreamSession->continuous()
            && maybeStopCatchupAtProgrammeBoundary(seconds, std::nullopt, QStringLiteral("continuous-programme-end"))) {
            return;
        }
    }
    const auto sampledPlaybackUrl = m_currentPlaybackUrl;
    const auto sampledStreamBase = m_catchupSession.streamBaseOffsetSeconds();
    evaluateCatchupDegradationRecovery(
        health.cacheDurationSeconds,
        health.cacheSpeedBytesPerSecond,
        playbackAdvanced,
        framePtsAdvanced);
    // Recovery can replace the transport. Its predecessor's clock/cache/EOF samples
    // must not drive the new timeline or trigger another recovery in this tick.
    if (playbackPlayer() != activePlayer || m_catchupSession.reloadInFlight()
        || m_currentPlaybackUrl != sampledPlaybackUrl
        || m_catchupSession.streamBaseOffsetSeconds() != sampledStreamBase) {
        refreshBufferingState();
        return;
    }
    maybeRetuneCatchupBuffering(health.cacheDurationSeconds);
    maybeRetuneSteadyStateBuffering(health.cacheDurationSeconds, health.bufferTargetSeconds);
    maybeStartNoRefillReconnect(health, playbackAdvanced, framePtsAdvanced);
    maybeStartVideoFreezeReconnect(health, playbackAdvanced, framePtsAdvanced);
    updateReconnectRecoveryWindow(health, playbackAdvanced, framePtsAdvanced);
    refreshBufferingState();
    evaluateReconnectRecovery();

    if (inCatchupMode() && !(m_catchupActiveStreamSession
        && (m_catchupActiveStreamSession->continuous() || m_catchupActiveStreamSession->failedMediaTransport()
            || m_catchupActiveStreamSession->nextPeriodBaseSeconds().has_value()))) {
        if (shouldExtendCatchupRollingWindowPredictively(seconds)) {
            extendCatchupRollingWindow(QStringLiteral("predictive-near-edge"), true);
        }
        const auto eofReached = activePlayer->demuxerCacheReaderEof().value_or(false);
        if (eofReached
            && maybeStopCatchupAtProgrammeBoundary(
                seconds,
                health.cacheDurationSeconds,
                QStringLiteral("demuxer-eof-boundary"))) {
            refreshBufferingState();
            return;
        }
        if (eofReached && !m_catchupSession.activeEofObserved()) {
            m_catchupSession.setActiveEofObserved(true);
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Catch-up active transport reached demuxer EOF; awaiting provider close confirmation."));
        }
        const auto providerClosed = activeCatchupProviderConnectionClosed();
        if (m_catchupSession.activeEofObserved() && m_catchupActiveStreamSession && !m_catchupActiveStreamSession->closeRequestedByApp()) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up demuxer EOF observed; requesting owned provider close before seamless rollover arm."));
            m_catchupActiveStreamSession->closeProviderConnection(QStringLiteral("active-eof-seamless"));
        }
        if (!m_catchupSession.standby().pending()
            && canUseSeamlessCatchupRolling()
            && m_catchupSession.activeEofObserved()
            && providerClosed) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up seamless rollover arm conditions met (EOF observed, provider closed)."));
            extendCatchupRollingWindow(QStringLiteral("eof-close-cache-threshold"), true);
        }
        if (m_catchupSession.standby().pending()
            && !m_catchupSession.standby().loadIssued()
            && canUseSeamlessCatchupRolling()
            && m_catchupSession.activeEofObserved()
            && providerClosed) {
            if (!m_catchupSession.standby().delayPending() && !m_catchupSession.standby().delayTimer().isActive()) {
                m_catchupSession.standby().beginDelay();
                m_catchupSession.standby().delayTimer().start();
                Core::DebugLogger::instance().log(
                    QStringLiteral("player"),
                    QStringLiteral(
                        "Catch-up seamless standby warmup delayed by %1ms after EOF/provider-close confirmation.")
                        .arg(kCatchupSeamlessPostCloseDelayMs));
            }
        }
        if (m_catchupSession.standby().pending() && m_catchupSession.standby().ready()) {
            maybeCommitSeamlessCatchupCutover(QStringLiteral("near-edge"));
        }
    }

    const auto displaySeconds = inCatchupMode() ? catchupTimelinePositionSeconds() : seconds;
    const auto duration = QTime(0, 0).addSecs(static_cast<int>(displaySeconds));
    const auto formatted =
        displaySeconds >= 3600.0 ? duration.toString(QStringLiteral("h:mm:ss")) : duration.toString(QStringLiteral("mm:ss"));
    if (formatted != m_positionText) {
        m_positionText = formatted;
        emit positionTextChanged();
    }
}

} // namespace OKILTV::App
