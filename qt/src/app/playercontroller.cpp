#include "playercontroller.h"

#include "timeshiftcontroller.h"

#include "../core/appdatapaths.h"
#include "../core/debuglogger.h"
#include "../core/processutils.h"
#include "../core/redaction.h"

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

Player::CatchupStreamSession::HeaderList catchupRequestHeadersFromSettings(
    const QString &playerUserAgent,
    const QMap<QString, QString> &mpvOptions)
{
    Player::CatchupStreamSession::HeaderList headers;
    auto appendHeader = [&headers](QByteArray name, QByteArray value) {
        name = name.trimmed();
        value = value.trimmed();
        if (name.isEmpty() || value.isEmpty()) {
            return;
        }
        for (auto &header : headers) {
            if (header.first.compare(name, Qt::CaseInsensitive) == 0) {
                header.second = value;
                return;
            }
        }
        headers.append(qMakePair(std::move(name), std::move(value)));
    };

    const auto trimmedUserAgent = playerUserAgent.trimmed();
    if (!trimmedUserAgent.isEmpty()) {
        appendHeader(QByteArrayLiteral("User-Agent"), trimmedUserAgent.toUtf8());
    }

    const auto rawHeaderFields = mpvOptions.value(QStringLiteral("http-header-fields")).trimmed();
    if (!rawHeaderFields.isEmpty()) {
        const QRegularExpression splitPattern(
            QStringLiteral(",(?=\\s*[!#$%&'*+.^_`|~0-9A-Za-z-]+\\s*:)"));
        const auto headerEntries = rawHeaderFields.split(splitPattern, Qt::SkipEmptyParts);
        for (const auto &entry : headerEntries) {
            const auto separatorIndex = entry.indexOf(u':');
            if (separatorIndex <= 0) {
                continue;
            }
            appendHeader(
                entry.left(separatorIndex).trimmed().toUtf8(),
                entry.mid(separatorIndex + 1).trimmed().toUtf8());
        }
    }

    const auto referrer = mpvOptions.value(QStringLiteral("referrer")).trimmed();
    if (!referrer.isEmpty()) {
        appendHeader(QByteArrayLiteral("Referer"), referrer.toUtf8());
    }

    return headers;
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
constexpr int kReconnectTransportSettleMs = 450;
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
constexpr int kCatchupRecoveryNearZeroSustainTicks = 1;
constexpr double kCatchupRecoveryNearZeroSeconds = 1.0;
constexpr double kCatchupRecoveryLowCacheSpeedBytesPerSecond = 1024.0;
constexpr double kCatchupRecoveryProgramEndGuardSeconds = 90.0;
constexpr int kCatchupRecoveryCooldownMs = 5000;
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
constexpr double kCatchupActiveCacheHeadSeconds = 90.0;
constexpr double kCatchupActiveCacheRefillMarginSeconds = 5.0;
constexpr qint64 kCatchupActiveDemuxerMaxBytes = 96LL * 1024LL * 1024LL;
constexpr qint64 kCatchupActiveDemuxerMaxBackBytes = 32LL * 1024LL * 1024LL;
constexpr double kCatchupStandbyCacheHeadSeconds = 30.0;
constexpr double kCatchupStandbyCacheRefillMarginSeconds = 5.0;
constexpr qint64 kCatchupStandbyDemuxerMaxBytes = 32LL * 1024LL * 1024LL;
constexpr qint64 kCatchupStandbyDemuxerMaxBackBytes = 8LL * 1024LL * 1024LL;
constexpr qsizetype kCatchupOwnedQueueActiveHighWaterBytes = 16 * 1024 * 1024;
constexpr qsizetype kCatchupOwnedQueueActiveLowWaterBytes = 8 * 1024 * 1024;
constexpr qint64 kCatchupOwnedQueueActiveReplyReadBufferBytes = 16 * 1024 * 1024;
constexpr qsizetype kCatchupOwnedQueueStandbyHighWaterBytes = 8 * 1024 * 1024;
constexpr qsizetype kCatchupOwnedQueueStandbyLowWaterBytes = 4 * 1024 * 1024;
constexpr qint64 kCatchupOwnedQueueStandbyReplyReadBufferBytes = 8 * 1024 * 1024;
constexpr double kCatchupBackwardSeekWindowSeconds = 60.0;
constexpr double kCatchupDebugEffectiveCacheMaxSeconds = kCatchupActiveCacheHeadSeconds;
constexpr double kCatchupUnexpectedRollbackThresholdSeconds = 15.0;
constexpr qint64 kCatchupRollbackGuardWarmupMs = 90000;
constexpr qint64 kCatchupRollbackDeferredCorrectionTimeoutMs = 3500;
constexpr int kCatchupTimelineReloadAckTimeoutMs = 500;
constexpr int kCatchupTimelineNoticeAutoClearMs = 5000;
constexpr double kCatchupRollingOverlapBiasSeconds = 2.0;
constexpr double kCatchupMinElapsedSeconds = 10.0 * 60.0;
constexpr double kCatchupRollingPredictiveTriggerSeconds = 45.0;
constexpr int kCatchupRollingRetryWindowMs = 10000;
constexpr int kCatchupRollingMaxRetries = 3;
constexpr int kCatchupRollingMinAttemptSpacingMs = 900;
constexpr int kCatchupSeamlessFallbackTimeoutMs = 2500;
constexpr int kCatchupSeamlessStandbyRetryBackoffMs = 5000;
constexpr int kCatchupSeamlessStandbyStopAckTimeoutMs = 500;
constexpr int kCatchupSeamlessStandbyVideoReadyTimeoutMs = 400;
constexpr int kCatchupSeamlessStandbyFastRetryDelayMs = 250;
constexpr int kCatchupSeamlessStandbyFastRetryWindowMs = 1200;
constexpr int kCatchupSeamlessStandbyFastRetryMaxAttempts = 2;
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

Player::CatchupStreamSession::BufferingPolicy catchupOwnedStreamPolicy(const bool standby)
{
    if (standby) {
        return {
            .queueHighWaterBytes = kCatchupOwnedQueueStandbyHighWaterBytes,
            .queueLowWaterBytes = kCatchupOwnedQueueStandbyLowWaterBytes,
            .replyReadBufferBytes = kCatchupOwnedQueueStandbyReplyReadBufferBytes,
            .roleLabel = QStringLiteral("standby"),
        };
    }

    return {
        .queueHighWaterBytes = kCatchupOwnedQueueActiveHighWaterBytes,
        .queueLowWaterBytes = kCatchupOwnedQueueActiveLowWaterBytes,
        .replyReadBufferBytes = kCatchupOwnedQueueActiveReplyReadBufferBytes,
        .roleLabel = QStringLiteral("active"),
    };
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
    m_catchupTimelineReloadAckTimer.setSingleShot(true);
    m_catchupTimelineReloadAckTimer.setInterval(kCatchupTimelineReloadAckTimeoutMs);
    connect(&m_catchupTimelineReloadAckTimer, &QTimer::timeout, this, [this]() {
        if (!m_catchupTimelineReloadInFlight) {
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
    m_catchupSeamlessFallbackTimer.setSingleShot(true);
    m_catchupSeamlessFallbackTimer.setInterval(kCatchupSeamlessFallbackTimeoutMs);
    connect(&m_catchupSeamlessFallbackTimer, &QTimer::timeout, this, [this]() {
        if (!m_catchupSeamlessPending || !m_catchupSeamlessFallbackDeferred || !inCatchupMode()) {
            return;
        }
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Catch-up seamless extension standby timeout after %1ms; falling back to standard recovery.")
                .arg(kCatchupSeamlessFallbackTimeoutMs));
        m_catchupSeamlessFallbackDeferred = false;
        if (handleCatchupPlaybackEndedRecovery()) {
            return;
        }
        startReconnectLoop(QStringLiteral("catchup-seamless-fallback-timeout"));
        refreshBufferingState();
    });
    m_catchupSeamlessStandbyStopAckTimer.setSingleShot(true);
    m_catchupSeamlessStandbyStopAckTimer.setInterval(kCatchupSeamlessStandbyStopAckTimeoutMs);
    connect(&m_catchupSeamlessStandbyStopAckTimer, &QTimer::timeout, this, [this]() {
        if (!m_catchupSeamlessPending || !m_catchupSeamlessStandbyStopPending || m_catchupSeamlessStandbyLoadIssued) {
            return;
        }
        auto *standbyPlayer = m_catchupSeamlessStandbyPlayer.data();
        if (standbyPlayer == nullptr || standbyPlayer == playbackPlayer()) {
            m_catchupSeamlessStandbyStopPending = false;
            return;
        }
        m_catchupSeamlessStandbyStopPending = false;
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Catch-up seamless standby stop ack timed out after %1ms; proceeding with standby load.")
                .arg(kCatchupSeamlessStandbyStopAckTimeoutMs));
        launchSeamlessCatchupStandbyLoad(standbyPlayer);
    });
    m_catchupSeamlessStandbyVideoReadyTimeoutTimer.setSingleShot(true);
    m_catchupSeamlessStandbyVideoReadyTimeoutTimer.setInterval(kCatchupSeamlessStandbyVideoReadyTimeoutMs);
    connect(&m_catchupSeamlessStandbyVideoReadyTimeoutTimer, &QTimer::timeout, this, [this]() {
        if (!m_catchupSeamlessPending || !m_catchupSeamlessStandbyLoadIssued || m_catchupSeamlessStandbyVideoReady) {
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
    m_catchupSeamlessFastRetryTimer.setSingleShot(true);
    m_catchupSeamlessFastRetryTimer.setInterval(kCatchupSeamlessStandbyFastRetryDelayMs);
    connect(&m_catchupSeamlessFastRetryTimer, &QTimer::timeout, this, [this]() {
        if (!m_catchupSeamlessPending || m_catchupSeamlessStandbyLoadIssued || !canUseSeamlessCatchupRolling()) {
            return;
        }
        if (!m_catchupSeamlessFastRetryPending || m_catchupSeamlessFastRetryBudgetRemaining <= 0) {
            return;
        }
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up seamless standby fast retry timer fired; attempting immediate retry."));
        startSeamlessCatchupStandbyLoad();
    });
    m_catchupSeamlessPostCloseDelayTimer.setSingleShot(true);
    m_catchupSeamlessPostCloseDelayTimer.setInterval(kCatchupSeamlessPostCloseDelayMs);
    connect(&m_catchupSeamlessPostCloseDelayTimer, &QTimer::timeout, this, [this]() {
        if (!m_catchupSeamlessPending || !m_catchupSeamlessPostCloseDelayPending || !canUseSeamlessCatchupRolling()) {
            return;
        }
        m_catchupSeamlessPostCloseDelayPending = false;
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
            if (!m_catchupSeamlessPending || !m_catchupSeamlessStandbyLoadIssued) {
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
            if (!m_catchupSeamlessPending || !m_catchupSeamlessStandbyLoadIssued) {
                return;
            }
            if (m_catchupSeamlessStandbyPlayer.data() != observedPlayer) {
                return;
            }
            if (!standbyCatchupSessionHealthyForCutover()) {
                markSeamlessStandbyAttemptFailed(QStringLiteral("standby owned stream became unavailable before readiness"));
                return;
            }
            m_catchupSeamlessStandbyReady = true;
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Catch-up seamless standby playback restarted; standby ready."));
            if (!m_catchupSeamlessStandbyVideoReady && !m_catchupSeamlessStandbyVideoReadyTimeoutTimer.isActive()) {
                m_catchupSeamlessStandbyVideoReadyTimeoutTimer.start();
            }
            maybeCommitSeamlessCatchupCutover(QStringLiteral("standby-ready"));
        });
        connect(observedPlayer, &Player::MpvPlayer::videoReconfigured, this, [this, observedPlayer]() {
            if (!m_catchupSeamlessPending || !m_catchupSeamlessStandbyLoadIssued) {
                return;
            }
            if (m_catchupSeamlessStandbyPlayer.data() != observedPlayer) {
                return;
            }
            if (!standbyCatchupSessionHealthyForCutover()) {
                markSeamlessStandbyAttemptFailed(QStringLiteral("standby owned stream became unavailable before video-ready"));
                return;
            }
            m_catchupSeamlessStandbyVideoReady = true;
            m_catchupSeamlessStandbyVideoReadyTimeoutTimer.stop();
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Catch-up seamless standby video reconfigured; standby video ready."));
            maybeCommitSeamlessCatchupCutover(QStringLiteral("standby-video-ready"));
        });
        connect(observedPlayer, &Player::MpvPlayer::playbackStopped, this, [this, observedPlayer]() {
            if (!m_catchupSeamlessPending || !m_catchupSeamlessStandbyStopPending || m_catchupSeamlessStandbyLoadIssued) {
                return;
            }
            auto *standbyPlayer = m_catchupSeamlessStandbyPlayer.data();
            if (standbyPlayer == nullptr || standbyPlayer != observedPlayer || standbyPlayer == playbackPlayer()) {
                m_catchupSeamlessStandbyStopPending = false;
                return;
            }
            m_catchupSeamlessStandbyStopAckTimer.stop();
            m_catchupSeamlessStandbyStopPending = false;
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Catch-up seamless standby stop acknowledged; starting standby load."));
            launchSeamlessCatchupStandbyLoad(standbyPlayer);
        });
        connect(observedPlayer, &Player::MpvPlayer::errorOccurred, this, [this, observedPlayer](const QString &message) {
            if (!m_catchupSeamlessPending || !m_catchupSeamlessStandbyLoadIssued) {
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
            m_catchupActiveEofObserved = false;
            if (m_catchupReconnectResumeStreamRelativeSeconds.has_value()) {
                const auto resumeTarget = std::max(0.0, m_catchupReconnectResumeStreamRelativeSeconds.value());
                m_catchupReconnectResumeStreamRelativeSeconds = std::nullopt;
                m_catchupSeekSettleTimer.restart();
                playbackPlayer()->seekAbsoluteFast(resumeTarget);
                Core::DebugLogger::instance().log(
                    QStringLiteral("player"),
                    QStringLiteral("Catch-up reconnect loaded; restoring stream-relative position to %1s.")
                        .arg(resumeTarget, 0, 'f', 3));
            }
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
            if (m_catchupPendingInitialSeekSeconds.has_value()) {
                const auto requestedSeekSeconds = std::max(0.0, m_catchupPendingInitialSeekSeconds.value());
                m_catchupPendingInitialSeekSeconds = std::nullopt;
                if (requestedSeekSeconds > kPlaybackPositionEpsilon) {
                    seekCatchupToTimelinePosition(requestedSeekSeconds);
                }
            }
            m_catchupRollingExtensionRetryCount = 0;
            m_catchupRollingRetryWindowTimer.invalidate();
            m_catchupProgramBoundaryReached = false;
            syncCatchupTimelineState();
            m_catchupTransportEndTimelineSeconds =
                std::max(m_catchupStreamBaseOffsetSeconds, m_catchupTimelineAvailableSeconds);
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
    connect(player, &Player::MpvPlayer::playbackStopped, this, [this, player]() {
        if (playbackPlayer() != player) {
            return;
        }
        if (!m_catchupTimelineReloadInFlight) {
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

    const auto minBufferNeeded = activePlayer->bufferTargetSeconds();
    const auto minBufferNeededSeconds = std::isfinite(minBufferNeeded) && minBufferNeeded > 0.0 ? minBufferNeeded : 0.0;

    const auto averageBitrateBitsPerSecond = m_averageBitrateBitsPerSecond.has_value()
        ? m_averageBitrateBitsPerSecond
        : instantaneousBitrateBitsPerSecond(activePlayer);
    const auto bitrateText = averageBitrateBitsPerSecond.has_value()
        ? formatDebugBitrate(averageBitrateBitsPerSecond.value())
        : QStringLiteral("N/A");
    const auto liveForwardTargetSeconds = adaptiveSteadyStateCacheLimitSeconds(activePlayer->bufferTargetSeconds());
    const auto liveBackTargetSeconds = Player::MpvPlayer::steadyStateBackBufferSeconds();
    const auto liveMaxBytes = adaptiveSteadyStateMaxBytes(activePlayer->bufferTargetSeconds(), averageBitrateBitsPerSecond);
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
    const auto totalLiveWindowSeconds =
        adaptiveSteadyStateCacheLimitSeconds(bufferTargetSeconds) + Player::MpvPlayer::steadyStateBackBufferSeconds();
    if (!averageBitsPerSecond.has_value()
        || !std::isfinite(averageBitsPerSecond.value())
        || averageBitsPerSecond.value() <= 0.0) {
        return Player::MpvPlayer::demuxerMaxBytesForBufferSeconds(totalLiveWindowSeconds);
    }

    const auto dynamicBudgetBytes = static_cast<qint64>(std::llround(
        (averageBitsPerSecond.value() / 8.0) * totalLiveWindowSeconds * kAdaptiveSteadyStateMaxBytesSafetyMultiplier));
    const auto maxBudgetBytes = std::numeric_limits<qint64>::max();
    return std::clamp(dynamicBudgetBytes, kAdaptiveSteadyStateMinBytes, maxBudgetBytes);
}

qint64 PlayerController::adaptiveSteadyStateMaxBackBytes(const std::optional<double> averageBitsPerSecond)
{
    const auto backBufferSeconds = Player::MpvPlayer::steadyStateBackBufferSeconds();
    if (!averageBitsPerSecond.has_value()
        || !std::isfinite(averageBitsPerSecond.value())
        || averageBitsPerSecond.value() <= 0.0) {
        return Player::MpvPlayer::demuxerMaxBytesForBufferSeconds(backBufferSeconds);
    }

    const auto dynamicBudgetBytes = static_cast<qint64>(std::llround(
        (averageBitsPerSecond.value() / 8.0) * backBufferSeconds * kAdaptiveSteadyStateMaxBytesSafetyMultiplier));
    return std::clamp(dynamicBudgetBytes, kAdaptiveSteadyStateMinBytes, std::numeric_limits<qint64>::max());
}

qint64 PlayerController::adaptiveCatchupMaxBytes(const std::optional<double> averageBitsPerSecond)
{
    if (!averageBitsPerSecond.has_value()
        || !std::isfinite(averageBitsPerSecond.value())
        || averageBitsPerSecond.value() <= 0.0) {
        return kCatchupActiveDemuxerMaxBytes;
    }

    const auto dynamicBudgetBytes = static_cast<qint64>(std::llround(
        (averageBitsPerSecond.value() / 8.0) * kCatchupActiveCacheHeadSeconds * kAdaptiveSteadyStateMaxBytesSafetyMultiplier));
    return std::clamp(dynamicBudgetBytes, kAdaptiveSteadyStateMinBytes, std::numeric_limits<qint64>::max());
}

qint64 PlayerController::adaptiveCatchupMaxBackBytes(const std::optional<double> averageBitsPerSecond)
{
    if (!averageBitsPerSecond.has_value()
        || !std::isfinite(averageBitsPerSecond.value())
        || averageBitsPerSecond.value() <= 0.0) {
        return kCatchupActiveDemuxerMaxBackBytes;
    }

    const auto dynamicBudgetBytes = static_cast<qint64>(std::llround(
        (averageBitsPerSecond.value() / 8.0) * kCatchupStandbyCacheHeadSeconds * kAdaptiveSteadyStateMaxBytesSafetyMultiplier));
    return std::clamp(dynamicBudgetBytes, kAdaptiveSteadyStateMinBytes, std::numeric_limits<qint64>::max());
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
    if (inCatchupMode()) {
        abortSeamlessCatchupRolling(QStringLiteral("reconnect-start"), true);
    }

    m_reconnectActive = true;
    m_reconnectAttemptCount = 0;
    m_reconnectAttemptInFlight = false;
    m_reconnectStabilizing = false;
    m_reconnectAttemptIssuedTimer.invalidate();
    m_reconnectTransportStopIssued = false;
    m_reconnectTransportStopIssuedTimer.invalidate();
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
    m_reconnectTransportStopIssued = false;
    m_reconnectTransportStopIssuedTimer.invalidate();
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
    m_reconnectTransportStopIssued = false;
    m_reconnectTransportStopIssuedTimer.invalidate();
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
            if (!m_reconnectAttemptIssuedTimer.isValid()) {
                m_reconnectAttemptIssuedTimer.restart();
                return;
            }
            if (m_reconnectAttemptIssuedTimer.elapsed() >= waitTimeoutMs) {
                Core::DebugLogger::instance().log(
                    QStringLiteral("player"),
                    QStringLiteral(
                        "Reconnect stabilization timed out after %1 ms; forcing next sequential attempt.")
                        .arg(waitTimeoutMs));
                if (activePlayer != nullptr) {
                    activePlayer->stop();
                }
                clearReconnectAttemptInFlight(QStringLiteral("stabilization-timeout"));
                // Continue below and schedule next reconnect attempt.
            } else {
                return;
            }
        }
        if (!m_reconnectAttemptInFlight) {
            // Previous block may have resolved the attempt.
        } else {
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

    if (!m_reconnectTransportStopIssued) {
        if (inCatchupMode()) {
            const auto streamPositionBeforeStop = activePlayer->position();
            m_catchupReconnectResumeStreamRelativeSeconds = (streamPositionBeforeStop >= 0.0)
                ? std::optional<double>(streamPositionBeforeStop)
                : m_catchupReconnectResumeStreamRelativeSeconds;
        }
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Reconnect attempt preparing transport: stop previous playback and wait %1 ms.")
                .arg(kReconnectTransportSettleMs));
        activePlayer->stop();
        m_reconnectTransportStopIssued = true;
        m_reconnectTransportStopIssuedTimer.restart();
        return;
    }
    if (!m_reconnectTransportStopIssuedTimer.isValid()
        || m_reconnectTransportStopIssuedTimer.elapsed() < kReconnectTransportSettleMs) {
        return;
    }
    m_reconnectTransportStopIssued = false;
    m_reconnectTransportStopIssuedTimer.invalidate();

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
    if (inCatchupMode()) {
        if (!m_catchupReconnectResumeStreamRelativeSeconds.has_value()) {
            const auto streamPosition = activePlayer->position();
            m_catchupReconnectResumeStreamRelativeSeconds = (streamPosition >= 0.0)
                ? std::optional<double>(streamPosition)
                : std::nullopt;
        }
        resetCatchupUrlLoadGuardState(false);
    } else {
        m_catchupReconnectResumeStreamRelativeSeconds = std::nullopt;
    }
    activePlayer->setPaused(false);
    const auto playbackUrl = inCatchupMode()
        ? prepareCatchupStreamPlaybackUrl(activePlayer, url, false)
        : url;
    activePlayer->play(playbackUrl, loadfileOptions);
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
    m_lastAdaptiveDemuxerMaxBackBytes = -1;
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
    const auto maxBackBytes = adaptiveSteadyStateMaxBackBytes(m_averageBitrateBitsPerSecond);

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
                    * kAdaptiveSteadyStateBytesRetuneThresholdRatio)))
        || m_lastAdaptiveDemuxerMaxBackBytes <= 0
        || std::abs(static_cast<double>(maxBackBytes - m_lastAdaptiveDemuxerMaxBackBytes))
            >= static_cast<double>(std::max<qint64>(
                1,
                std::llround(std::abs(static_cast<double>(m_lastAdaptiveDemuxerMaxBackBytes))
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
        .maxBackBytes = maxBackBytes,
    });
    m_lastAdaptiveCacheLimitSeconds = cacheLimitSeconds;
    m_lastAdaptiveCacheHysteresisSeconds = hysteresisSeconds;
    m_lastAdaptiveDemuxerMaxBytes = maxBytes;
    m_lastAdaptiveDemuxerMaxBackBytes = maxBackBytes;
    m_steadyStateBufferRetuneTimer.restart();

    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral(
            "Adaptive steady-state buffering retune: avg-bitrate=%1 cache=%2 buffer=%3 limit=%4 hysteresis=%5 max-bytes=%6 max-back-bytes=%7.")
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
            .arg(maxBytes)
            .arg(maxBackBytes));
}

void PlayerController::maybeRetuneCatchupBuffering(const std::optional<double> cacheDurationSeconds)
{
    auto *activePlayer = playbackPlayer();
    if (!m_isPlaying
        || !inCatchupMode()
        || activePlayer == nullptr
        || !m_currentChannel.has_value()
        || m_resumePlaybackAfterLoad
        || m_reconnectActive
        || m_channelLoadFailed
        || m_userPausedManually) {
        return;
    }

    const auto cacheLimitSeconds = kCatchupActiveCacheHeadSeconds;
    const auto hysteresisSeconds = cacheLimitSeconds - kCatchupActiveCacheRefillMarginSeconds;
    const auto maxBytes = adaptiveCatchupMaxBytes(m_averageBitrateBitsPerSecond);
    const auto maxBackBytes = adaptiveCatchupMaxBackBytes(m_averageBitrateBitsPerSecond);

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
                    * kAdaptiveSteadyStateBytesRetuneThresholdRatio)))
        || m_lastAdaptiveDemuxerMaxBackBytes <= 0
        || std::abs(static_cast<double>(maxBackBytes - m_lastAdaptiveDemuxerMaxBackBytes))
            >= static_cast<double>(std::max<qint64>(
                1,
                std::llround(std::abs(static_cast<double>(m_lastAdaptiveDemuxerMaxBackBytes))
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

    activePlayer->setSteadyStateBufferingPolicy({
        .cacheLimitSeconds = cacheLimitSeconds,
        .hysteresisSeconds = hysteresisSeconds,
        .maxBytes = maxBytes,
        .maxBackBytes = maxBackBytes,
    });
    m_lastAdaptiveCacheLimitSeconds = cacheLimitSeconds;
    m_lastAdaptiveCacheHysteresisSeconds = hysteresisSeconds;
    m_lastAdaptiveDemuxerMaxBytes = maxBytes;
    m_lastAdaptiveDemuxerMaxBackBytes = maxBackBytes;
    m_steadyStateBufferRetuneTimer.restart();

    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral(
            "Adaptive catch-up buffering retune: avg-bitrate=%1 cache=%2 limit=%3 hysteresis=%4 max-bytes=%5 max-back-bytes=%6.")
            .arg(
                m_averageBitrateBitsPerSecond.has_value()
                    ? QString::number(m_averageBitrateBitsPerSecond.value(), 'f', 0)
                    : QStringLiteral("N/A"))
            .arg(
                cacheDurationSeconds.has_value() && std::isfinite(cacheDurationSeconds.value())
                    ? QString::number(std::max(0.0, cacheDurationSeconds.value()), 'f', 2)
                    : QStringLiteral("N/A"))
            .arg(cacheLimitSeconds, 0, 'f', 1)
            .arg(hysteresisSeconds, 0, 'f', 1)
            .arg(maxBytes)
            .arg(maxBackBytes));
}

void PlayerController::applyActiveCatchupBufferingPolicy(Player::MpvPlayer *player)
{
    if (player == nullptr) {
        return;
    }

    const auto cacheLimitSeconds = kCatchupActiveCacheHeadSeconds;
    const auto hysteresisSeconds = cacheLimitSeconds - kCatchupActiveCacheRefillMarginSeconds;
    const auto maxBytes = adaptiveCatchupMaxBytes(m_averageBitrateBitsPerSecond);
    const auto maxBackBytes = adaptiveCatchupMaxBackBytes(m_averageBitrateBitsPerSecond);
    player->setSteadyStateBufferingPolicy({
        .cacheLimitSeconds = cacheLimitSeconds,
        .hysteresisSeconds = hysteresisSeconds,
        .maxBytes = maxBytes,
        .maxBackBytes = maxBackBytes,
    });
    m_lastAdaptiveCacheLimitSeconds = cacheLimitSeconds;
    m_lastAdaptiveCacheHysteresisSeconds = hysteresisSeconds;
    m_lastAdaptiveDemuxerMaxBytes = maxBytes;
    m_lastAdaptiveDemuxerMaxBackBytes = maxBackBytes;
    m_steadyStateBufferRetuneTimer.restart();

    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral(
            "Applied active catch-up buffering policy immediately: player=%1 limit=%2 hysteresis=%3 max-bytes=%4 max-back-bytes=%5.")
            .arg(reinterpret_cast<quintptr>(player), 0, 16)
            .arg(cacheLimitSeconds, 0, 'f', 1)
            .arg(hysteresisSeconds, 0, 'f', 1)
            .arg(maxBytes)
            .arg(maxBackBytes));
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
    m_catchupRequestHeaders = catchupRequestHeadersFromSettings(playerUserAgent, mpvOptions);
    m_catchupStandbyPlayer.configureLibraryPath(mpvDllPath);
    m_catchupStandbyPlayer.configureOptions(mpvOptions);
    m_catchupStandbyPlayer.configurePlaybackTuning(m_waitForDataStreamSeconds, deinterlaceEnabled, bufferSizeSeconds);
    m_catchupStandbyPlayer.configureUserAgent(playerUserAgent);
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
    if (!inCatchupMode()) {
        resetCatchupDegradationRecoveryState();
    }
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

QString PlayerController::catchupLoadfileOptions(const double streamBaseOffsetSeconds, const bool standby) const
{
    const auto cacheTargetSeconds = standby ? kCatchupStandbyCacheHeadSeconds : kCatchupActiveCacheHeadSeconds;
    const auto cacheRefillFloorSeconds = cacheTargetSeconds
        - (standby ? kCatchupStandbyCacheRefillMarginSeconds : kCatchupActiveCacheRefillMarginSeconds);
    const auto demuxerMaxBytes = standby ? kCatchupStandbyDemuxerMaxBytes : kCatchupActiveDemuxerMaxBytes;
    const auto demuxerMaxBackBytes = standby ? kCatchupStandbyDemuxerMaxBackBytes : kCatchupActiveDemuxerMaxBackBytes;
    const auto sharedOptions = QStringLiteral(
        "force-seekable=yes,hr-seek=no,cache=yes,cache-pause=no,cache-pause-wait=0,cache-secs=%1,demuxer-readahead-secs=%2,demuxer-hysteresis-secs=%3,demuxer-seekable-cache=yes,demuxer-max-bytes=%4,demuxer-max-back-bytes=%5")
                                   .arg(cacheTargetSeconds, 0, 'f', 0)
                                   .arg(cacheTargetSeconds, 0, 'f', 0)
                                   .arg(cacheRefillFloorSeconds, 0, 'f', 0)
                                   .arg(demuxerMaxBytes)
                                   .arg(demuxerMaxBackBytes);
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
    m_catchupActiveEofObserved = false;
    m_catchupStreamBaseOffsetSeconds = 0.0;
    m_catchupDesiredDelaySeconds = 0.0;
    m_catchupTransportEndTimelineSeconds = 0.0;
    m_catchupPendingStreamRelativeSeekSeconds = std::nullopt;
    m_catchupReconnectResumeStreamRelativeSeconds = std::nullopt;
    m_catchupPendingInitialSeekSeconds = std::nullopt;
    m_catchupTimelineReloadAckTimer.stop();
    m_catchupTimelineReloadInFlight = false;
    m_catchupTimelineReloadUrl.clear();
    m_catchupTimelineReloadStreamBaseOffsetSeconds = 0.0;
    m_pendingCatchupTimelineReloadTargetSeconds = std::nullopt;
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
    m_catchupUrlLoadGuardTimer.invalidate();
    m_catchupLastObservedStreamSeconds = -1.0;
    m_catchupRollbackGuardConsumed = false;
    m_catchupRollbackInitialLoadContext = false;
    m_catchupRollbackDeferredPending = false;
    m_catchupRollbackDeferredTargetSeconds = -1.0;
    m_catchupRollbackDeferredTimer.invalidate();
    m_catchupSeamlessLastStandbyAttemptTimer.invalidate();
    m_catchupTimelineStartEpochMs = 0;
    m_catchupTimelineAvailableEdgeEpochMs = 0;
    m_catchupTimelineAvailableSeconds = 0.0;
    m_catchupTimelinePositionSeconds = 0.0;
    m_catchupTimelineAtLiveEdge = true;
    setCatchupTimelineNoticeText(QString {});

    if (m_playbackMode != previousMode) {
        emit playbackModeChanged();
    }
    if (m_catchupProgramLabel != previousLabel) {
        emit catchupProgramLabelChanged();
    }
    emit catchupTimelineChanged();
    emit timeshiftStateChanged();
}

void PlayerController::resetCatchupDegradationRecoveryState()
{
    m_catchupNearZeroTickCount = 0;
    m_catchupRecoveryCooldownTimer.invalidate();
    m_catchupRollingRetryWindowTimer.invalidate();
    m_catchupLastRollingExtensionAttemptTimer.invalidate();
    m_catchupRollingExtensionRetryCount = 0;
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

void PlayerController::resetCatchupUrlLoadGuardState(const bool initialLoadContext)
{
    m_catchupUrlLoadGuardTimer.restart();
    m_catchupLastObservedStreamSeconds = -1.0;
    m_catchupRollbackGuardConsumed = false;
    m_catchupRollbackInitialLoadContext = initialLoadContext;
    m_catchupRollbackDeferredPending = false;
    m_catchupRollbackDeferredTargetSeconds = -1.0;
    m_catchupRollbackDeferredTimer.invalidate();
}

void PlayerController::maybeCorrectUnexpectedCatchupRollback(const double currentStreamSeconds)
{
    if (!inCatchupMode() || currentStreamSeconds < 0.0) {
        return;
    }
    auto *activePlayer = playbackPlayer();
    if (m_catchupRollbackDeferredPending) {
        if (activePlayer == nullptr) {
            m_catchupRollbackDeferredPending = false;
            m_catchupRollbackDeferredTargetSeconds = -1.0;
            m_catchupRollbackDeferredTimer.invalidate();
            return;
        }
        if (!m_catchupRollbackDeferredTimer.isValid()
            || m_catchupRollbackDeferredTimer.elapsed() > kCatchupRollbackDeferredCorrectionTimeoutMs) {
            m_catchupRollbackDeferredPending = false;
            m_catchupRollbackDeferredTargetSeconds = -1.0;
            m_catchupRollbackDeferredTimer.invalidate();
            m_catchupLastObservedStreamSeconds = currentStreamSeconds;
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up rollback guard deferred correction expired after %1ms; continuing playback.")
                    .arg(kCatchupRollbackDeferredCorrectionTimeoutMs));
            return;
        }
        const auto seekableRange = activePlayer->demuxerSeekableRangeSeconds();
        if (seekableRange.has_value()) {
            const auto target = std::max(0.0, m_catchupRollbackDeferredTargetSeconds);
            const auto rangeStart = std::max(0.0, seekableRange->first);
            const auto rangeEnd = std::max(rangeStart, seekableRange->second);
            if (target + kPlaybackPositionEpsilon >= rangeStart
                && target <= rangeEnd - kCatchupCacheSeekSafetyMarginSeconds) {
                m_catchupSeekSettleTimer.restart();
                activePlayer->seekAbsoluteFast(target);
                m_catchupRollbackDeferredPending = false;
                m_catchupRollbackDeferredTargetSeconds = -1.0;
                m_catchupRollbackDeferredTimer.invalidate();
                Core::DebugLogger::instance().log(
                    QStringLiteral("player"),
                    QStringLiteral(
                        "Catch-up rollback guard deferred correction applied: target=%1s range=%2..%3s.")
                        .arg(target, 0, 'f', 3)
                        .arg(rangeStart, 0, 'f', 3)
                        .arg(rangeEnd, 0, 'f', 3));
                return;
            }
        }
    }
    if (m_catchupRollbackGuardConsumed || !m_catchupUrlLoadGuardTimer.isValid()
        || m_catchupUrlLoadGuardTimer.elapsed() > kCatchupRollbackGuardWarmupMs) {
        m_catchupLastObservedStreamSeconds = currentStreamSeconds;
        return;
    }
    if (m_catchupLastObservedStreamSeconds < 0.0) {
        m_catchupLastObservedStreamSeconds = currentStreamSeconds;
        return;
    }
    if (m_catchupSeekSettleTimer.isValid() && m_catchupSeekSettleTimer.elapsed() < kCatchupSeekSettleMs) {
        m_catchupLastObservedStreamSeconds = currentStreamSeconds;
        return;
    }
    const auto rollbackSeconds = m_catchupLastObservedStreamSeconds - currentStreamSeconds;
    if (rollbackSeconds < kCatchupUnexpectedRollbackThresholdSeconds) {
        m_catchupLastObservedStreamSeconds = std::max(m_catchupLastObservedStreamSeconds, currentStreamSeconds);
        return;
    }
    if (activePlayer == nullptr) {
        m_catchupLastObservedStreamSeconds = currentStreamSeconds;
        return;
    }
    if (m_catchupRollbackInitialLoadContext) {
        const auto seekableRange = activePlayer->demuxerSeekableRangeSeconds();
        const auto target = std::max(0.0, m_catchupLastObservedStreamSeconds);
        const auto seekableReady = seekableRange.has_value()
            && (target + kPlaybackPositionEpsilon >= std::max(0.0, seekableRange->first))
            && (target <= std::max(std::max(0.0, seekableRange->first), seekableRange->second) - kCatchupCacheSeekSafetyMarginSeconds);
        if (!seekableReady) {
            m_catchupRollbackDeferredPending = true;
            m_catchupRollbackDeferredTargetSeconds = target;
            m_catchupRollbackDeferredTimer.restart();
            m_catchupRollbackGuardConsumed = true;
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up rollback guard deferred correction armed: current=%1s target=%2s rollback=%3s seekable=%4.")
                    .arg(currentStreamSeconds, 0, 'f', 3)
                    .arg(target, 0, 'f', 3)
                    .arg(rollbackSeconds, 0, 'f', 3)
                    .arg(seekableRange.has_value() ? QStringLiteral("yes") : QStringLiteral("no")));
            return;
        }
    }
    m_catchupSeekSettleTimer.restart();
    activePlayer->seekAbsoluteFast(m_catchupLastObservedStreamSeconds);
    m_catchupRollbackGuardConsumed = true;
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up rollback guard corrected backward jump: current=%1s previous=%2s rollback=%3s.")
            .arg(currentStreamSeconds, 0, 'f', 3)
            .arg(m_catchupLastObservedStreamSeconds, 0, 'f', 3)
            .arg(rollbackSeconds, 0, 'f', 3));
}

bool PlayerController::seekCatchupToTimelinePosition(const double targetSeconds)
{
    if (!inCatchupMode()) {
        return false;
    }
    const auto bounded = std::max(0.0, std::min(m_catchupTimelineAvailableSeconds, targetSeconds));
    m_catchupTimelinePositionSeconds = bounded;
    m_catchupDesiredDelaySeconds = std::max(0.0, m_catchupTimelineAvailableSeconds - bounded);
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

bool PlayerController::shouldExtendCatchupRollingWindowPredictively(const double currentStreamSeconds) const
{
    if (!inCatchupMode()
        || !m_currentChannel.has_value()
        || m_catchupProgramBoundaryReached
        || m_channelLoadFailed
        || m_resumePlaybackAfterLoad
        || m_catchupTimelineReloadInFlight
        || m_reconnectActive) {
        return false;
    }
    if (!m_catchupProgramStartUtc.isValid()
        || !m_catchupProgramStopUtc.isValid()
        || m_catchupProgramStopUtc <= m_catchupProgramStartUtc) {
        return false;
    }
    if (QDateTime::currentDateTimeUtc() >= m_catchupProgramStopUtc.addSecs(-1)) {
        return false;
    }
    if (currentStreamSeconds < 0.0
        || m_catchupCanonicalPlaybackUrl.trimmed().isEmpty()
        || !matchXtreamTimeshiftUrl(m_catchupCanonicalPlaybackUrl).has_value()) {
        return false;
    }
    if (m_catchupLastRollingExtensionAttemptTimer.isValid()
        && m_catchupLastRollingExtensionAttemptTimer.elapsed() < kCatchupRollingMinAttemptSpacingMs) {
        return false;
    }

    const auto remainingSeconds = m_catchupTransportEndTimelineSeconds - m_catchupTimelinePositionSeconds;
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
    if (!seamlessCatchupRollingEnabled()) {
        return false;
    }
    if (!inCatchupMode()
        || !m_currentChannel.has_value()
        || m_catchupProgramBoundaryReached
        || m_channelLoadFailed
        || m_resumePlaybackAfterLoad
        || m_catchupTimelineReloadInFlight
        || m_reconnectActive) {
        return false;
    }
    if (!m_catchupProgramStartUtc.isValid()
        || !m_catchupProgramStopUtc.isValid()
        || m_catchupProgramStopUtc <= m_catchupProgramStartUtc) {
        return false;
    }
    if (QDateTime::currentDateTimeUtc() >= m_catchupProgramStopUtc.addSecs(-1)) {
        return false;
    }
    if (m_catchupCanonicalPlaybackUrl.trimmed().isEmpty()
        || !matchXtreamTimeshiftUrl(m_catchupCanonicalPlaybackUrl).has_value()) {
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

    m_catchupSeamlessPending = true;
    m_catchupSeamlessStandbyLoadIssued = false;
    m_catchupSeamlessStandbyReady = false;
    m_catchupSeamlessStandbyVideoReady = false;
    m_catchupSeamlessStandbyPlayer = seamlessCatchupStandbyPlayer();
    m_catchupSeamlessStandbyUrl = regeneratedUrl;
    m_catchupSeamlessStandbyStreamBaseOffsetSeconds = streamBaseOffsetSeconds;
    m_catchupSeamlessFallbackDeferred = false;
    m_catchupSeamlessStandbyStopPending = false;
    m_catchupSeamlessPostCloseDelayPending = false;
    m_catchupSeamlessFallbackTimer.stop();
    m_catchupSeamlessStandbyStopAckTimer.stop();
    m_catchupSeamlessStandbyVideoReadyTimeoutTimer.stop();
    m_catchupSeamlessFastRetryTimer.stop();
    m_catchupSeamlessPostCloseDelayTimer.stop();
    m_catchupSeamlessFastRetryPending = false;
    m_catchupSeamlessFastRetryBudgetRemaining = kCatchupSeamlessStandbyFastRetryMaxAttempts;
    m_catchupSeamlessFastRetryWindowTimer.invalidate();
    m_catchupSeamlessLastStandbyFailureTimer.invalidate();
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
    if (!m_catchupSeamlessPending || m_catchupSeamlessStandbyLoadIssued || m_catchupSeamlessStandbyReady) {
        return false;
    }
    if (!canUseSeamlessCatchupRolling()) {
        return false;
    }

    auto *standbyPlayer = m_catchupSeamlessStandbyPlayer.data();
    if (standbyPlayer == nullptr || standbyPlayer == playbackPlayer()) {
        return false;
    }
    if (!standbyCatchupSessionHealthyForCutover()) {
        markSeamlessStandbyAttemptFailed(QStringLiteral("standby owned stream unavailable before warmup start"));
        return false;
    }
    if (m_catchupSeamlessStandbyStopPending) {
        return true;
    }
    if (m_catchupSeamlessLastStandbyAttemptTimer.isValid()
        && m_catchupSeamlessLastStandbyAttemptTimer.elapsed() < kCatchupSeamlessStandbyRetryBackoffMs) {
        const auto fastRetryWindowOpen = m_catchupSeamlessFastRetryWindowTimer.isValid()
            && m_catchupSeamlessFastRetryWindowTimer.elapsed() < kCatchupSeamlessStandbyFastRetryWindowMs;
        const auto fastRetryDelaySatisfied = m_catchupSeamlessLastStandbyFailureTimer.isValid()
            && m_catchupSeamlessLastStandbyFailureTimer.elapsed() >= kCatchupSeamlessStandbyFastRetryDelayMs;
        const auto allowFastRetry = m_catchupSeamlessFastRetryPending
            && m_catchupSeamlessFastRetryBudgetRemaining > 0
            && fastRetryWindowOpen
            && fastRetryDelaySatisfied;
        if (!allowFastRetry) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Catch-up seamless standby retry backoff active (%1/%2ms); waiting before next attempt.")
                .arg(m_catchupSeamlessLastStandbyAttemptTimer.elapsed())
                .arg(kCatchupSeamlessStandbyRetryBackoffMs));
        return false;
        }
        --m_catchupSeamlessFastRetryBudgetRemaining;
        m_catchupSeamlessFastRetryPending = false;
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up seamless standby fast retry bypassing backoff (remaining=%1).")
                .arg(m_catchupSeamlessFastRetryBudgetRemaining));
    }

    m_catchupSeamlessStandbyStopPending = true;
    m_catchupSeamlessPostCloseDelayPending = false;
    standbyPlayer->stop();
    m_catchupSeamlessStandbyStopAckTimer.start();
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up seamless standby stop-first requested before next standby load."));
    return true;
}

bool PlayerController::launchSeamlessCatchupStandbyLoad(Player::MpvPlayer *standbyPlayer)
{
    if (!m_catchupSeamlessPending || m_catchupSeamlessStandbyLoadIssued || m_catchupSeamlessStandbyReady) {
        return false;
    }
    if (!canUseSeamlessCatchupRolling()) {
        return false;
    }
    if (standbyPlayer == nullptr || standbyPlayer == playbackPlayer()) {
        return false;
    }
    if (!standbyCatchupSessionHealthyForCutover()) {
        markSeamlessStandbyAttemptFailed(QStringLiteral("standby owned stream unavailable before loadfile"));
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
        prepareCatchupStreamPlaybackUrl(standbyPlayer, m_catchupSeamlessStandbyUrl, true);
    standbyPlayer->play(
        standbyPlaybackUrl,
        catchupLoadfileOptions(m_catchupSeamlessStandbyStreamBaseOffsetSeconds, true));
    m_catchupSeamlessStandbyLoadIssued = true;
    m_catchupSeamlessStandbyReady = false;
    m_catchupSeamlessStandbyVideoReady = false;
    m_catchupSeamlessStandbyStopPending = false;
    m_catchupSeamlessPostCloseDelayPending = false;
    m_catchupSeamlessStandbyStopAckTimer.stop();
    m_catchupSeamlessStandbyVideoReadyTimeoutTimer.stop();
    m_catchupSeamlessFastRetryTimer.stop();
    m_catchupSeamlessFastRetryPending = false;
    m_catchupSeamlessLastStandbyAttemptTimer.restart();
    setSeamlessStandbyPrewarmState(true, standbyPlayer);

    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up seamless standby load started (host=%1).")
            .arg(QUrl(m_catchupSeamlessStandbyUrl).host(QUrl::FullyDecoded)));
    return true;
}

bool PlayerController::refreshSeamlessCatchupStandbyRetryUrl()
{
    if (!m_catchupSeamlessPending || m_catchupSeamlessStandbyUrl.trimmed().isEmpty()) {
        return false;
    }
    if (!matchXtreamTimeshiftUrl(m_catchupCanonicalPlaybackUrl).has_value()) {
        return true;
    }

    syncCatchupTimelineState();
    if (!m_catchupProgramStartUtc.isValid()
        || !m_catchupProgramStopUtc.isValid()
        || m_catchupProgramStopUtc <= m_catchupProgramStartUtc) {
        return false;
    }

    const auto availableSeconds = std::max(0.0, m_catchupTimelineAvailableSeconds);
    const auto desiredDelaySeconds = std::clamp(m_catchupDesiredDelaySeconds, 0.0, availableSeconds);
    auto targetTimelineSeconds = std::max(0.0, availableSeconds - desiredDelaySeconds);
    const auto totalDurationSeconds =
        std::max<qint64>(1, m_catchupProgramStartUtc.secsTo(m_catchupProgramStopUtc));
    targetTimelineSeconds =
        std::min(targetTimelineSeconds, std::max(0.0, static_cast<double>(totalDurationSeconds) - 1.0));

    double streamBaseOffsetSeconds = 0.0;
    const auto refreshedUrl = regeneratedXtreamCatchupUrl(targetTimelineSeconds, &streamBaseOffsetSeconds);
    if (refreshedUrl.trimmed().isEmpty()) {
        return false;
    }

    const auto previousUrl = m_catchupSeamlessStandbyUrl;
    const auto previousStreamBase = m_catchupSeamlessStandbyStreamBaseOffsetSeconds;
    m_catchupSeamlessStandbyUrl = refreshedUrl;
    m_catchupSeamlessStandbyStreamBaseOffsetSeconds = streamBaseOffsetSeconds;

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
    const auto trimmedSource = sourceUrl.trimmed();
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

    auto session = Player::CatchupStreamSession::create(
        trimmedSource,
        m_catchupRequestHeaders,
        catchupOwnedStreamPolicy(standby));
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
        return false;
    }
    return true;
}

void PlayerController::markSeamlessStandbyAttemptFailed(const QString &reason)
{
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up seamless standby load failed: %1").arg(reason));
    m_catchupSeamlessStandbyLoadIssued = false;
    m_catchupSeamlessStandbyReady = false;
    m_catchupSeamlessStandbyVideoReady = false;
    m_catchupSeamlessStandbyStopPending = false;
    m_catchupSeamlessPostCloseDelayPending = false;
    m_catchupSeamlessStandbyVideoReadyTimeoutTimer.stop();
    m_catchupSeamlessStandbyStopAckTimer.stop();
    m_catchupSeamlessPostCloseDelayTimer.stop();
    if (!m_catchupSeamlessFastRetryWindowTimer.isValid()) {
        m_catchupSeamlessFastRetryWindowTimer.start();
    }
    m_catchupSeamlessLastStandbyFailureTimer.restart();
    if (m_catchupSeamlessFastRetryBudgetRemaining > 0) {
        m_catchupSeamlessFastRetryPending = true;
        m_catchupSeamlessFastRetryTimer.start();
    }
}

void PlayerController::abortSeamlessCatchupRolling(const QString &reason, const bool stopStandbyPlayer)
{
    auto *standbyPlayer = m_catchupSeamlessStandbyPlayer.data();
    const auto hadState = m_catchupSeamlessPending
        || m_catchupSeamlessStandbyLoadIssued
        || m_catchupSeamlessStandbyReady
        || m_catchupSeamlessFallbackDeferred
        || !m_catchupSeamlessStandbyUrl.isEmpty()
        || m_catchupSeamlessStandbyPlayer;
    m_catchupSeamlessFallbackTimer.stop();
    m_catchupSeamlessPending = false;
    m_catchupSeamlessStandbyLoadIssued = false;
    m_catchupSeamlessStandbyReady = false;
    m_catchupSeamlessStandbyVideoReady = false;
    m_catchupSeamlessStandbyStopPending = false;
    m_catchupSeamlessPostCloseDelayPending = false;
    m_catchupSeamlessStandbyPlayer = nullptr;
    m_catchupSeamlessStandbyUrl.clear();
    m_catchupSeamlessStandbyStreamBaseOffsetSeconds = 0.0;
    m_catchupSeamlessFallbackDeferred = false;
    m_catchupSeamlessStandbyStopAckTimer.stop();
    m_catchupSeamlessStandbyVideoReadyTimeoutTimer.stop();
    m_catchupSeamlessFastRetryTimer.stop();
    m_catchupSeamlessPostCloseDelayTimer.stop();
    m_catchupSeamlessFastRetryPending = false;
    m_catchupSeamlessFastRetryBudgetRemaining = 0;
    m_catchupSeamlessFastRetryWindowTimer.invalidate();
    m_catchupSeamlessLastStandbyFailureTimer.invalidate();
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
    if (!m_catchupSeamlessPending || !m_catchupSeamlessStandbyReady) {
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
    if (QDateTime::currentDateTimeUtc() >= m_catchupProgramStopUtc.addSecs(-1)) {
        return false;
    }

    const auto remainingSeconds = m_catchupTransportEndTimelineSeconds - m_catchupTimelinePositionSeconds;
    const auto nearTransportEnd = !std::isfinite(remainingSeconds)
        || remainingSeconds <= kCatchupSeamlessCutoverRemainingSeconds;
    if (!forceWithoutNearEdge && !nearTransportEnd && !m_catchupSeamlessFallbackDeferred) {
        return false;
    }

    const auto previousPlaybackUrl = m_currentPlaybackUrl;
    const auto effectiveVolume = (m_muted || m_volume <= 0.0)
        ? 0
        : static_cast<int>(std::round(std::clamp(m_volume, 0.0, 100.0)));
    standbyPlayer->setAudioEnabled(true);
    standbyPlayer->setVolume(effectiveVolume);

    const auto activeCacheDuration = activePlayer->demuxerCacheDurationSeconds();
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
            .arg(Core::redactSensitiveUrl(m_catchupSeamlessStandbyUrl))
            .arg(forceWithoutNearEdge ? QStringLiteral("yes") : QStringLiteral("no")));

    m_backendBuffering = false;
    clearPlaybackStallTracking();
    setIsBuffering(false);
    setIsLoading(false);
    setChannelSwitchInProgress(false);
    applyActiveCatchupBufferingPolicy(standbyPlayer);
    setSharedPlaybackPlayer(standbyPlayer, false);

    const QPointer<Player::MpvPlayer> oldActivePlayer = activePlayer;
    if (activePlayer != standbyPlayer) {
        activePlayer->setAudioEnabled(false);
        activePlayer->setVolume(0);
        QMetaObject::invokeMethod(this, [this, oldActivePlayer]() {
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

    m_currentPlaybackUrl = m_catchupSeamlessStandbyUrl;
    m_catchupActiveEofObserved = false;
    m_catchupStreamBaseOffsetSeconds = m_catchupSeamlessStandbyStreamBaseOffsetSeconds;
    if (m_catchupActiveStreamSession) {
        m_catchupActiveStreamSession->closeProviderConnection(QStringLiteral("seamless-cutover"));
    }
    m_catchupActiveStreamSession = std::move(m_catchupStandbyStreamSession);
    syncCatchupTimelineState();
    m_catchupTransportEndTimelineSeconds =
        std::max(m_catchupStreamBaseOffsetSeconds, m_catchupTimelineAvailableSeconds);
    setCatchupTimelineNoticeText(QString {});
    abortSeamlessCatchupRolling(QStringLiteral("cutover"), false);
    if (previousPlaybackUrl != m_currentPlaybackUrl) {
        emit currentPlaybackUrlChanged();
    }

    refreshBufferingState();
    syncIsPlayingFromBackend();
    if (!isPlaying()) {
        setIsPlaying(true);
    }
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
    return m_catchupSeamlessStandbyVideoReady;
}

bool PlayerController::maybeStopCatchupAtProgrammeBoundary(
    const std::optional<double> currentStreamSeconds,
    const std::optional<double> remainingBufferedSeconds,
    const QString &reason)
{
    if (!inCatchupMode()
        || !m_catchupProgramStartUtc.isValid()
        || !m_catchupProgramStopUtc.isValid()
        || m_catchupProgramStopUtc <= m_catchupProgramStartUtc) {
        return false;
    }

    syncCatchupTimelineState();
    const auto programmeDurationSeconds =
        static_cast<double>(std::max<qint64>(0, m_catchupProgramStartUtc.secsTo(m_catchupProgramStopUtc)));
    const auto availableEdgeAtProgrammeStop =
        m_catchupTimelineAvailableEdgeEpochMs >= m_catchupProgramStopUtc.toMSecsSinceEpoch();
    const auto observedTimelinePositionSeconds = std::clamp(
        currentStreamSeconds.has_value() && std::isfinite(currentStreamSeconds.value())
            ? m_catchupStreamBaseOffsetSeconds + currentStreamSeconds.value()
            : m_catchupTimelinePositionSeconds,
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
    if (m_catchupProgramBoundaryReached) {
        return true;
    }

    m_catchupProgramBoundaryReached = true;
    abortSeamlessCatchupRolling(QStringLiteral("programme-boundary"), true);
    refreshBufferingState();
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up playback reached programme boundary (%1); stopping playback.")
            .arg(reason.trimmed().isEmpty() ? QStringLiteral("unspecified") : reason.trimmed()));
    QMetaObject::invokeMethod(this, &PlayerController::stop, Qt::QueuedConnection);
    return true;
}

bool PlayerController::handleCatchupPlaybackEndedRecovery()
{
    if (!inCatchupMode()) {
        return false;
    }

    if (maybeStopCatchupAtProgrammeBoundary(
            std::nullopt,
            std::nullopt,
            QStringLiteral("playback-ended-boundary"))) {
        return true;
    }

    if (m_catchupSeamlessPending) {
        if (m_catchupSeamlessStandbyReady && maybeCommitSeamlessCatchupCutover(QStringLiteral("playback-ended"))) {
            refreshBufferingState();
            return true;
        }
        if (m_catchupSeamlessStandbyLoadIssued && !m_catchupSeamlessStandbyReady) {
            m_catchupSeamlessFallbackDeferred = true;
            if (!m_catchupSeamlessFallbackTimer.isActive()) {
                m_catchupSeamlessFallbackTimer.start();
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
    return false;
}

bool PlayerController::extendCatchupRollingWindow(const QString &reason, const bool fromPredictiveTrigger)
{
    if (!inCatchupMode()
        || !m_currentChannel.has_value()
        || m_catchupProgramBoundaryReached
        || m_channelLoadFailed
        || m_resumePlaybackAfterLoad
        || m_catchupTimelineReloadInFlight
        || m_reconnectActive) {
        return false;
    }
    if (!m_catchupProgramStartUtc.isValid()
        || !m_catchupProgramStopUtc.isValid()
        || m_catchupProgramStopUtc <= m_catchupProgramStartUtc) {
        return false;
    }
    if (QDateTime::currentDateTimeUtc() >= m_catchupProgramStopUtc.addSecs(-1)) {
        return false;
    }
    if (m_catchupCanonicalPlaybackUrl.trimmed().isEmpty()
        || !matchXtreamTimeshiftUrl(m_catchupCanonicalPlaybackUrl).has_value()) {
        return false;
    }
    if (m_sharedPlaybackPlayer && m_sharedPlaybackPlayer.data() != &m_catchupStandbyPlayer) {
        return false;
    }
    if (fromPredictiveTrigger && m_catchupSeamlessPending) {
        return false;
    }
    if (m_catchupLastRollingExtensionAttemptTimer.isValid()
        && m_catchupLastRollingExtensionAttemptTimer.elapsed() < kCatchupRollingMinAttemptSpacingMs) {
        return false;
    }

    if (!m_catchupRollingRetryWindowTimer.isValid()
        || m_catchupRollingRetryWindowTimer.elapsed() > kCatchupRollingRetryWindowMs) {
        m_catchupRollingExtensionRetryCount = 0;
        m_catchupRollingRetryWindowTimer.restart();
    }

    if (m_catchupRollingExtensionRetryCount >= kCatchupRollingMaxRetries) {
        const auto retryNotice =
            QStringLiteral("Catch-up extension failed. Retry or return to live.");
        setCatchupTimelineNoticeText(retryNotice);
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up rolling extension aborted after %1 attempts in %2ms (%3).")
                .arg(m_catchupRollingExtensionRetryCount)
                .arg(kCatchupRollingRetryWindowMs)
                .arg(reason));
        return false;
    }

    syncCatchupTimelineState();
    const auto availableSeconds = std::max(0.0, m_catchupTimelineAvailableSeconds);
    const auto totalDurationSeconds =
        std::max<qint64>(1, m_catchupProgramStartUtc.secsTo(m_catchupProgramStopUtc));
    if (availableSeconds <= 0.0 || totalDurationSeconds <= 0) {
        return false;
    }
    if (fromPredictiveTrigger && availableSeconds <= m_catchupTransportEndTimelineSeconds + 1.0) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Catch-up rolling extension skipped (predictive): no new provider window yet (available=%1s transportEnd=%2s).")
                .arg(availableSeconds, 0, 'f', 3)
                .arg(m_catchupTransportEndTimelineSeconds, 0, 'f', 3));
        return false;
    }

    const auto desiredDelaySeconds = std::clamp(m_catchupDesiredDelaySeconds, 0.0, availableSeconds);
    auto targetTimelineSeconds = std::max(0.0, availableSeconds - desiredDelaySeconds);
    if (fromPredictiveTrigger) {
        targetTimelineSeconds = std::max(0.0, targetTimelineSeconds - kCatchupRollingOverlapBiasSeconds);
    }
    targetTimelineSeconds = std::min(targetTimelineSeconds, std::max(0.0, static_cast<double>(totalDurationSeconds) - 1.0));

    double streamBaseOffsetSeconds = 0.0;
    const auto regeneratedUrl = regeneratedXtreamCatchupUrl(targetTimelineSeconds, &streamBaseOffsetSeconds);
    if (regeneratedUrl.trimmed().isEmpty()) {
        m_catchupRollingExtensionRetryCount += 1;
        m_catchupRollingRetryWindowTimer.restart();
        m_catchupLastRollingExtensionAttemptTimer.restart();
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Catch-up rolling extension URL regeneration failed (attempt %1/%2, reason=%3, target=%4s).")
                .arg(m_catchupRollingExtensionRetryCount)
                .arg(kCatchupRollingMaxRetries)
                .arg(reason)
                .arg(targetTimelineSeconds, 0, 'f', 3));
        return false;
    }

    m_catchupRollingExtensionRetryCount += 1;
    m_catchupRollingRetryWindowTimer.restart();
    m_catchupLastRollingExtensionAttemptTimer.restart();
    bool started = false;
    if (fromPredictiveTrigger && canUseSeamlessCatchupRolling()) {
        started = armSeamlessCatchupRollingExtension(
            reason,
            targetTimelineSeconds,
            regeneratedUrl,
            streamBaseOffsetSeconds);
    } else {
        started = beginCatchupTimelineReload(targetTimelineSeconds, regeneratedUrl, streamBaseOffsetSeconds);
    }
    if (!started) {
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
            .arg(m_catchupRollingExtensionRetryCount)
            .arg(kCatchupRollingMaxRetries));
    return true;
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
    if (m_catchupSeamlessPending) {
        abortSeamlessCatchupRolling(QStringLiteral("timeline-reload"), true);
    }

    if (m_catchupTimelineReloadInFlight) {
        m_pendingCatchupTimelineReloadTargetSeconds = targetSeconds;
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up timeline reload already in progress; queued latest target=%1s.")
                .arg(targetSeconds, 0, 'f', 3));
        return true;
    }

    m_catchupTimelineReloadInFlight = true;
    m_catchupTimelineReloadUrl = regeneratedUrl;
    m_catchupTimelineReloadStreamBaseOffsetSeconds = streamBaseOffsetSeconds;
    m_pendingCatchupTimelineReloadTargetSeconds = std::nullopt;
    m_catchupTimelineReloadAckTimer.start();
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up timeline reload started: stop-first teardown wait <=%1ms.")
            .arg(kCatchupTimelineReloadAckTimeoutMs));
    activePlayer->stop();
    return true;
}

void PlayerController::runCatchupTimelineReload()
{
    auto *activePlayer = playbackPlayer();
    if (activePlayer == nullptr || m_catchupTimelineReloadUrl.trimmed().isEmpty()) {
        return;
    }

    const auto previousPlaybackUrl = m_currentPlaybackUrl;
    m_currentPlaybackUrl = m_catchupTimelineReloadUrl;
    m_catchupActiveEofObserved = false;
    m_catchupStreamBaseOffsetSeconds = m_catchupTimelineReloadStreamBaseOffsetSeconds;
    // Deliberate compromise: long seek starts from regenerated minute anchor without residual second-precision seek.
    m_catchupPendingStreamRelativeSeekSeconds = std::nullopt;
    m_catchupSeekSettleTimer.restart();
    resetCatchupUrlLoadGuardState(true);
    m_resumePlaybackAfterLoad = false;
    m_pauseAfterLoad = false;
    stopDeferredLoadingIndicator();
    setIsLoading(false);
    m_backendBuffering = true;
    clearPlaybackStallTracking();
    refreshBufferingState();

    activePlayer->setStartupBufferingStrictMode(false);
    activePlayer->setPaused(false);
    const auto playbackUrl = prepareCatchupStreamPlaybackUrl(activePlayer, m_catchupTimelineReloadUrl, false);
    activePlayer->play(
        playbackUrl,
        catchupLoadfileOptions(m_catchupTimelineReloadStreamBaseOffsetSeconds));
    if (previousPlaybackUrl != m_currentPlaybackUrl) {
        emit currentPlaybackUrlChanged();
    }
}

void PlayerController::processPendingCatchupTimelineReload()
{
    if (!m_pendingCatchupTimelineReloadTargetSeconds.has_value()) {
        return;
    }
    const auto targetSeconds = m_pendingCatchupTimelineReloadTargetSeconds.value();
    m_pendingCatchupTimelineReloadTargetSeconds = std::nullopt;
    const auto boundedTarget = std::max(0.0, std::min(m_catchupTimelineAvailableSeconds, targetSeconds));
    m_catchupTimelinePositionSeconds = boundedTarget;
    m_catchupDesiredDelaySeconds = std::max(0.0, m_catchupTimelineAvailableSeconds - boundedTarget);
    m_catchupTimelineAtLiveEdge = (m_catchupTimelineAvailableSeconds - boundedTarget) <= 0.75;
    emit catchupTimelineChanged();
    if (shouldReloadCatchupForSeek(boundedTarget)) {
        reloadCatchupForTimelineSeek(boundedTarget);
        return;
    }
    const auto streamRelativeTarget = std::max(0.0, boundedTarget - m_catchupStreamBaseOffsetSeconds);
    m_catchupSeekSettleTimer.restart();
    playbackPlayer()->seekAbsoluteFast(streamRelativeTarget);
}

void PlayerController::finishCatchupTimelineReload(const QString &reason)
{
    m_catchupTimelineReloadAckTimer.stop();
    m_catchupTimelineReloadInFlight = false;
    m_catchupTimelineReloadUrl.clear();
    m_catchupTimelineReloadStreamBaseOffsetSeconds = 0.0;
    m_catchupRecoveryCooldownTimer.restart();
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up timeline reload finished: %1.").arg(reason));
    processPendingCatchupTimelineReload();
}

bool PlayerController::hardRestoreCatchupAtCurrentTimelinePoint(const QString &reason)
{
    if (!inCatchupMode() || playbackPlayer() == nullptr) {
        return false;
    }
    if (m_catchupTimelineReloadInFlight) {
        return false;
    }
    abortSeamlessCatchupRolling(QStringLiteral("hard-restore"), true);

    const auto targetSeconds = std::max(0.0, std::min(m_catchupTimelineAvailableSeconds, m_catchupTimelinePositionSeconds));
    if (matchXtreamTimeshiftUrl(m_catchupCanonicalPlaybackUrl).has_value()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up hard restore (%1): Xtream regenerate at timeline=%2s.")
                .arg(reason)
                .arg(targetSeconds, 0, 'f', 3));
        return reloadCatchupForTimelineSeek(targetSeconds);
    }

    auto *activePlayer = playbackPlayer();
    if (activePlayer == nullptr) {
        return false;
    }
    if (m_reconnectActive) {
        stopReconnectLoop(QStringLiteral("catchup-hard-restore"));
    }

    const auto restoreUrl = m_currentPlaybackUrl.trimmed();
    if (restoreUrl.isEmpty()) {
        return false;
    }

    const auto previousPlaybackUrl = m_currentPlaybackUrl;
    m_currentPlaybackUrl = restoreUrl;
    m_resumePlaybackAfterLoad = false;
    m_pauseAfterLoad = false;
    m_backendBuffering = true;
    clearPlaybackStallTracking();
    refreshBufferingState();

    activePlayer->setStartupBufferingStrictMode(false);
    activePlayer->setPaused(false);
    const auto playbackUrl = prepareCatchupStreamPlaybackUrl(activePlayer, restoreUrl, false);
    activePlayer->play(playbackUrl, catchupLoadfileOptions(m_catchupStreamBaseOffsetSeconds));
    if (previousPlaybackUrl != m_currentPlaybackUrl) {
        emit currentPlaybackUrlChanged();
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up hard restore (%1): reloaded current catch-up URL.")
            .arg(reason));
    m_catchupRecoveryCooldownTimer.restart();
    return true;
}

void PlayerController::evaluateCatchupDegradationRecovery(
    const std::optional<double> cacheDurationSeconds,
    const std::optional<double> cacheSpeedBytesPerSecond,
    const bool playbackAdvanced,
    const std::optional<bool> framePtsAdvanced)
{
    if (!inCatchupMode() || !m_currentChannel.has_value() || m_channelLoadFailed || m_resumePlaybackAfterLoad
        || m_catchupTimelineReloadInFlight || m_reconnectActive) {
        return;
    }
    if (m_catchupTimelineAvailableSeconds <= 0.0) {
        return;
    }
    const auto remainingSeconds = std::max(0.0, m_catchupTimelineAvailableSeconds - m_catchupTimelinePositionSeconds);
    if (remainingSeconds <= kCatchupRecoveryProgramEndGuardSeconds) {
        m_catchupNearZeroTickCount = 0;
        return;
    }

    const auto inCooldown = m_catchupRecoveryCooldownTimer.isValid()
        && m_catchupRecoveryCooldownTimer.elapsed() < kCatchupRecoveryCooldownMs;

    const auto hasCacheSample = cacheDurationSeconds.has_value()
        && std::isfinite(cacheDurationSeconds.value())
        && cacheDurationSeconds.value() >= 0.0;
    if (!hasCacheSample) {
        return;
    }
    const auto cacheSeconds = std::max(0.0, cacheDurationSeconds.value());
    const auto hasCacheSpeed = cacheSpeedBytesPerSecond.has_value()
        && std::isfinite(cacheSpeedBytesPerSecond.value())
        && cacheSpeedBytesPerSecond.value() >= 0.0;
    const auto cacheSpeed = hasCacheSpeed ? std::max(0.0, cacheSpeedBytesPerSecond.value()) : -1.0;
    const auto lowRefillSignal = hasCacheSpeed
        ? cacheSpeed <= kCatchupRecoveryLowCacheSpeedBytesPerSecond
        : (m_backendBuffering || m_playbackStalled);
    Q_UNUSED(framePtsAdvanced);
    Q_UNUSED(playbackAdvanced);

    if (cacheSeconds <= kCatchupRecoveryNearZeroSeconds && lowRefillSignal) {
        m_catchupNearZeroTickCount += 1;
    } else {
        m_catchupNearZeroTickCount = 0;
    }

    if (!inCooldown && m_catchupNearZeroTickCount >= kCatchupRecoveryNearZeroSustainTicks) {
        const auto fastRetryWindowOpen = m_catchupSeamlessFastRetryWindowTimer.isValid()
            && m_catchupSeamlessFastRetryWindowTimer.elapsed() < kCatchupSeamlessStandbyFastRetryWindowMs;
        const auto fastRetryInProgress = m_catchupSeamlessPending
            && !m_catchupSeamlessStandbyLoadIssued
            && m_catchupSeamlessFastRetryPending
            && m_catchupSeamlessFastRetryBudgetRemaining > 0
            && fastRetryWindowOpen;
        if (fastRetryInProgress) {
            const auto remainingMs = std::max<qint64>(
                0,
                static_cast<qint64>(kCatchupSeamlessStandbyFastRetryWindowMs)
                    - m_catchupSeamlessFastRetryWindowTimer.elapsed());
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up degradation watchdog (near-zero only): deferring hard restore while fast standby retry window is active (%1ms left, retries=%2).")
                    .arg(remainingMs)
                    .arg(m_catchupSeamlessFastRetryBudgetRemaining));
            return;
        }
        m_catchupNearZeroTickCount = 0;
        if (m_catchupSeamlessPending && m_catchupSeamlessStandbyReady) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up degradation watchdog (near-zero only): standby is ready, attempting seamless cutover before hard restore (cache=%1s speed=%2B/s remaining=%3s).")
                    .arg(cacheSeconds, 0, 'f', 3)
                    .arg(hasCacheSpeed ? QString::number(cacheSpeed, 'f', 0) : QStringLiteral("N/A"))
                    .arg(remainingSeconds, 0, 'f', 3));
            if (maybeCommitSeamlessCatchupCutover(QStringLiteral("degradation-near-zero"), true)) {
                m_catchupRecoveryCooldownTimer.restart();
                return;
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
            m_catchupRecoveryCooldownTimer.restart();
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
    abortSeamlessCatchupRolling(QStringLiteral("channel-switch"), true);
    m_resumePlaybackAfterLoad = false;
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
        activePlayer->stop();
        if (activePlayer == &m_catchupStandbyPlayer) {
            setSharedPlaybackPlayer(nullptr, false);
            activePlayer = &m_player;
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
    const QString &canonicalCatchupUrl,
    std::optional<double> initialProgramSeekSeconds,
    std::optional<double> initialStreamBaseOffsetSeconds,
    std::optional<double> initialTimelinePositionSeconds)
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
    abortSeamlessCatchupRolling(QStringLiteral("catchup-switch"), true);
    m_resumePlaybackAfterLoad = false;
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
        QStringLiteral("catchup.play.start"),
        QStringLiteral("Switching to provider catch-up playback for %1.").arg(channel.name));
    m_catchupProgramStartUtc = programStartUtc.toUTC();
    m_catchupProgramStopUtc = programStopUtc.toUTC();
    m_catchupProgramBoundaryReached = false;
    m_catchupActiveEofObserved = false;
    m_catchupStreamBaseOffsetSeconds = std::max(0.0, initialStreamBaseOffsetSeconds.value_or(0.0));
    m_catchupTimelinePositionSeconds = std::max(0.0, initialTimelinePositionSeconds.value_or(0.0));
    m_catchupPendingStreamRelativeSeekSeconds = std::nullopt;
    m_catchupReconnectResumeStreamRelativeSeconds = std::nullopt;
    m_catchupPendingInitialSeekSeconds = initialProgramSeekSeconds;
    m_catchupCanonicalPlaybackUrl = canonicalCatchupUrl.trimmed().isEmpty()
        ? catchupUrl.trimmed()
        : canonicalCatchupUrl.trimmed();
    resetCatchupUrlLoadGuardState(true);
    setCatchupState(channel.streamUrl, programLabel);
    syncCatchupTimelineState();
    m_catchupDesiredDelaySeconds =
        std::max(0.0, m_catchupTimelineAvailableSeconds - m_catchupTimelinePositionSeconds);
    m_catchupTransportEndTimelineSeconds =
        std::max(m_catchupStreamBaseOffsetSeconds, m_catchupTimelineAvailableSeconds);
    m_catchupRollingRetryWindowTimer.invalidate();
    m_catchupLastRollingExtensionAttemptTimer.invalidate();
    m_catchupRollingExtensionRetryCount = 0;
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
    abortSeamlessCatchupRolling(QStringLiteral("catchup-return-live"), true);
    m_resumePlaybackAfterLoad = false;
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
    m_resumePlaybackAfterLoad = false;
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
    if (timeshiftActive() && m_timeshiftController) {
        m_timeshiftController->jumpToLiveEdge();
        return;
    }
    if (m_liveBufferActive) {
        seekLiveBufferToPosition(m_liveBufferAvailableSeconds);
    }
}

void PlayerController::seekTimeshiftRelative(const double seconds)
{
    if (inCatchupMode()) {
        seekCatchupToTimelinePosition(m_catchupTimelinePositionSeconds + seconds);
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
        const auto nowUtc = QDateTime::currentDateTimeUtc();
        const auto runningProgram = m_catchupProgramStartUtc.isValid()
            && m_catchupProgramStopUtc.isValid()
            && m_catchupProgramStartUtc < nowUtc
            && nowUtc < m_catchupProgramStopUtc;
        if (runningProgram) {
            const auto elapsedSeconds = std::max<qint64>(0, m_catchupProgramStartUtc.secsTo(nowUtc));
            if (elapsedSeconds > static_cast<qint64>(kCatchupMinElapsedSeconds)) {
                const auto targetSeconds = clamped * m_catchupTimelineAvailableSeconds;
                const auto secondsBehindLiveEdge = std::max(0.0, m_catchupTimelineAvailableSeconds - targetSeconds);
                if (secondsBehindLiveEdge < kCatchupMinElapsedSeconds) {
                    const auto blockedNotice =
                        QStringLiteral("Timeline selection is locked within 10 minutes of live.");
                    setCatchupTimelineNoticeText(blockedNotice, kCatchupTimelineNoticeAutoClearMs);
                    return;
                }
            }
        }
        seekCatchupToTimelinePosition(clamped * m_catchupTimelineAvailableSeconds);
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
    const auto playbackUrl = catchupRetry
        ? prepareCatchupStreamPlaybackUrl(playbackPlayer(), retryUrl, false)
        : retryUrl;
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
            .arg(m_playbackStalled ? QStringLiteral("true") : QStringLiteral("false"))
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

void PlayerController::updatePosition()
{
    auto *activePlayer = playbackPlayer();
    if (m_currentChannel.has_value()) {
        updateBitrateAverageBitsPerSecond(instantaneousBitrateBitsPerSecond(activePlayer));
    }
    syncLiveBufferState();
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

    if (inCatchupMode()) {
        maybeCorrectUnexpectedCatchupRollback(seconds);
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
    evaluateCatchupDegradationRecovery(
        health.cacheDurationSeconds,
        health.cacheSpeedBytesPerSecond,
        playbackAdvanced,
        framePtsAdvanced);
    // Catch-up degradation recovery can commit seamless cutover and swap playbackPlayer().
    // Refresh active-player binding before any logic that inspects EOF/provider state.
    auto *activePlayerAfterRecovery = playbackPlayer();
    const auto playerSwitchedDuringTick = activePlayerAfterRecovery != activePlayer;
    if (activePlayerAfterRecovery != nullptr) {
        activePlayer = activePlayerAfterRecovery;
    }
    maybeRetuneCatchupBuffering(health.cacheDurationSeconds);
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
        if (shouldExtendCatchupRollingWindowPredictively(seconds)) {
            extendCatchupRollingWindow(QStringLiteral("predictive-near-edge"), true);
        }
        const auto eofReached = !playerSwitchedDuringTick
            && activePlayer->demuxerCacheReaderEof().value_or(false);
        if (eofReached
            && maybeStopCatchupAtProgrammeBoundary(
                seconds,
                health.cacheDurationSeconds,
                QStringLiteral("demuxer-eof-boundary"))) {
            refreshBufferingState();
            return;
        }
        if (eofReached && !m_catchupActiveEofObserved) {
            m_catchupActiveEofObserved = true;
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral("Catch-up active transport reached demuxer EOF; awaiting provider close confirmation."));
        }
        const auto providerClosed = activeCatchupProviderConnectionClosed();
        if (m_catchupActiveEofObserved && m_catchupActiveStreamSession && !m_catchupActiveStreamSession->closeRequestedByApp()) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up demuxer EOF observed; requesting owned provider close before seamless rollover arm."));
            m_catchupActiveStreamSession->closeProviderConnection(QStringLiteral("active-eof-seamless"));
        }
        if (!m_catchupSeamlessPending
            && canUseSeamlessCatchupRolling()
            && m_catchupActiveEofObserved
            && providerClosed) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up seamless rollover arm conditions met (EOF observed, provider closed)."));
            extendCatchupRollingWindow(QStringLiteral("eof-close-cache-threshold"), true);
        }
        if (m_catchupSeamlessPending
            && !m_catchupSeamlessStandbyLoadIssued
            && canUseSeamlessCatchupRolling()
            && m_catchupActiveEofObserved
            && providerClosed) {
            if (!m_catchupSeamlessPostCloseDelayPending && !m_catchupSeamlessPostCloseDelayTimer.isActive()) {
                m_catchupSeamlessPostCloseDelayPending = true;
                m_catchupSeamlessPostCloseDelayTimer.start();
                Core::DebugLogger::instance().log(
                    QStringLiteral("player"),
                    QStringLiteral(
                        "Catch-up seamless standby warmup delayed by %1ms after EOF/provider-close confirmation.")
                        .arg(kCatchupSeamlessPostCloseDelayMs));
            }
        }
        if (m_catchupSeamlessPending && m_catchupSeamlessStandbyReady) {
            maybeCommitSeamlessCatchupCutover(QStringLiteral("near-edge"));
        }
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
