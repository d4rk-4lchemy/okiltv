#include "timeshiftcontroller.h"

#include "dvrcontroller.h"
#include "multiviewcontroller.h"
#include "playercontroller.h"

#include "../core/appdatapaths.h"
#include "../core/debuglogger.h"
#include "../core/processutils.h"
#include "../core/redaction.h"
#include "../core/settingsmanager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStorageInfo>
#include <QStringList>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>

namespace OKILTV::App {

using namespace Core;

namespace {

constexpr double kDefaultBitrateBitsPerSecond = 8.0 * 1000.0 * 1000.0;
constexpr int kReadyPollIntervalMs = 300;
constexpr int kPlaylistPollIntervalMs = 1000;
constexpr int kPlaylistStallTimeoutMs = 20000;
constexpr double kLiveBadgeExtraFrontSlackSeconds = 3.0;
constexpr double kLiveEdgeSafetyGuardSeconds = 1.0;
constexpr int kNearLivePlaybackEndedRecoveryCooldownMs = 10000;
constexpr int kNearLivePlaybackEndedPlaylistFreshnessMs = 5000;
constexpr int kLocalPlaybackRecoveryCooldownMs = 3000;
constexpr int kPostLoadVerifyDelayMs = 700;
constexpr double kPostLoadSeekToleranceFloorSeconds = 2.0;
constexpr double kStartupAttachWarmupMultiplier = 1.2;
constexpr double kStartupTargetToleranceSeconds = 1.0;
constexpr int kOutOfRetentionNoticeDurationMs = 2000;
constexpr int kGapSnapNoticeDurationMs = 2000;
constexpr int kFfprobeTimeoutMs = 10000;
constexpr int kReconnectBackoffInitialMs = 1000;
constexpr int kReconnectBackoffCapMs = 60000;
constexpr quint16 kPlaybackServerPort = 0;
constexpr int kDelayedAttachPrerollMinSeconds = 8;
constexpr int kDelayedAttachPrerollSegmentMultiplier = 4;
constexpr auto kTimeshiftLoadfileOptions = "force-seekable=yes,demuxer-lavf-o=live_start_index=0";
constexpr auto kTimeshiftSessionMarkerFileName = ".okiltv-timeshift-session";

double normalizedPlayerBufferSeconds(const double playerBufferSeconds)
{
    if (!std::isfinite(playerBufferSeconds) || playerBufferSeconds < 0.0) {
        return 0.0;
    }
    return playerBufferSeconds;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double effectiveLiveEdgeSafetyBufferSeconds(const double playerBufferSeconds, const int timeshiftSegmentSeconds)
{
    const auto normalizedSegmentSeconds = std::clamp(timeshiftSegmentSeconds, 2, 60);
    const auto minimumTailSafetySeconds = static_cast<double>(normalizedSegmentSeconds) + kLiveEdgeSafetyGuardSeconds;
    return std::max(normalizedPlayerBufferSeconds(playerBufferSeconds), minimumTailSafetySeconds);
}

double liveBadgeThresholdSeconds(const double playerBufferSeconds, const int timeshiftSegmentSeconds)
{
    return effectiveLiveEdgeSafetyBufferSeconds(playerBufferSeconds, timeshiftSegmentSeconds)
        + kLiveBadgeExtraFrontSlackSeconds;
}

double millisecondsToSeconds(const qint64 milliseconds)
{
    return static_cast<double>(milliseconds) / 1000.0;
}

QDateTime parseIsoDateTimeUtc(const QString &value)
{
    auto parsed = QDateTime::fromString(value.trimmed(), Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(value.trimmed(), Qt::ISODate);
    }
    return parsed.toUTC();
}

QString sessionTimestamp()
{
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmsszzz"));
}

QString sessionMarkerFilePath(const QString &sessionDirectory)
{
    return QDir(sessionDirectory).filePath(QString::fromLatin1(kTimeshiftSessionMarkerFileName));
}

bool ensureSessionMarkerFile(const QString &sessionDirectory)
{
    QFile marker(sessionMarkerFilePath(sessionDirectory));
    if (marker.exists()) {
        return true;
    }
    if (!marker.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    marker.write("okiltv-timeshift-session\n");
    return marker.flush();
}

bool isManagedTimeshiftSessionDirectory(const QFileInfo &entry)
{
    if (!entry.isDir()) {
        return false;
    }
    return QFileInfo::exists(sessionMarkerFilePath(entry.absoluteFilePath()));
}

QString isoDateTimeUtc(const qint64 epochMs)
{
    if (epochMs <= 0) {
        return {};
    }
    const auto timestamp = QDateTime::fromMSecsSinceEpoch(epochMs, QTimeZone::UTC);
    return timestamp.isValid() ? timestamp.toString(Qt::ISODateWithMs) : QString {};
}

struct PlaylistSegment
{
    QString relativePath;
    QDateTime programDateTimeUtc;
    double durationSeconds { 0.0 };
};

struct PlaylistSnapshot
{
    QDateTime windowStartUtc;
    QDateTime liveEdgeUtc;
    double availableSeconds { 0.0 };
    bool valid { false };
    QList<PlaylistSegment> segments;
    int targetDurationSeconds { 2 };
    qint64 mediaSequence { 0 };
};

struct ProbedStream
{
    int inputStreamIndex { -1 };
    QString codecName;
    QString language;
    QString title;
    bool isDefault { false };
};

struct StreamLayout
{
    bool valid { false };
    std::optional<ProbedStream> primaryVideo;
    QList<ProbedStream> audioStreams;
    QList<ProbedStream> supportedSubtitleStreams;
    QList<ProbedStream> droppedSubtitleStreams;
    QString error;
};

PlaylistSnapshot parsePlaylistSnapshot(const QString &playlistPath)
{
    QFile file(playlistPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    PlaylistSnapshot snapshot;
    auto pendingProgramDateTime = QDateTime {};
    auto pendingDuration = -1.0;
    const auto lines = QString::fromUtf8(file.readAll()).split(u'\n');
    for (const auto &rawLine : lines) {
        const auto line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QStringLiteral("#EXT-X-TARGETDURATION:"))) {
            snapshot.targetDurationSeconds =
                std::max(1, line.mid(QStringLiteral("#EXT-X-TARGETDURATION:").size()).toInt());
            continue;
        }
        if (line.startsWith(QStringLiteral("#EXT-X-MEDIA-SEQUENCE:"))) {
            snapshot.mediaSequence = std::max<qint64>(0, line.mid(QStringLiteral("#EXT-X-MEDIA-SEQUENCE:").size()).toLongLong());
            continue;
        }
        if (line.startsWith(QStringLiteral("#EXT-X-PROGRAM-DATE-TIME:"))) {
            pendingProgramDateTime =
                parseIsoDateTimeUtc(line.mid(QStringLiteral("#EXT-X-PROGRAM-DATE-TIME:").size()));
            continue;
        }
        if (line.startsWith(QStringLiteral("#EXTINF:"))) {
            const auto markerEnd = line.indexOf(u',');
            const auto durationText = markerEnd >= 0 ? line.mid(8, markerEnd - 8) : line.mid(8);
            pendingDuration = std::max(0.0, durationText.toDouble());
            continue;
        }
        if (line.startsWith(u'#') || pendingDuration <= 0.0) {
            continue;
        }

        PlaylistSegment segment;
        segment.relativePath = line;
        segment.programDateTimeUtc = pendingProgramDateTime;
        segment.durationSeconds = pendingDuration;
        snapshot.segments.push_back(segment);
        snapshot.availableSeconds += pendingDuration;
        pendingProgramDateTime = {};
        pendingDuration = -1.0;
    }

    auto firstProgramDateIndex = -1;
    for (int index = 0; index < snapshot.segments.size(); ++index) {
        if (snapshot.segments.at(index).programDateTimeUtc.isValid()) {
            firstProgramDateIndex = index;
            break;
        }
    }

    if (firstProgramDateIndex >= 0) {
        for (int index = firstProgramDateIndex + 1; index < snapshot.segments.size(); ++index) {
            auto &segment = snapshot.segments[index];
            if (segment.programDateTimeUtc.isValid()) {
                continue;
            }
            const auto &previous = snapshot.segments.at(index - 1);
            if (!previous.programDateTimeUtc.isValid()) {
                continue;
            }
            segment.programDateTimeUtc =
                previous.programDateTimeUtc.addMSecs(static_cast<qint64>(std::llround(previous.durationSeconds * 1000.0)));
        }
        for (int index = firstProgramDateIndex - 1; index >= 0; --index) {
            auto &segment = snapshot.segments[index];
            if (segment.programDateTimeUtc.isValid()) {
                continue;
            }
            const auto &next = snapshot.segments.at(index + 1);
            if (!next.programDateTimeUtc.isValid()) {
                continue;
            }
            segment.programDateTimeUtc =
                next.programDateTimeUtc.addMSecs(-static_cast<qint64>(std::llround(segment.durationSeconds * 1000.0)));
        }
    }

    for (const auto &segment : snapshot.segments) {
        if (segment.programDateTimeUtc.isValid()) {
            snapshot.windowStartUtc = segment.programDateTimeUtc;
            break;
        }
    }
    for (int index = static_cast<int>(snapshot.segments.size()) - 1; index >= 0; --index) {
        const auto &segment = snapshot.segments.at(index);
        if (!segment.programDateTimeUtc.isValid()) {
            continue;
        }
        snapshot.liveEdgeUtc =
            segment.programDateTimeUtc.addMSecs(static_cast<qint64>(std::llround(segment.durationSeconds * 1000.0)));
        break;
    }

    snapshot.valid = snapshot.availableSeconds > 0.0 && !snapshot.segments.isEmpty();
    return snapshot;
}

double cumulativeSecondsBeforeSegment(const PlaylistSnapshot &snapshot, const int targetIndex)
{
    auto seconds = 0.0;
    for (int index = 0; index < targetIndex && index < snapshot.segments.size(); ++index) {
        seconds += std::max(0.0, snapshot.segments.at(index).durationSeconds);
    }
    return seconds;
}

int anchorSegmentIndexForOffsetSeconds(const PlaylistSnapshot &snapshot, const double targetOffsetSeconds)
{
    if (snapshot.segments.isEmpty()) {
        return 0;
    }

    const auto clampedOffset = std::clamp(targetOffsetSeconds, 0.0, std::max(0.0, snapshot.availableSeconds));
    auto cumulativeSeconds = 0.0;
    for (int index = 0; index < snapshot.segments.size(); ++index) {
        const auto segmentDuration = std::max(0.0, snapshot.segments.at(index).durationSeconds);
        if (cumulativeSeconds + segmentDuration > clampedOffset || index == snapshot.segments.size() - 1) {
            return index;
        }
        cumulativeSeconds += segmentDuration;
    }

    return std::max(0, static_cast<int>(snapshot.segments.size()) - 1);
}

int anchorSegmentIndexForProgramDateTime(const PlaylistSnapshot &snapshot, const qint64 targetEpochMs)
{
    if (snapshot.segments.isEmpty() || targetEpochMs <= 0) {
        return -1;
    }

    auto firstProgramDateIndex = -1;
    auto candidateIndex = -1;
    for (int index = 0; index < snapshot.segments.size(); ++index) {
        const auto &segment = snapshot.segments.at(index);
        if (!segment.programDateTimeUtc.isValid()) {
            continue;
        }
        if (firstProgramDateIndex < 0) {
            firstProgramDateIndex = index;
        }
        const auto segmentEpochMs = segment.programDateTimeUtc.toMSecsSinceEpoch();
        if (segmentEpochMs <= targetEpochMs) {
            candidateIndex = index;
            continue;
        }
        break;
    }

    if (candidateIndex >= 0) {
        return candidateIndex;
    }
    return firstProgramDateIndex;
}

QString urlWithPlaybackTimingQuery(
    const QString &relativePath,
    const qint64 anchorEpochMs,
    const qint64 targetEpochMs)
{
    if (anchorEpochMs <= 0 && targetEpochMs <= 0) {
        return relativePath;
    }

    auto url = QUrl(relativePath);
    QUrlQuery query(url);
    if (anchorEpochMs > 0) {
        const auto pdt = isoDateTimeUtc(anchorEpochMs);
        if (!pdt.isEmpty()) {
            query.addQueryItem(QStringLiteral("pdt"), pdt);
        }
    }
    if (targetEpochMs > 0) {
        const auto targetPdt = isoDateTimeUtc(targetEpochMs);
        if (!targetPdt.isEmpty()) {
            query.addQueryItem(QStringLiteral("target_pdt"), targetPdt);
        }
    }
    if (query.queryItems().isEmpty()) {
        return relativePath;
    }
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

QString trimmedTagString(const QJsonObject &tags, const char *key)
{
    return tags.value(QLatin1String(key)).toString().trimmed();
}

QString normalizedSubtitleCodecName(const QString &codecName)
{
    return codecName.trimmed().toLower();
}

bool subtitleCodecSupportedForWebVtt(const QString &codecName)
{
    static const QSet<QString> kSupported {
        QStringLiteral("ass"),
        QStringLiteral("mov_text"),
        QStringLiteral("ssa"),
        QStringLiteral("srt"),
        QStringLiteral("subrip"),
        QStringLiteral("text"),
        QStringLiteral("ttml"),
        QStringLiteral("webvtt")
    };
    return kSupported.contains(normalizedSubtitleCodecName(codecName));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QString attributeSafeName(const QString &value, const QString &fallback)
{
    auto result = value.trimmed();
    if (result.isEmpty()) {
        result = fallback;
    }
    result.replace(QRegularExpression(QStringLiteral("[\\r\\n\"]+")), QStringLiteral(" "));
    result = result.simplified();
    return result.isEmpty() ? fallback : result;
}

QString subtitleDisplayName(const ProbedStream &stream, const int index)
{
    if (!stream.title.trimmed().isEmpty()) {
        return attributeSafeName(stream.title, QStringLiteral("Subtitle %1").arg(index + 1));
    }
    if (!stream.language.trimmed().isEmpty()) {
        return attributeSafeName(stream.language.toUpper(), QStringLiteral("Subtitle %1").arg(index + 1));
    }
    return QStringLiteral("Subtitle %1").arg(index + 1);
}

StreamLayout probeStreamLayout(const QString &payload)
{
    StreamLayout layout;
    if (payload.trimmed().isEmpty()) {
        layout.error = QStringLiteral("ffprobe did not return stream metadata");
        return layout;
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(payload.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        layout.error = QStringLiteral("ffprobe returned invalid JSON");
        return layout;
    }

    const auto streams = document.object().value(QStringLiteral("streams")).toArray();
    for (const auto &streamValue : streams) {
        const auto streamObject = streamValue.toObject();
        const auto codecType = streamObject.value(QStringLiteral("codec_type")).toString().trimmed();
        const auto codecName = streamObject.value(QStringLiteral("codec_name")).toString().trimmed();
        const auto inputIndex = streamObject.value(QStringLiteral("index")).toInt(-1);
        const auto tags = streamObject.value(QStringLiteral("tags")).toObject();
        const auto disposition = streamObject.value(QStringLiteral("disposition")).toObject();

        if (codecType.isEmpty() || inputIndex < 0) {
            continue;
        }

        ProbedStream stream;
        stream.inputStreamIndex = inputIndex;
        stream.codecName = codecName;
        stream.language = trimmedTagString(tags, "language");
        stream.title = trimmedTagString(tags, "title");
        stream.isDefault = disposition.value(QStringLiteral("default")).toInt(0) != 0;

        if (codecType == QLatin1String("video")) {
            if (!layout.primaryVideo.has_value()) {
                layout.primaryVideo = stream;
            }
            continue;
        }
        if (codecType == QLatin1String("audio")) {
            layout.audioStreams.push_back(stream);
            continue;
        }
        if (codecType == QLatin1String("subtitle")) {
            if (subtitleCodecSupportedForWebVtt(codecName)) {
                layout.supportedSubtitleStreams.push_back(stream);
            } else {
                layout.droppedSubtitleStreams.push_back(stream);
            }
        }
    }

    layout.valid = layout.primaryVideo.has_value() || !layout.audioStreams.isEmpty();
    if (!layout.valid) {
        layout.error = QStringLiteral("ffprobe found no playable audio/video streams");
    }
    return layout;
}

QString timeshiftHlsFlags()
{
    QStringList flags {
        QStringLiteral("delete_segments"),
        QStringLiteral("append_list"),
        QStringLiteral("program_date_time"),
        // Split on wall-clock time instead of waiting for keyframes. Live IPTV sources
        // often use long GOPs, which otherwise produces 5-10s bursts and a stale playlist.
        QStringLiteral("split_by_time")
    };
#if !defined(Q_OS_WIN)
    // Atomic playlist replacement is fine on POSIX, but on Windows mpv can keep the
    // playlist file open while ffmpeg tries to rename the temp file into place.
    // That leaves the on-disk playlist stuck on the first few segments.
    flags.push_back(QStringLiteral("temp_file"));
#endif
    return flags.join(u'+');
}

QByteArray httpReasonPhrase(const int statusCode)
{
    switch (statusCode) {
    case 200:
        return QByteArrayLiteral("OK");
    case 206:
        return QByteArrayLiteral("Partial Content");
    case 400:
        return QByteArrayLiteral("Bad Request");
    case 404:
        return QByteArrayLiteral("Not Found");
    case 405:
        return QByteArrayLiteral("Method Not Allowed");
    case 416:
        return QByteArrayLiteral("Range Not Satisfiable");
    case 503:
        return QByteArrayLiteral("Service Unavailable");
    default:
        return QByteArrayLiteral("Internal Server Error");
    }
}

QByteArray httpContentType(const QString &path)
{
    if (path.endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive)) {
        return QByteArrayLiteral("application/vnd.apple.mpegurl");
    }
    if (path.endsWith(QStringLiteral(".vtt"), Qt::CaseInsensitive)) {
        return QByteArrayLiteral("text/vtt");
    }
    if (path.endsWith(QStringLiteral(".ts"), Qt::CaseInsensitive)) {
        return QByteArrayLiteral("video/mp2t");
    }
    return QByteArrayLiteral("application/octet-stream");
}

QByteArray buildHttpResponse(
    const int statusCode,
    const QByteArray &contentType,
    const QByteArray &body,
    const QList<QByteArray> &extraHeaders = {},
    const bool headOnly = false)
{
    QByteArray response;
    response += "HTTP/1.1 ";
    response += QByteArray::number(statusCode);
    response += ' ';
    response += httpReasonPhrase(statusCode);
    response += "\r\n";
    response += "Connection: close\r\n";
    response += "Cache-Control: no-store, no-cache, must-revalidate\r\n";
    response += "Pragma: no-cache\r\n";
    if (!contentType.isEmpty()) {
        response += "Content-Type: ";
        response += contentType;
        response += "\r\n";
    }
    for (const auto &header : extraHeaders) {
        if (!header.isEmpty()) {
            response += header;
            response += "\r\n";
        }
    }
    response += "Content-Length: ";
    response += QByteArray::number(body.size());
    response += "\r\n\r\n";
    if (!headOnly) {
        response += body;
    }
    return response;
}

QString sanitizeRelativeRequestPath(const QString &path)
{
    auto normalized = QDir::cleanPath(path);
    while (normalized.startsWith(u'/')) {
        normalized.remove(0, 1);
    }
    if (normalized.isEmpty()
        || normalized == QStringLiteral(".")
        || normalized.startsWith(QStringLiteral("../"))
        || normalized.contains(QStringLiteral("/../"))
        || QDir::isAbsolutePath(normalized)) {
        return {};
    }
    return normalized;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QByteArray requestHeaderValue(const QByteArray &requestData, const QByteArray &headerName)
{
    const auto normalizedHeaderName = headerName.toLower();
    const auto headerBoundary = requestData.indexOf("\r\n\r\n");
    const auto headerBlock = headerBoundary >= 0 ? requestData.left(headerBoundary) : requestData;
    const auto lines = headerBlock.split('\n');
    for (int index = 1; index < lines.size(); ++index) {
        const auto line = lines.at(index).trimmed();
        const auto normalizedLine = line.toLower();
        if (normalizedLine.startsWith(normalizedHeaderName)) {
            return line.mid(headerName.size()).trimmed();
        }
    }
    return {};
}

struct ByteRange
{
    bool requested { false };
    bool satisfiable { true };
    qint64 start { 0 };
    qint64 end { -1 };
};

ByteRange parseByteRangeHeader(const QByteArray &rangeHeader, const qint64 totalSize)
{
    ByteRange range;
    if (rangeHeader.trimmed().isEmpty()) {
        return range;
    }

    range.requested = true;
    if (totalSize < 0) {
        range.satisfiable = false;
        return range;
    }

    const auto trimmed = rangeHeader.trimmed();
    if (!trimmed.toLower().startsWith("bytes=")) {
        range.satisfiable = false;
        return range;
    }

    const auto spec = trimmed.mid(6).trimmed();
    if (spec.isEmpty() || spec.contains(',')) {
        range.satisfiable = false;
        return range;
    }

    const auto dashIndex = spec.indexOf('-');
    if (dashIndex < 0) {
        range.satisfiable = false;
        return range;
    }

    bool okStart = false;
    bool okEnd = false;
    const auto startText = spec.left(dashIndex).trimmed();
    const auto endText = spec.mid(dashIndex + 1).trimmed();
    if (startText.isEmpty()) {
        const auto suffixLength = endText.toLongLong(&okEnd);
        if (!okEnd || suffixLength <= 0) {
            range.satisfiable = false;
            return range;
        }
        range.start = std::max<qint64>(0, totalSize - suffixLength);
        range.end = totalSize - 1;
        return range;
    }

    range.start = startText.toLongLong(&okStart);
    if (!okStart || range.start < 0 || range.start >= totalSize) {
        range.satisfiable = false;
        return range;
    }

    if (endText.isEmpty()) {
        range.end = totalSize - 1;
        return range;
    }

    range.end = endText.toLongLong(&okEnd);
    if (!okEnd || range.end < range.start) {
        range.satisfiable = false;
        return range;
    }

    range.end = std::min(range.end, totalSize - 1);
    return range;
}

} // namespace

TimeshiftController::TimeshiftController(
    SettingsManager *settings,
    PlayerController *playerController,
    DvrController *dvrController,
    MultiViewController *multiViewController,
    QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_playerController(playerController)
    , m_dvrController(dvrController)
    , m_multiViewController(multiViewController)
{
    connect(&m_playbackServer, &QTcpServer::newConnection, this, &TimeshiftController::handlePlaybackServerConnection);

    m_readyPollTimer.setInterval(kReadyPollIntervalMs);
    m_readyPollTimer.setSingleShot(false);
    connect(&m_readyPollTimer, &QTimer::timeout, this, &TimeshiftController::attachSessionPlaybackIfReady);

    m_playlistPollTimer.setInterval(kPlaylistPollIntervalMs);
    m_playlistPollTimer.setSingleShot(false);
    connect(&m_playlistPollTimer, &QTimer::timeout, this, &TimeshiftController::handlePlaylistPoll);
    m_noticeClearTimer.setSingleShot(true);
    connect(&m_noticeClearTimer, &QTimer::timeout, this, [this]() {
        if (!m_session.has_value()) {
            return;
        }
        if (m_session->id != m_noticeAutoClearSessionId
            || m_session->noticeText != m_noticeAutoClearText) {
            return;
        }
        setNoticeText(QString {});
    });
    m_reconnectGenerationTimer.setSingleShot(true);
    connect(&m_reconnectGenerationTimer, &QTimer::timeout, this, &TimeshiftController::attemptReconnectGeneration);

    connect(m_playerController, &PlayerController::currentChannelChanged, this, &TimeshiftController::handleCurrentChannelChanged);
    connect(m_playerController, &PlayerController::playbackFileLoaded, this, &TimeshiftController::handlePlaybackFileLoaded);
    connect(m_playerController, &PlayerController::isLoadingChanged, this, [this]() {
        finalizePendingPlaybackLoadIfReady(QStringLiteral("loading-state"));
    });
    connect(m_playerController, &PlayerController::isPlayingChanged, this, [this]() {
        finalizePendingPlaybackLoadIfReady(QStringLiteral("playing-state"));
    });
    connect(m_multiViewController, &MultiViewController::layoutModeChanged, this, &TimeshiftController::handleMultiviewLayoutChanged);

    cleanupStaleSessions();
    applySettings();
}

TimeshiftController::~TimeshiftController()
{
    stopSession(false, QStringLiteral("controller-destroyed"));
}

bool TimeshiftController::enabled() const
{
    return m_settings->current().timeshiftEnabled && Core::ffmpegToolsAvailable();
}

bool TimeshiftController::isActive() const
{
    return m_session.has_value() && m_session->playbackAttached;
}

bool TimeshiftController::isPreparing() const
{
    return m_session.has_value() && m_session->state == SessionState::Starting;
}

bool TimeshiftController::isAtLiveEdge() const
{
    if (!isActive()) {
        return true;
    }

    const auto thresholdSeconds = liveBadgeThresholdSeconds(
        m_settings->current().playerBufferSeconds,
        m_settings->current().timeshiftSegmentSeconds);
    return behindLiveSeconds() <= thresholdSeconds;
}

int TimeshiftController::configuredSegmentSeconds() const
{
    return std::clamp(m_settings->current().timeshiftSegmentSeconds, 2, 60);
}

double TimeshiftController::behindLiveSeconds() const
{
    if (!isActive()) {
        return 0.0;
    }

    const auto currentEpoch = currentPlaybackEpochMs();
    const auto liveEpoch = liveEdgeEpochMs();
    if (currentEpoch <= 0 || liveEpoch <= 0) {
        return std::max(0.0, availableDurationSeconds() - currentPositionSeconds());
    }
    return std::max(0.0, millisecondsToSeconds(liveEpoch - currentEpoch));
}

int TimeshiftController::windowSeconds() const
{
    return std::max(0, m_settings->current().timeshiftWindowMinutes * 60);
}

double TimeshiftController::availableDurationSeconds() const
{
    const auto globalStart = globalWindowStartEpochMs();
    const auto globalLive = globalLiveEdgeEpochMs();
    if (globalStart <= 0 || globalLive <= globalStart) {
        return 0.0;
    }
    return std::max(0.0, millisecondsToSeconds(globalLive - globalStart));
}

double TimeshiftController::currentPositionSeconds() const
{
    if (!isActive()) {
        return 0.0;
    }

    const auto currentEpoch = currentPlaybackEpochMs();
    const auto startEpoch = windowStartEpochMs();
    if (currentEpoch > 0 && startEpoch > 0) {
        return std::clamp(millisecondsToSeconds(currentEpoch - startEpoch), 0.0, availableDurationSeconds());
    }

    return std::clamp(std::max(0.0, m_playerController->playbackPositionSeconds()), 0.0, availableDurationSeconds());
}

qint64 TimeshiftController::windowStartEpochMs() const
{
    return globalWindowStartEpochMs();
}

qint64 TimeshiftController::liveEdgeEpochMs() const
{
    return globalLiveEdgeEpochMs();
}

qint64 TimeshiftController::attachedWindowStartEpochMs() const
{
    return m_session.has_value() ? std::max<qint64>(0, m_session->attachedWindowStartEpochMs) : 0;
}

qint64 TimeshiftController::attachedWindowEndEpochMs() const
{
    if (!m_session.has_value()) {
        return 0;
    }
    return std::max(attachedWindowStartEpochMs(), sessionLiveEdgeEpochMs(m_session.value()));
}

qint64 TimeshiftController::currentPlaybackEpochMs() const
{
    if (!isActive()) {
        return 0;
    }
    const auto *session = m_session ? &*m_session : nullptr;
    if (session == nullptr) {
        return 0;
    }

    const auto attachedStart = attachedWindowStartEpochMs();
    const auto liveEdge = sessionLiveEdgeEpochMs(*session);
    if (attachedStart <= 0 || liveEdge <= 0) {
        return 0;
    }

    const auto playbackMs =
        attachedStart + static_cast<qint64>(std::llround(std::max(0.0, m_playerController->playbackPositionSeconds()) * 1000.0));
    return std::clamp(playbackMs, attachedStart, liveEdge);
}

int TimeshiftController::audioTrackCount() const
{
    return m_session.has_value() ? std::max(0, m_session->audioTrackCount) : 0;
}

int TimeshiftController::subtitleTrackCount() const
{
    return m_session.has_value() ? std::max(0, m_session->subtitleTrackCount) : 0;
}

QString TimeshiftController::droppedSubtitleSummary() const
{
    if (!m_session.has_value() || m_session->droppedSubtitleCodecs.isEmpty()) {
        return QStringLiteral("None");
    }
    return m_session->droppedSubtitleCodecs.join(QStringLiteral(", "));
}

QString TimeshiftController::noticeText() const
{
    return m_session.has_value() ? m_session->noticeText : QString {};
}

QString TimeshiftController::lastSeekModeText() const
{
    return m_session.has_value() ? m_session->lastSeekModeText : QStringLiteral("None");
}

qint64 TimeshiftController::targetEpochMsFromFraction(const double fraction) const
{
    const auto startEpoch = windowStartEpochMs();
    const auto liveEpoch = liveEdgeEpochMs();
    if (startEpoch <= 0 || liveEpoch <= startEpoch) {
        return 0;
    }

    return startEpoch + static_cast<qint64>(std::llround(
                            static_cast<double>(liveEpoch - startEpoch) * std::clamp(fraction, 0.0, 1.0)));
}

qint64 TimeshiftController::targetEpochMsFromCurrentOffset(const double seconds) const
{
    const auto currentEpoch = currentPlaybackEpochMs();
    if (currentEpoch <= 0) {
        return 0;
    }
    return currentEpoch + static_cast<qint64>(std::llround(seconds * 1000.0));
}

bool TimeshiftController::canUseDirectSeek(const qint64 targetEpochMs) const
{
    if (!isActive()) {
        return false;
    }
    const auto *session = m_session ? &*m_session : nullptr;
    if (session == nullptr) {
        return false;
    }
    if (!m_playerController->player()->seekable().value_or(false)) {
        return false;
    }
    if (session->playbackLoadPending) {
        return false;
    }

    const auto attachedStart = attachedWindowStartEpochMs();
    const auto attachedEnd = attachedWindowEndEpochMs();
    if (attachedStart <= 0 || attachedEnd <= attachedStart) {
        return false;
    }
    if (targetEpochMs < attachedStart || targetEpochMs > attachedEnd) {
        return false;
    }

    return true;
}

bool TimeshiftController::directSeekToEpochMs(const qint64 targetEpochMs)
{
    if (!isActive() || !m_session.has_value()) {
        return false;
    }

    const auto attachedStart = attachedWindowStartEpochMs();
    const auto targetSeconds =
        std::max(0.0, millisecondsToSeconds(std::max(targetEpochMs, attachedStart) - attachedStart));
    m_playerController->player()->seekAbsolute(targetSeconds);
    m_session->lastSeekModeText = QStringLiteral("Direct");
    resumePlaybackAfterTransportAction();
    emit stateChanged();
    return true;
}

void TimeshiftController::setNoticeText(const QString &text, const int autoClearMs)
{
    if (!m_session.has_value()) {
        return;
    }

    const auto normalized = text.trimmed();
    if (!normalized.isEmpty() && autoClearMs > 0) {
        m_noticeAutoClearSessionId = m_session->id;
        m_noticeAutoClearText = normalized;
        m_noticeClearTimer.start(std::max(1, autoClearMs));
    } else {
        m_noticeClearTimer.stop();
        m_noticeAutoClearSessionId.clear();
        m_noticeAutoClearText.clear();
    }

    if (m_session->noticeText == normalized) {
        return;
    }

    m_session->noticeText = normalized;
    emit stateChanged();
}

TimeshiftController::TrackPreference TimeshiftController::currentTrackPreference(const QString &type) const
{
    TrackPreference preference;
    const auto tracks = m_playerController->player()->trackList();
    for (const auto &trackValue : tracks) {
        const auto track = trackValue.toMap();
        if (track.value(QStringLiteral("type")).toString() != type || !track.value(QStringLiteral("selected")).toBool()) {
            continue;
        }

        preference.valid = true;
        preference.title = track.value(QStringLiteral("title")).toString().trimmed();
        preference.language = track.value(QStringLiteral("lang")).toString().trimmed();
        return preference;
    }

    if (type == QLatin1String("sub")) {
        preference.valid = true;
        preference.noneSelected = true;
    }
    return preference;
}

void TimeshiftController::captureTrackPreferences()
{
    if (!m_session.has_value()) {
        return;
    }

    m_session->preferredAudioTrack = currentTrackPreference(QStringLiteral("audio"));
    m_session->preferredSubtitleTrack = currentTrackPreference(QStringLiteral("sub"));
    m_session->pendingTrackRestore = true;
}

int TimeshiftController::findTrackIdForPreference(
    const QVariantList &tracks,
    const QString &type,
    const TrackPreference &preference) const
{
    if (!preference.valid) {
        return -1;
    }

    if (type == QLatin1String("sub") && preference.noneSelected) {
        return 0;
    }

    auto fallbackMatch = -1;
    auto defaultMatch = -1;
    for (const auto &trackValue : tracks) {
        const auto track = trackValue.toMap();
        if (track.value(QStringLiteral("type")).toString() != type) {
            continue;
        }

        const auto id = track.value(QStringLiteral("id")).toInt();
        if (id < 0) {
            continue;
        }

        const auto title = track.value(QStringLiteral("title")).toString().trimmed();
        const auto language = track.value(QStringLiteral("lang")).toString().trimmed();
        const auto titleMatches = !preference.title.isEmpty() && title.compare(preference.title, Qt::CaseInsensitive) == 0;
        const auto languageMatches =
            !preference.language.isEmpty() && language.compare(preference.language, Qt::CaseInsensitive) == 0;
        if (titleMatches && languageMatches) {
            return id;
        }
        if (fallbackMatch < 0 && (titleMatches || languageMatches)) {
            fallbackMatch = id;
        }
        if (defaultMatch < 0 && track.value(QStringLiteral("default")).toBool()) {
            defaultMatch = id;
        }
    }

    return fallbackMatch >= 0 ? fallbackMatch : defaultMatch;
}

void TimeshiftController::restoreTrackPreferences()
{
    if (!m_session.has_value() || !m_session->pendingTrackRestore) {
        return;
    }

    const auto tracks = m_playerController->player()->trackList();
    const auto audioId = findTrackIdForPreference(tracks, QStringLiteral("audio"), m_session->preferredAudioTrack);
    if (audioId > 0) {
        m_playerController->player()->selectAudioTrack(audioId);
    }

    const auto subtitleId = findTrackIdForPreference(tracks, QStringLiteral("sub"), m_session->preferredSubtitleTrack);
    if (subtitleId >= 0) {
        m_playerController->player()->selectSubtitleTrack(subtitleId);
    }

    m_session->pendingTrackRestore = false;
}

bool TimeshiftController::seekToEpochMs(const qint64 requestedEpochMs, const QString &logContext, const bool preferDirect)
{
    if (!isActive() || !m_session.has_value()) {
        return false;
    }

    const auto globalStart = windowStartEpochMs();
    const auto globalLive = liveEdgeEpochMs();
    if (globalStart <= 0 || globalLive <= globalStart) {
        return false;
    }

    const auto clampedTarget = std::clamp(requestedEpochMs, globalStart, globalLive);
    if (clampedTarget != requestedEpochMs) {
        setNoticeText(QStringLiteral("Required segment is not available"), kOutOfRetentionNoticeDurationMs);
        return false;
    }

    QString targetSessionId;
    qint64 resolvedTargetEpochMs = 0;
    auto snappedGap = false;
    if (!resolveSeekTarget(clampedTarget, &targetSessionId, &resolvedTargetEpochMs, &snappedGap)
        || targetSessionId.trimmed().isEmpty()
        || resolvedTargetEpochMs <= 0) {
        return false;
    }

    if (targetSessionId != m_session->id && !activateRetainedSession(targetSessionId)) {
        return false;
    }
    if (!m_session.has_value()) {
        return false;
    }

    if (snappedGap) {
        setNoticeText(
            QStringLiteral("Requested time was unavailable. Jumped to the next available segment."),
            kGapSnapNoticeDurationMs);
    } else {
        setNoticeText(QString {});
    }

    const auto sessionTarget = clampedPlaybackAnchorEpochMs(resolvedTargetEpochMs);
    if (sessionTarget <= 0) {
        return false;
    }

    if (preferDirect && canUseDirectSeek(sessionTarget)) {
        directSeekToEpochMs(sessionTarget);
    } else {
        auto pauseWhenReady = m_playerController->player()->pauseState().value_or(false);
        if (m_session->playbackLoadPending) {
            pauseWhenReady = m_session->pauseWhenReady;
        }
        playSessionFromAnchor(sessionTarget, pauseWhenReady);
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("timeshift.seek"),
        QStringLiteral("%1 -> retained-target=%2s mode=%3 session=%4 gap-snap=%5.")
            .arg(logContext)
            .arg(millisecondsToSeconds(sessionTarget - globalStart), 0, 'f', 2)
            .arg(m_session->lastSeekModeText)
            .arg(m_session->id)
            .arg(snappedGap ? QStringLiteral("yes") : QStringLiteral("no")));
    emit stateChanged();
    return true;
}

bool TimeshiftController::handlePauseRequest()
{
    if (!enabled() || m_multiViewController->isActive()) {
        return false;
    }
    return startSessionForCurrentChannel(true, QStringLiteral("pause-request"));
}

void TimeshiftController::handleUserStopRequest()
{
    if (!m_session.has_value() && m_retainedSessions.empty()) {
        return;
    }

    stopSession(false, QStringLiteral("user-stop-request"), false, true);
    m_restartWhenSinglePlaybackReturns = false;
}

void TimeshiftController::handleUserChannelSwitchRequest(const Core::Channel &nextChannel)
{
    if (!m_session.has_value()) {
        return;
    }

    if (m_session->channel.profileId == nextChannel.profileId && m_session->channel.id == nextChannel.id) {
        return;
    }

    stopSession(false, QStringLiteral("user-channel-switch"), false, true);
    m_restartWhenSinglePlaybackReturns = false;
}

bool TimeshiftController::handlePlaybackFailure(const QString &reason)
{
    if (!m_session.has_value() || !isActive()) {
        return false;
    }

    if (reason == QLatin1String("playback-ended") && tryRecoverNearLivePlaybackEnded()) {
        return true;
    }

    if (handlePlaybackStarvation(reason)) {
        return true;
    }

    scheduleReconnectGeneration(QStringLiteral("playback-failure-%1").arg(reason));
    Core::DebugLogger::instance().log(
        QStringLiteral("timeshift.session.error"),
        QStringLiteral("Timeshift playback failure handled without teardown (%1).")
            .arg(reason));
    return true;
}

bool TimeshiftController::handlePlaybackStarvation(const QString &reason)
{
    if (!m_session.has_value() || !isActive()) {
        return false;
    }

    if (m_session->playbackLoadPending) {
        return true;
    }

    const auto nowUtc = QDateTime::currentDateTimeUtc();
    const auto terminalReason = reason == QLatin1String("playback-ended");
    if (!terminalReason
        && m_session->lastLocalRecoveryUtc.isValid()
        && m_session->lastLocalRecoveryUtc.msecsTo(nowUtc) < kLocalPlaybackRecoveryCooldownMs) {
        return true;
    }

    attachSessionPlaybackIfReady();
    if (!m_session.has_value()) {
        return false;
    }
    const auto &session = *m_session;

    auto sessionStartEpochMs = sessionWindowStartEpochMs(session);
    auto sessionLiveEpochMs = sessionLiveEdgeEpochMs(session);
    auto currentEpochMs = currentPlaybackEpochMs();
    if (currentEpochMs <= 0) {
        currentEpochMs = sessionCurrentPlaybackEpochMs(session);
    }

    const auto hasPlaybackWindow =
        sessionStartEpochMs > 0 && sessionLiveEpochMs > sessionStartEpochMs && currentEpochMs > 0;
    auto remainingSeconds = 0.0;
    if (hasPlaybackWindow) {
        remainingSeconds = std::max(0.0, millisecondsToSeconds(sessionLiveEpochMs - currentEpochMs));
    }

    const auto failedSession = m_session->state == SessionState::Failed;
    const auto shouldAttemptGenerationSwitch = terminalReason
        || (hasPlaybackWindow && remainingSeconds <= 1.0)
        || failedSession;
    if (shouldAttemptGenerationSwitch && switchToNextGenerationPlayback(reason, false)) {
        return true;
    }

    if (failedSession) {
        scheduleReconnectGeneration(QStringLiteral("starvation-%1").arg(reason));
    }

    if (sessionStartEpochMs <= 0 || sessionLiveEpochMs <= sessionStartEpochMs) {
        return true;
    }
    if (currentEpochMs <= 0) {
        currentEpochMs = std::clamp(preferredLiveEdgeAnchorEpochMs(), sessionStartEpochMs, sessionLiveEpochMs);
    }
    if (currentEpochMs <= 0) {
        currentEpochMs = sessionStartEpochMs;
    }
    const auto targetEpochMs = std::clamp(currentEpochMs, sessionStartEpochMs, sessionLiveEpochMs);

    m_session->lastLocalRecoveryUtc = nowUtc;
    m_session->localRecoveryCount += 1;
    const auto pauseWhenReady = m_playerController->player()->pauseState().value_or(false);
    playSessionFromAnchor(targetEpochMs, pauseWhenReady);
    const auto targetTimestamp = QDateTime::fromMSecsSinceEpoch(targetEpochMs).toLocalTime();
    Core::DebugLogger::instance().log(
        QStringLiteral("timeshift.seek"),
        QStringLiteral("Local playback recovery #%1 (%2): target=%3 behind=%4s.")
            .arg(m_session->localRecoveryCount)
            .arg(reason)
            .arg(targetTimestamp.isValid() ? targetTimestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) : QStringLiteral("N/A"))
            .arg(behindLiveSeconds(), 0, 'f', 2));
    return true;
}

bool TimeshiftController::tryRecoverNearLivePlaybackEnded()
{
    if (!m_session.has_value() || !isActive()) {
        return false;
    }

    if (m_session->playbackLoadPending) {
        return false;
    }

    if (m_playerController->player()->pauseState().value_or(false)) {
        return false;
    }

    const auto nowUtc = QDateTime::currentDateTimeUtc();
    if (m_session->lastNearLiveEofRecoveryUtc.isValid()
        && m_session->lastNearLiveEofRecoveryUtc.msecsTo(nowUtc) < kNearLivePlaybackEndedRecoveryCooldownMs) {
        return false;
    }

    if (!m_session->playlistInfo.valid || !m_session->lastPlaylistAdvanceUtc.isValid()) {
        return false;
    }

    const auto playlistAgeMs = m_session->lastPlaylistAdvanceUtc.msecsTo(nowUtc);
    if (playlistAgeMs < 0 || playlistAgeMs > kNearLivePlaybackEndedPlaylistFreshnessMs) {
        return false;
    }

    const auto behindSeconds = behindLiveSeconds();
    const auto thresholdSeconds = liveBadgeThresholdSeconds(
        m_settings->current().playerBufferSeconds,
        m_settings->current().timeshiftSegmentSeconds);
    if (behindSeconds > thresholdSeconds) {
        return false;
    }

    const auto targetEpochMs = preferredLiveEdgeAnchorEpochMs();
    if (targetEpochMs <= 0) {
        return false;
    }

    m_session->lastNearLiveEofRecoveryUtc = nowUtc;
    const auto pauseWhenReady = m_playerController->player()->pauseState().value_or(false);
    playSessionFromAnchor(targetEpochMs, pauseWhenReady);
    Core::DebugLogger::instance().log(
        QStringLiteral("timeshift.seek"),
        QStringLiteral("Recovered near-live playback-ended via delayed reattach: behind=%1s threshold=%2s playlist-age=%3ms.")
            .arg(behindSeconds, 0, 'f', 2)
            .arg(thresholdSeconds, 0, 'f', 2)
            .arg(playlistAgeMs));
    return true;
}

bool TimeshiftController::seekRelative(const double seconds)
{
    if (!isActive()) {
        return false;
    }

    const auto targetEpochMs = targetEpochMsFromCurrentOffset(seconds);
    if (targetEpochMs <= 0) {
        return false;
    }

    const auto delta = std::abs(seconds);
    if (delta < 0.05) {
        resumePlaybackAfterTransportAction();
        return true;
    }

    return seekToEpochMs(
        targetEpochMs,
        QStringLiteral("Relative seek %1s").arg(seconds, 0, 'f', 1),
        true);
}

bool TimeshiftController::seekToFraction(const double fraction)
{
    if (!isActive()) {
        return false;
    }

    const auto normalized = std::clamp(fraction, 0.0, 1.0);
    if (normalized >= 0.995) {
        return jumpToLiveEdge();
    }

    return seekToEpochMs(
        targetEpochMsFromFraction(normalized),
        QStringLiteral("Timeline seek fraction=%1").arg(normalized, 0, 'f', 3),
        true);
}

bool TimeshiftController::jumpToLiveEdge()
{
    if (!isActive()) {
        return false;
    }

    const auto liveTargetEpoch = preferredLiveEdgeAnchorEpochMs();
    if (liveTargetEpoch <= 0) {
        return false;
    }
    const auto result = seekToEpochMs(liveTargetEpoch, QStringLiteral("Jump live"), true);
    if (result) {
        Core::DebugLogger::instance().log(QStringLiteral("timeshift.jump_live"), QStringLiteral("Jumped to live edge."));
    }
    return result;
}

void TimeshiftController::applySettings()
{
    if (!enabled()) {
        stopSession(true, QStringLiteral("settings-disabled"));
        m_restartWhenSinglePlaybackReturns = false;
        emit stateChanged();
        return;
    }

    if (m_multiViewController->isActive()) {
        emit stateChanged();
        return;
    }

    if (!m_session.has_value()) {
        startSessionForCurrentChannel(false, QStringLiteral("settings-apply"));
    }
    emit stateChanged();
}

void TimeshiftController::shutdownForApplicationExit()
{
    Core::DebugLogger::instance().log(
        QStringLiteral("timeshift.session.stop"),
        QStringLiteral("Application shutdown requested. Forcing immediate timeshift teardown."));
    stopSession(false, QStringLiteral("application-shutdown"), false, true);
    if (m_playbackServer.isListening()) {
        m_playbackServer.close();
    }
}

QString TimeshiftController::findFfmpegBinary()
{
    return Core::resolveProcessBinary(QStringLiteral("ffmpeg"));
}

QString TimeshiftController::findFfprobeBinary()
{
    return Core::resolveProcessBinary(QStringLiteral("ffprobe"));
}

QString TimeshiftController::sessionFolderName(const Channel &channel)
{
    return QStringLiteral("%1_%2_%3")
        .arg(guidToString(channel.profileId))
        .arg(channel.id)
        .arg(sessionTimestamp());
}

TimeshiftController::PlaylistInfo TimeshiftController::parsePlaylistFile(const QString &playlistPath)
{
    PlaylistInfo info;
    const auto snapshot = parsePlaylistSnapshot(playlistPath);
    info.windowStartUtc = snapshot.windowStartUtc;
    info.liveEdgeUtc = snapshot.liveEdgeUtc;
    info.availableSeconds = snapshot.availableSeconds;
    info.valid = snapshot.windowStartUtc.isValid()
        && snapshot.liveEdgeUtc.isValid()
        && snapshot.availableSeconds > 0.0
        && !snapshot.segments.isEmpty();
    return info;
}

QString TimeshiftController::stateName(const SessionState state)
{
    switch (state) {
    case SessionState::Idle:
        return QStringLiteral("idle");
    case SessionState::Starting:
        return QStringLiteral("starting");
    case SessionState::Running:
        return QStringLiteral("running");
    case SessionState::Failed:
        return QStringLiteral("failed");
    case SessionState::Stopping:
        return QStringLiteral("stopping");
    }

    return QStringLiteral("unknown");
}

int TimeshiftController::reconnectBackoffMs(const int attempt)
{
    const auto shift = std::clamp(attempt, 0, 16);
    const auto scaled = kReconnectBackoffInitialMs * (1 << shift);
    return std::min(kReconnectBackoffCapMs, scaled);
}

QString TimeshiftController::rootDirectory() const
{
    auto configured = configuredStorageDirectory();
    if (!configured.isEmpty()) {
        QDir().mkpath(configured);
        return configured;
    }
    return AppDataPaths::timeshiftDirectory();
}

QString TimeshiftController::configuredStorageDirectory() const
{
    return m_settings->current().timeshiftStorageDirectory.trimmed();
}

qint64 TimeshiftController::sessionWindowStartEpochMs(const Session &session) const
{
    return session.playlistInfo.windowStartUtc.isValid()
        ? session.playlistInfo.windowStartUtc.toMSecsSinceEpoch()
        : 0;
}

qint64 TimeshiftController::sessionLiveEdgeEpochMs(const Session &session) const
{
    return session.playlistInfo.liveEdgeUtc.isValid()
        ? session.playlistInfo.liveEdgeUtc.toMSecsSinceEpoch()
        : 0;
}

qint64 TimeshiftController::sessionOldestSegmentEpochMs(const Session &session) const
{
    const auto snapshot = parsePlaylistSnapshot(session.playlistPath);
    if (snapshot.valid && !snapshot.segments.isEmpty()) {
        for (const auto &segment : snapshot.segments) {
            if (segment.programDateTimeUtc.isValid()) {
                return segment.programDateTimeUtc.toMSecsSinceEpoch();
            }
        }
    }
    return sessionWindowStartEpochMs(session);
}

qint64 TimeshiftController::sessionReliableStartEpochMs(const Session &session) const
{
    const auto startEpochMs = sessionWindowStartEpochMs(session);
    const auto liveEpochMs = sessionLiveEdgeEpochMs(session);
    if (startEpochMs <= 0) {
        return 0;
    }
    if (liveEpochMs <= startEpochMs) {
        return startEpochMs;
    }

    const auto snapshot = parsePlaylistSnapshot(session.playlistPath);
    if (!snapshot.valid || snapshot.segments.isEmpty()) {
        return startEpochMs;
    }

    const auto segmentSeconds = std::clamp(m_settings->current().timeshiftSegmentSeconds, 2, 60);
    const auto reliabilityLeadSeconds = std::max(4, segmentSeconds * 2);
    const auto desiredEpochMs =
        std::min(liveEpochMs, startEpochMs + static_cast<qint64>(reliabilityLeadSeconds) * 1000);

    auto anchorIndex = anchorSegmentIndexForProgramDateTime(snapshot, desiredEpochMs);
    if (anchorIndex < 0) {
        const auto targetOffsetSeconds =
            std::clamp(millisecondsToSeconds(desiredEpochMs - startEpochMs), 0.0, snapshot.availableSeconds);
        anchorIndex = anchorSegmentIndexForOffsetSeconds(snapshot, targetOffsetSeconds);
    }
    if (anchorIndex < 0 || anchorIndex >= snapshot.segments.size()) {
        return startEpochMs;
    }

    const auto &segment = snapshot.segments.at(anchorIndex);
    if (segment.programDateTimeUtc.isValid()) {
        return segment.programDateTimeUtc.toMSecsSinceEpoch();
    }
    return startEpochMs + static_cast<qint64>(std::llround(cumulativeSecondsBeforeSegment(snapshot, anchorIndex) * 1000.0));
}

qint64 TimeshiftController::sessionCurrentPlaybackEpochMs(const Session &session) const
{
    const auto attachedStart = std::max<qint64>(0, session.attachedWindowStartEpochMs);
    const auto sessionLiveEdge = sessionLiveEdgeEpochMs(session);
    if (attachedStart <= 0 || sessionLiveEdge <= 0) {
        return 0;
    }
    const auto playbackMs =
        attachedStart + static_cast<qint64>(std::llround(std::max(0.0, m_playerController->playbackPositionSeconds()) * 1000.0));
    return std::clamp(playbackMs, attachedStart, sessionLiveEdge);
}

TimeshiftController::Session *TimeshiftController::findRetainedSessionById(const QString &sessionId)
{
    for (auto &session : m_retainedSessions) {
        if (session.id == sessionId) {
            return &session;
        }
    }
    return nullptr;
}

const TimeshiftController::Session *TimeshiftController::findRetainedSessionById(const QString &sessionId) const
{
    for (const auto &session : m_retainedSessions) {
        if (session.id == sessionId) {
            return &session;
        }
    }
    return nullptr;
}

TimeshiftController::Session *TimeshiftController::findAnySessionById(const QString &sessionId)
{
    if (m_session.has_value() && m_session->id == sessionId) {
        return &m_session.value();
    }
    return findRetainedSessionById(sessionId);
}

const TimeshiftController::Session *TimeshiftController::findAnySessionById(const QString &sessionId) const
{
    if (m_session.has_value() && m_session->id == sessionId) {
        return &m_session.value();
    }
    return findRetainedSessionById(sessionId);
}

std::vector<const TimeshiftController::Session *> TimeshiftController::allSessionsSortedByStart() const
{
    std::vector<const Session *> sessions;
    if (m_session.has_value()) {
        sessions.push_back(&m_session.value());
    }
    for (const auto &session : m_retainedSessions) {
        sessions.push_back(&session);
    }
    std::sort(sessions.begin(), sessions.end(), [this](const Session *lhs, const Session *rhs) {
        const auto lhsStart = sessionWindowStartEpochMs(*lhs);
        const auto rhsStart = sessionWindowStartEpochMs(*rhs);
        if (lhsStart == rhsStart) {
            return sessionLiveEdgeEpochMs(*lhs) < sessionLiveEdgeEpochMs(*rhs);
        }
        return lhsStart < rhsStart;
    });
    return sessions;
}

qint64 TimeshiftController::globalWindowStartEpochMs() const
{
    qint64 earliest = 0;
    for (const auto *session : allSessionsSortedByStart()) {
        const auto start = sessionWindowStartEpochMs(*session);
        if (start <= 0) {
            continue;
        }
        if (earliest <= 0 || start < earliest) {
            earliest = start;
        }
    }
    return earliest;
}

qint64 TimeshiftController::globalLiveEdgeEpochMs() const
{
    qint64 latest = 0;
    for (const auto *session : allSessionsSortedByStart()) {
        const auto live = sessionLiveEdgeEpochMs(*session);
        if (live > latest) {
            latest = live;
        }
    }
    return latest;
}

void TimeshiftController::refreshSessionPlaylistInfo(Session &session)
{
    const auto wasValid = session.playlistInfo.valid;
    auto info = parsePlaylistFile(session.playlistPath);
    if (!info.valid || info.availableSeconds <= 0.0) {
        return;
    }
    const auto liveEdgeChanged = !session.playlistInfo.liveEdgeUtc.isValid()
        || info.liveEdgeUtc != session.playlistInfo.liveEdgeUtc;
    session.playlistInfo = info;
    if (session.state == SessionState::Starting) {
        session.state = SessionState::Running;
    }
    if (!wasValid
        && session.reconnectReadyOldestEpochMs <= 0
        && session.startReason.startsWith(QStringLiteral("reconnect"), Qt::CaseInsensitive)
        && session.playlistInfo.windowStartUtc.isValid()) {
        session.reconnectReadyOldestEpochMs = session.playlistInfo.windowStartUtc.toMSecsSinceEpoch();
        Core::DebugLogger::instance().log(
            QStringLiteral("timeshift.session.start"),
            QStringLiteral("Reconnect generation %1 became playable at oldest=%2.")
                .arg(session.id)
                .arg(session.playlistInfo.windowStartUtc.toUTC().toString(Qt::ISODateWithMs)));
    }
    if (liveEdgeChanged) {
        session.lastPlaylistAdvanceUtc = QDateTime::currentDateTimeUtc();
    }
}

void TimeshiftController::refreshRetainedSessionPlaylists()
{
    for (auto &session : m_retainedSessions) {
        refreshSessionPlaylistInfo(session);
    }
}

bool TimeshiftController::hasRunningIngestSession() const
{
    if (m_session.has_value()
        && m_session->ingestProcess
        && m_session->ingestProcess->state() != QProcess::NotRunning
        && !m_session->stopRequested) {
        return true;
    }
    for (const auto &session : m_retainedSessions) {
        if (session.ingestProcess
            && session.ingestProcess->state() != QProcess::NotRunning
            && !session.stopRequested) {
            return true;
        }
    }
    return false;
}

bool TimeshiftController::resolveSeekTarget(
    const qint64 requestedEpochMs,
    QString *sessionId,
    qint64 *resolvedEpochMs,
    bool *snappedGap) const
{
    if (sessionId == nullptr || resolvedEpochMs == nullptr || snappedGap == nullptr) {
        return false;
    }

    *sessionId = {};
    *resolvedEpochMs = 0;
    *snappedGap = false;

    const auto sessions = allSessionsSortedByStart();
    if (sessions.empty()) {
        return false;
    }

    const auto globalStart = globalWindowStartEpochMs();
    const auto globalLive = globalLiveEdgeEpochMs();
    if (globalStart <= 0 || globalLive <= globalStart) {
        return false;
    }

    const auto clampedTarget = std::clamp(requestedEpochMs, globalStart, globalLive);
    for (const auto *session : sessions) {
        const auto start = sessionWindowStartEpochMs(*session);
        const auto end = sessionLiveEdgeEpochMs(*session);
        if (start <= 0 || end <= start) {
            continue;
        }
        if (clampedTarget >= start && clampedTarget <= end) {
            *sessionId = session->id;
            *resolvedEpochMs = clampedTarget;
            return true;
        }
    }

    for (const auto *session : sessions) {
        const auto start = sessionWindowStartEpochMs(*session);
        const auto end = sessionLiveEdgeEpochMs(*session);
        if (start <= 0 || end <= start) {
            continue;
        }
        if (clampedTarget < start) {
            *sessionId = session->id;
            *resolvedEpochMs = start;
            *snappedGap = true;
            return true;
        }
    }

    const auto *lastSession = sessions.back();
    *sessionId = lastSession->id;
    *resolvedEpochMs = sessionLiveEdgeEpochMs(*lastSession);
    return true;
}

bool TimeshiftController::activateRetainedSession(const QString &sessionId)
{
    if (!m_session.has_value() || m_session->id == sessionId) {
        return m_session.has_value() && m_session->id == sessionId;
    }

    auto index = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = 0; i < m_retainedSessions.size(); ++i) {
        if (m_retainedSessions.at(i).id == sessionId) {
            index = i;
            break;
        }
    }
    if (index == std::numeric_limits<std::size_t>::max()) {
        return false;
    }

    auto current = std::move(m_session.value());
    m_session.reset();
    auto next = std::move(m_retainedSessions[index]);
    m_retainedSessions.erase(m_retainedSessions.begin() + static_cast<std::ptrdiff_t>(index));
    m_retainedSessions.push_back(std::move(current));
    m_session = std::move(next);
    emit stateChanged();
    return true;
}

bool TimeshiftController::switchToNextGenerationPlayback(const QString &reason, const bool emitNotice)
{
    if (!m_session.has_value()) {
        return false;
    }

    const auto switchingFromFailedSession = m_session->state == SessionState::Failed;
    const auto preferReconnectReadyAnchor = switchingFromFailedSession
        || reason == QLatin1String("failed-generation-deplete");
    const auto requireNewerLiveEdge = !switchingFromFailedSession
        && reason != QLatin1String("failed-generation-deplete");
    const auto currentLive = sessionLiveEdgeEpochMs(*m_session);
    Session *candidate = nullptr;
    qint64 candidateStart = 0;
    qint64 candidateSelectionAnchor = 0;
    for (auto &retained : m_retainedSessions) {
        const auto start = sessionOldestSegmentEpochMs(retained);
        const auto end = sessionLiveEdgeEpochMs(retained);
        if (start <= 0 || end <= start) {
            continue;
        }
        if (requireNewerLiveEdge && currentLive > 0 && end <= currentLive) {
            continue;
        }
        const auto selectionAnchor = preferReconnectReadyAnchor && retained.reconnectReadyOldestEpochMs > 0
            ? retained.reconnectReadyOldestEpochMs
            : start;
        if (candidate == nullptr || selectionAnchor < candidateSelectionAnchor) {
            candidate = &retained;
            candidateStart = start;
            candidateSelectionAnchor = selectionAnchor;
        }
    }
    if (candidate == nullptr || candidateStart <= 0) {
        return false;
    }

    const auto targetSessionId = candidate->id;
    if (!activateRetainedSession(targetSessionId) || !m_session.has_value()) {
        return false;
    }

    const auto pauseWhenReady = m_playerController->player()->pauseState().value_or(false);
    const auto oldestSegmentStart = sessionOldestSegmentEpochMs(*m_session);
    const auto preferOldestTarget = switchingFromFailedSession
        || reason == QLatin1String("failed-generation-deplete")
        || reason == QLatin1String("playback-ended");
    const auto reliableStart = preferOldestTarget ? 0 : sessionReliableStartEpochMs(*m_session);
    const auto oldestTarget = oldestSegmentStart > 0 ? oldestSegmentStart : candidateStart;
    const auto reconnectReadyTarget = preferReconnectReadyAnchor && m_session->reconnectReadyOldestEpochMs > 0
        ? m_session->reconnectReadyOldestEpochMs
        : 0;
    const auto oldestOrReconnectTarget = reconnectReadyTarget > 0 ? reconnectReadyTarget : oldestTarget;
    const auto switchTarget = reliableStart > 0 ? reliableStart : oldestOrReconnectTarget;
    playSessionFromAnchor(switchTarget, pauseWhenReady);
    if (emitNotice) {
        setNoticeText(QStringLiteral("Requested time was unavailable. Jumped to the next available segment."), kGapSnapNoticeDurationMs);
    }
    Core::DebugLogger::instance().log(
        QStringLiteral("timeshift.seek"),
        QStringLiteral("Switched playback generation (%1) to %2 at %3 (candidate-start=%4 oldest-segment=%5 reliable-start=%6 reconnect-ready=%7).")
            .arg(reason)
            .arg(m_session->id)
            .arg(QDateTime::fromMSecsSinceEpoch(switchTarget).toUTC().toString(Qt::ISODateWithMs))
            .arg(QDateTime::fromMSecsSinceEpoch(candidateStart).toUTC().toString(Qt::ISODateWithMs))
            .arg(oldestSegmentStart > 0
                     ? QDateTime::fromMSecsSinceEpoch(oldestSegmentStart).toUTC().toString(Qt::ISODateWithMs)
                     : QStringLiteral("n/a"))
            .arg(reliableStart > 0
                     ? QDateTime::fromMSecsSinceEpoch(reliableStart).toUTC().toString(Qt::ISODateWithMs)
                     : QStringLiteral("n/a"))
            .arg(reconnectReadyTarget > 0
                     ? QDateTime::fromMSecsSinceEpoch(reconnectReadyTarget).toUTC().toString(Qt::ISODateWithMs)
                     : QStringLiteral("n/a")));
    return true;
}

void TimeshiftController::scheduleReconnectGeneration(const QString &reason)
{
    if (!enabled() || !m_session.has_value() || m_multiViewController->isActive()) {
        return;
    }
    if (hasRunningIngestSession()) {
        m_reconnectGenerationAttempt = 0;
        return;
    }

    const auto delayMs = reconnectBackoffMs(m_reconnectGenerationAttempt);
    if (m_reconnectGenerationTimer.isActive()) {
        return;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("timeshift.session.start"),
        QStringLiteral("Scheduling reconnect generation retry in %1 ms (%2).")
            .arg(delayMs)
            .arg(reason));
    m_reconnectGenerationTimer.start(delayMs);
    m_reconnectGenerationAttempt = std::min(m_reconnectGenerationAttempt + 1, 16);
}

void TimeshiftController::cleanupSessionFiles(Session &session, const bool forceImmediateProcessKill)
{
    session.stopRequested = true;
    session.probeCompletionHandled = true;
    if (session.probeProcess) {
        session.probeProcess->kill();
        session.probeProcess.reset();
    }
    if (session.ingestProcess) {
        session.state = SessionState::Stopping;
        if (forceImmediateProcessKill) {
            session.ingestProcess->kill();
            session.ingestProcess->waitForFinished(250);
        } else {
            session.ingestProcess->terminate();
            if (!session.ingestProcess->waitForFinished(1000)) {
                session.ingestProcess->kill();
                session.ingestProcess->waitForFinished(1000);
            }
        }
        session.ingestProcess.reset();
    }

    if (!session.sessionDirectory.isEmpty()) {
        QDir(session.sessionDirectory).removeRecursively();
    }
}

void TimeshiftController::cleanupUnreachableRetainedSessions()
{
    if (m_retainedSessions.empty()) {
        return;
    }

    const auto globalLive = globalLiveEdgeEpochMs();
    if (globalLive <= 0) {
        return;
    }
    const auto floorEpoch = globalLive - static_cast<qint64>(windowSeconds()) * 1000;
    for (auto index = static_cast<int>(m_retainedSessions.size()) - 1; index >= 0; --index) {
        auto &session = m_retainedSessions[index];
        const auto endEpoch = sessionLiveEdgeEpochMs(session);
        if (endEpoch <= 0 || endEpoch >= floorEpoch) {
            continue;
        }
        cleanupSessionFiles(session);
        m_retainedSessions.erase(m_retainedSessions.begin() + index);
    }
}

void TimeshiftController::handleRuntimeIngestFailure(
    Session &session,
    const QString &reason,
    const bool fromStartup,
    const QString &fallbackStatusMessage)
{
    const auto isCurrent = m_session.has_value() && m_session->id == session.id;
    const auto wasPlaybackAttached = session.playbackAttached;
    const auto fallbackPlaybackUrl = session.fallbackPlaybackUrl;

    session.stopRequested = true;
    session.probeCompletionHandled = true;
    if (session.ingestProcess) {
        session.ingestProcess.reset();
    }

    if (fromStartup && isCurrent && !wasPlaybackAttached) {
        stopSession(false, reason);
        if (!fallbackPlaybackUrl.trimmed().isEmpty()) {
            emit statusMessageRequested(fallbackStatusMessage);
            emit uiTestPlaybackUrlObserved(
                QStringLiteral("timeshift.fallback-direct"),
                redactSensitiveText(fallbackPlaybackUrl));
            m_playerController->playCurrentPlaybackUrl(fallbackPlaybackUrl, false);
        }
        return;
    }

    session.stopRequested = false;
    session.state = SessionState::Failed;
    if (isCurrent) {
        emit statusMessageRequested(QStringLiteral("Source disconnected. Continuing from cache while reconnecting..."));
    }
    scheduleReconnectGeneration(reason);
}

bool TimeshiftController::startDetachedGeneration(const QString &reason)
{
    if (!m_session.has_value() || m_multiViewController->isActive()) {
        return false;
    }

    const auto currentChannel = m_playerController->currentChannelValue();
    if (!currentChannel.has_value() || !channelEligibleForTimeshift(currentChannel)) {
        return false;
    }
    const auto &channel = *currentChannel;

    const auto inputUrl = selectInputUrlForChannel(channel);
    if (inputUrl.trimmed().isEmpty()) {
        return false;
    }
    if (!ensurePlaybackServer()) {
        return false;
    }

    QString admissionFailure;
    if (!ensureDiskAdmission(inputUrl, &admissionFailure)) {
        emit statusMessageRequested(admissionFailure);
        return false;
    }

    Session session;
    session.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    session.state = SessionState::Starting;
    session.startReason = reason;
    session.channel = channel;
    session.inputUrl = inputUrl;
    session.fallbackPlaybackUrl = inputUrl;
    session.pauseWhenReady = false;
    session.sessionDirectory = QDir(rootDirectory()).filePath(sessionFolderName(channel));
    QDir().mkpath(session.sessionDirectory);
    ensureSessionMarkerFile(session.sessionDirectory);
    session.playlistPath = QDir(session.sessionDirectory).filePath(QStringLiteral("index.m3u8"));

    const auto segmentSeconds = std::clamp(m_settings->current().timeshiftSegmentSeconds, 2, 60);
    const auto listSize = std::max(8, static_cast<int>(std::ceil(windowSeconds() / static_cast<double>(segmentSeconds))));
    const auto ffmpeg = findFfmpegBinary();
    session.ingestProcess = std::make_unique<QProcess>(this);
    session.ingestProcess->setProcessChannelMode(QProcess::MergedChannels);
    auto *process = session.ingestProcess.get();
    const auto detachedId = session.id;
    connect(process, &QProcess::readyRead, this, [this, detachedId]() {
        auto *detached = findAnySessionById(detachedId);
        if (detached == nullptr || !detached->ingestProcess) {
            return;
        }
        auto output = QString::fromLocal8Bit(detached->ingestProcess->readAll()).trimmed();
        if (output.isEmpty()) {
            return;
        }
        output.replace(u'\r', u' ');
        output.replace(u'\n', QStringLiteral(" | "));
        Core::DebugLogger::instance().log(
            QStringLiteral("timeshift.session.start"),
            QStringLiteral("detached ffmpeg output[%1]: %2").arg(detachedId, output.left(600)));
    });
    connect(process, &QProcess::errorOccurred, this, [this, detachedId](const QProcess::ProcessError error) {
        auto *detached = findAnySessionById(detachedId);
        if (detached == nullptr || detached->stopRequested) {
            return;
        }
        Core::DebugLogger::instance().log(
            QStringLiteral("timeshift.session.error"),
            QStringLiteral("Detached ffmpeg process error (%1) for %2.")
                .arg(static_cast<int>(error))
                .arg(detachedId));
        handleRuntimeIngestFailure(
            *detached,
            QStringLiteral("detached-ffmpeg-error"),
            false);
    });
    connect(process, &QProcess::finished, this, [this, detachedId](const int exitCode, const QProcess::ExitStatus exitStatus) {
        auto *detached = findAnySessionById(detachedId);
        if (detached == nullptr || detached->stopRequested) {
            return;
        }
        Core::DebugLogger::instance().log(
            QStringLiteral("timeshift.session.error"),
            QStringLiteral("Detached ffmpeg exited unexpectedly (id=%1 exit=%2 status=%3).")
                .arg(detachedId)
                .arg(exitCode)
                .arg(exitStatus == QProcess::NormalExit ? QStringLiteral("normal") : QStringLiteral("crash")));
        handleRuntimeIngestFailure(
            *detached,
            QStringLiteral("detached-ffmpeg-finished"),
            false);
    });

    m_retainedSessions.push_back(std::move(session));
    auto &created = m_retainedSessions.back();
    QStringList args {
        QStringLiteral("-y"),
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("warning"),
        QStringLiteral("-nostdin"),
        QStringLiteral("-i"), created.inputUrl,
        QStringLiteral("-map"), QStringLiteral("0:v?"),
        QStringLiteral("-map"), QStringLiteral("0:a?"),
        QStringLiteral("-sn"),
        QStringLiteral("-c"), QStringLiteral("copy"),
        QStringLiteral("-f"), QStringLiteral("hls"),
        QStringLiteral("-hls_time"), QString::number(segmentSeconds),
        QStringLiteral("-hls_list_size"), QString::number(listSize),
        QStringLiteral("-hls_flags"), timeshiftHlsFlags(),
        created.playlistPath
    };
    Core::DebugLogger::instance().log(
        QStringLiteral("timeshift.session.start"),
        QStringLiteral("Starting detached reconnect generation %1 (%2).")
            .arg(created.id)
            .arg(reason));
    created.ingestProcess->start(ffmpeg, args);
    emit stateChanged();
    return true;
}

void TimeshiftController::attemptReconnectGeneration()
{
    if (!enabled() || !m_session.has_value() || m_multiViewController->isActive()) {
        return;
    }
    if (hasRunningIngestSession()) {
        m_reconnectGenerationAttempt = 0;
        return;
    }

    if (startDetachedGeneration(QStringLiteral("reconnect-attempt"))) {
        m_reconnectGenerationAttempt = 0;
        return;
    }
    scheduleReconnectGeneration(QStringLiteral("reconnect-retry"));
}

qint64 TimeshiftController::preferredLiveEdgeAnchorEpochMs() const
{
    if (!m_session.has_value() || !m_session->playlistInfo.liveEdgeUtc.isValid()) {
        return 0;
    }

    const auto liveEpochMs = liveEdgeEpochMs();
    const auto retainedStartEpochMs = windowStartEpochMs();
    if (liveEpochMs <= 0 || retainedStartEpochMs <= 0) {
        return 0;
    }

    const auto liveBufferSeconds = effectiveLiveEdgeSafetyBufferSeconds(
        m_settings->current().playerBufferSeconds,
        m_settings->current().timeshiftSegmentSeconds);
    const auto liveBufferMs = static_cast<qint64>(std::llround(
        liveBufferSeconds * 1000.0));
    return std::max(retainedStartEpochMs, liveEpochMs - liveBufferMs);
}

qint64 TimeshiftController::clampedPlaybackAnchorEpochMs(const qint64 targetEpochMs) const
{
    if (!m_session.has_value()) {
        return 0;
    }
    const auto minEpochMs = sessionWindowStartEpochMs(*m_session);
    const auto maxEpochMs = sessionLiveEdgeEpochMs(*m_session);
    if (minEpochMs <= 0 || maxEpochMs <= 0) {
        return 0;
    }
    return std::clamp(targetEpochMs, minEpochMs, maxEpochMs);
}

qint64 TimeshiftController::anchoredPlaybackStartEpochMs(
    const Session &session,
    const qint64 targetEpochMs,
    const QString &playlistPath) const
{
    const auto retainedStart = sessionWindowStartEpochMs(session);
    const auto liveEpoch = sessionLiveEdgeEpochMs(session);
    if (retainedStart <= 0 || liveEpoch <= retainedStart) {
        return 0;
    }

    const auto snapshot = parsePlaylistSnapshot(playlistPath);
    if (!snapshot.valid || snapshot.segments.isEmpty()) {
        return std::clamp(targetEpochMs, retainedStart, liveEpoch);
    }

    const auto clampedTargetEpochMs = std::clamp(targetEpochMs, retainedStart, liveEpoch);
    auto anchorIndex = anchorSegmentIndexForProgramDateTime(snapshot, clampedTargetEpochMs);
    if (anchorIndex < 0) {
        const auto targetOffsetSeconds =
            std::clamp(millisecondsToSeconds(clampedTargetEpochMs - retainedStart), 0.0, snapshot.availableSeconds);
        anchorIndex = anchorSegmentIndexForOffsetSeconds(snapshot, targetOffsetSeconds);
    }
    const auto offsetBeforeAnchor = cumulativeSecondsBeforeSegment(snapshot, anchorIndex);
    if (snapshot.segments.at(anchorIndex).programDateTimeUtc.isValid()) {
        return snapshot.segments.at(anchorIndex).programDateTimeUtc.toMSecsSinceEpoch();
    }
    return retainedStart + static_cast<qint64>(std::llround(offsetBeforeAnchor * 1000.0));
}

void TimeshiftController::playSessionFromAnchor(const qint64 targetEpochMs, const bool pauseWhenReady)
{
    if (!m_session.has_value()) {
        return;
    }
    const auto &session = *m_session;

    const auto requestedEpochMs = clampedPlaybackAnchorEpochMs(targetEpochMs);
    if (requestedEpochMs <= 0) {
        return;
    }
    const auto segmentSeconds = std::clamp(m_settings->current().timeshiftSegmentSeconds, 2, 60);
    const auto prerollSeconds = std::max(
        kDelayedAttachPrerollMinSeconds,
        segmentSeconds * kDelayedAttachPrerollSegmentMultiplier);
    const auto prerollMs = static_cast<qint64>(prerollSeconds) * 1000;
    const auto sessionStartEpochMs = sessionWindowStartEpochMs(session);
    if (sessionStartEpochMs <= 0) {
        return;
    }
    const auto anchorRequestEpochMs =
        clampedPlaybackAnchorEpochMs(std::max<qint64>(sessionStartEpochMs, requestedEpochMs - prerollMs));
    const auto delayedStartEpochMs = anchoredPlaybackStartEpochMs(session, anchorRequestEpochMs, session.playlistPath);
    const auto playbackUrl = delayedPlaybackUrl(session, delayedStartEpochMs, requestedEpochMs);
    if (playbackUrl.trimmed().isEmpty()) {
        return;
    }

    if (m_session->playbackAttached) {
        captureTrackPreferences();
    }

    if (m_session->playbackLoadPending && m_session->pendingPlaybackUrl == playbackUrl) {
        const auto startupAttach = !m_session->playbackAttached;
        m_session->pendingPlaybackAnchorEpochMs = delayedStartEpochMs;
        m_session->pendingPlaybackTargetEpochMs = requestedEpochMs;
        m_session->pendingPlaybackRequestedUtc = QDateTime::currentDateTimeUtc();
        m_session->pendingPostLoadSeekToleranceSeconds =
            startupAttach ? kStartupTargetToleranceSeconds : -1.0;
        m_session->resumeAfterLoad = !pauseWhenReady;
        m_session->pauseWhenReady = pauseWhenReady;
        m_session->lastSeekModeText = QStringLiteral("Delayed");
        emit stateChanged();
        return;
    }

    const auto startupAttach = !m_session->playbackAttached;
    m_session->pendingPlaybackAnchorEpochMs = delayedStartEpochMs;
    m_session->pendingPlaybackTargetEpochMs = requestedEpochMs;
    m_session->pendingPlaybackRequestedUtc = QDateTime::currentDateTimeUtc();
    m_session->pendingPlaybackUrl = playbackUrl;
    m_session->pendingPostLoadSeekSeconds = -1.0;
    m_session->pendingPostLoadSeekToleranceSeconds =
        startupAttach ? kStartupTargetToleranceSeconds : -1.0;
    m_session->pendingPostLoadSeekVerified = false;
    m_session->playbackLoadPending = true;
    m_session->resumeAfterLoad = !pauseWhenReady;
    m_session->pauseWhenReady = pauseWhenReady;
    m_session->lastSeekModeText = QStringLiteral("Delayed");
    emit uiTestPlaybackUrlObserved(QStringLiteral("timeshift.pending-playback"), redactSensitiveText(playbackUrl));
    m_playerController->playCurrentPlaybackUrl(playbackUrl, pauseWhenReady, QString::fromLatin1(kTimeshiftLoadfileOptions));
    emit stateChanged();
}

void TimeshiftController::resumePlaybackAfterTransportAction()
{
    auto paused = m_playerController->player()->pauseState();
    if (!paused.has_value() || !paused.value()) {
        return;
    }

    m_playerController->player()->setPaused(false);
}

void TimeshiftController::handlePlaybackFileLoaded()
{
    if (!m_session.has_value()
        || !m_session->playbackLoadPending) {
        return;
    }

    const auto pendingTargetEpochMs = m_session->pendingPlaybackTargetEpochMs;
    const auto pendingAnchorEpochMs = m_session->pendingPlaybackAnchorEpochMs;
    m_session->delayedStartEpochMs = pendingAnchorEpochMs;
    m_session->attachedWindowStartEpochMs =
        pendingAnchorEpochMs > 0 ? pendingAnchorEpochMs : sessionWindowStartEpochMs(*m_session);
    m_session->attachedWindowEndEpochMs = sessionLiveEdgeEpochMs(*m_session);
    const auto currentPlaybackUrl = m_playerController->currentPlaybackUrl().trimmed();
    m_session->playbackUrl = currentPlaybackUrl.isEmpty() ? m_session->pendingPlaybackUrl : currentPlaybackUrl;
    emit uiTestPlaybackUrlObserved(
        QStringLiteral("timeshift.playback"),
        redactSensitiveText(m_session->playbackUrl));
    m_session->pendingPlaybackAnchorEpochMs = 0;
    m_session->pendingPlaybackTargetEpochMs = 0;
    m_session->pendingPlaybackRequestedUtc = {};
    m_session->pendingPlaybackUrl.clear();
    m_session->playbackLoadPending = false;
    m_session->playbackAttached = true;

    const auto pendingSeekSeconds =
        pendingTargetEpochMs > 0 && pendingAnchorEpochMs > 0
        ? std::max(0.0, millisecondsToSeconds(pendingTargetEpochMs - pendingAnchorEpochMs))
        : -1.0;
    m_session->pendingPostLoadSeekSeconds =
        pendingSeekSeconds >= 0.0 ? pendingSeekSeconds : -1.0;
    m_session->pendingPostLoadSeekVerified = false;

    restoreTrackPreferences();
    QTimer::singleShot(kPostLoadVerifyDelayMs, this, [this]() {
        applyPendingPostLoadSeekIfReady(QStringLiteral("post-load-verify"));
    });
    emit stateChanged();
}

void TimeshiftController::finalizePendingPlaybackLoadIfReady(const QString &source)
{
    if (!m_session.has_value()
        || !m_session->playbackLoadPending
        || m_session->pendingPlaybackUrl.trimmed().isEmpty()
        || m_playerController->currentPlaybackUrl().trimmed() != m_session->pendingPlaybackUrl.trimmed()) {
        return;
    }

    if (m_playerController->isLoading()) {
        return;
    }

    const auto elapsedMs = m_session->pendingPlaybackRequestedUtc.isValid()
        ? m_session->pendingPlaybackRequestedUtc.msecsTo(QDateTime::currentDateTimeUtc())
        : 0;
    if (m_playerController->isPlaying()) {
        if (elapsedMs < 300) {
            return;
        }
        Core::DebugLogger::instance().log(
            QStringLiteral("timeshift.seek"),
            QStringLiteral("Finalizing pending delayed playback via %1 while playing after %2 ms.")
                .arg(source, QString::number(elapsedMs)));
        handlePlaybackFileLoaded();
        return;
    }
    if (elapsedMs < 1500) {
        return;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("timeshift.seek"),
        QStringLiteral("Finalizing pending delayed playback via %1 fallback after %2 ms.")
            .arg(source, QString::number(elapsedMs)));
    handlePlaybackFileLoaded();
}

void TimeshiftController::applyPendingPostLoadSeekIfReady(const QString &source)
{
    if (!m_session.has_value()
        || !m_session->playbackAttached
        || m_session->playbackLoadPending
        || m_session->playbackUrl.trimmed().isEmpty()
        || m_playerController->currentPlaybackUrl().trimmed() != m_session->playbackUrl.trimmed()) {
        return;
    }

    if (!std::isfinite(m_session->pendingPostLoadSeekSeconds) || m_session->pendingPostLoadSeekSeconds < 0.0) {
        return;
    }
    if (m_session->pendingPostLoadSeekVerified) {
        return;
    }

    if (m_playerController->isLoading()) {
        return;
    }

    const auto targetSeconds = m_session->pendingPostLoadSeekSeconds;
    const auto currentSeconds = std::max(0.0, m_playerController->playbackPositionSeconds());
    auto seekToleranceSeconds = m_session->pendingPostLoadSeekToleranceSeconds;
    if (!std::isfinite(seekToleranceSeconds) || seekToleranceSeconds <= 0.0) {
        seekToleranceSeconds =
            std::max(
                kPostLoadSeekToleranceFloorSeconds,
                static_cast<double>(std::clamp(m_settings->current().timeshiftSegmentSeconds, 2, 60)));
    }
    m_session->pendingPostLoadSeekVerified = true;
    if (std::abs(currentSeconds - targetSeconds) <= seekToleranceSeconds) {
        if (m_session->resumeAfterLoad) {
            m_playerController->player()->setPaused(false);
            m_session->resumeAfterLoad = false;
        }
        m_session->pendingPostLoadSeekSeconds = -1.0;
        m_session->pendingPostLoadSeekToleranceSeconds = -1.0;
        return;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("timeshift.seek"),
        QStringLiteral("Applying single post-load delayed seek to %1s via %2 (current=%3s).")
            .arg(targetSeconds, 0, 'f', 3)
            .arg(source)
            .arg(currentSeconds, 0, 'f', 3));
    m_playerController->player()->seekAbsolute(targetSeconds);
    if (m_session->resumeAfterLoad) {
        m_playerController->player()->setPaused(false);
        m_session->resumeAfterLoad = false;
    }
    m_session->pendingPostLoadSeekSeconds = -1.0;
    m_session->pendingPostLoadSeekToleranceSeconds = -1.0;
}

bool TimeshiftController::ensurePlaybackServer()
{
    if (m_playbackServer.isListening()) {
        return true;
    }

    if (!m_playbackServer.listen(QHostAddress::LocalHost, kPlaybackServerPort)) {
        Core::DebugLogger::instance().log(
            QStringLiteral("timeshift.session.error"),
            QStringLiteral("Timeshift playback server failed to listen on localhost: %1")
                .arg(m_playbackServer.errorString()));
        return false;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("timeshift.session.start"),
        QStringLiteral("Timeshift playback server listening on http://127.0.0.1:%1/")
            .arg(m_playbackServer.serverPort()));
    emit uiTestPlaybackUrlObserved(
        QStringLiteral("timeshift.local-server"),
        redactSensitiveText(QStringLiteral("http://127.0.0.1:%1/").arg(m_playbackServer.serverPort())));
    return true;
}

QString TimeshiftController::localPlaybackUrl(const Session &session) const
{
    if (!m_playbackServer.isListening()) {
        return {};
    }

    return QStringLiteral("http://127.0.0.1:%1/%2/master.m3u8")
        .arg(m_playbackServer.serverPort())
        .arg(session.id);
}

QString TimeshiftController::delayedPlaybackUrl(
    const Session &session,
    const qint64 anchorEpochMs,
    const qint64 targetEpochMs) const
{
    auto url = localPlaybackUrl(session);
    if (url.trimmed().isEmpty()) {
        return url;
    }
    return urlWithPlaybackTimingQuery(url, anchorEpochMs, targetEpochMs);
}

QByteArray TimeshiftController::buildDelayedMediaPlaylist(
    const Session &session,
    const QString &playlistPath,
    const qint64 anchorEpochMs,
    const qint64 targetEpochMs) const
{
    const auto snapshot = parsePlaylistSnapshot(playlistPath);
    if (!snapshot.valid || snapshot.segments.isEmpty()) {
        return {};
    }

    auto retainedStart = sessionWindowStartEpochMs(session);
    auto liveEpoch = sessionLiveEdgeEpochMs(session);
    if (retainedStart <= 0 && snapshot.windowStartUtc.isValid()) {
        retainedStart = snapshot.windowStartUtc.toMSecsSinceEpoch();
    }
    if (liveEpoch <= 0 && snapshot.liveEdgeUtc.isValid()) {
        liveEpoch = snapshot.liveEdgeUtc.toMSecsSinceEpoch();
    }
    if (retainedStart <= 0 || liveEpoch <= retainedStart) {
        return {};
    }

    const auto clampedAnchorEpoch = anchorEpochMs > 0 ? std::clamp(anchorEpochMs, retainedStart, liveEpoch) : retainedStart;
    const auto clampedTargetEpoch =
        targetEpochMs > 0 ? std::clamp(targetEpochMs, clampedAnchorEpoch, liveEpoch) : clampedAnchorEpoch;
    auto anchorIndex = anchorSegmentIndexForProgramDateTime(snapshot, clampedAnchorEpoch);
    if (anchorIndex < 0) {
        const auto targetOffsetSeconds =
            retainedStart > 0
            ? std::clamp(millisecondsToSeconds(clampedAnchorEpoch - retainedStart), 0.0, snapshot.availableSeconds)
            : 0.0;
        anchorIndex = anchorSegmentIndexForOffsetSeconds(snapshot, targetOffsetSeconds);
    }
    const auto anchorStartEpoch =
        snapshot.segments.at(anchorIndex).programDateTimeUtc.isValid()
        ? snapshot.segments.at(anchorIndex).programDateTimeUtc.toMSecsSinceEpoch()
        : retainedStart + static_cast<qint64>(std::llround(cumulativeSecondsBeforeSegment(snapshot, anchorIndex) * 1000.0));
    const auto startOffsetSeconds =
        std::max(0.0, millisecondsToSeconds(clampedTargetEpoch - anchorStartEpoch));

    QStringList outputLines;
    outputLines.reserve((snapshot.segments.size() - anchorIndex) * 3 + 5);
    outputLines << QStringLiteral("#EXTM3U");
    outputLines << QStringLiteral("#EXT-X-VERSION:3");
    outputLines << QStringLiteral("#EXT-X-PLAYLIST-TYPE:EVENT");
    outputLines << QStringLiteral("#EXT-X-TARGETDURATION:%1").arg(std::max(1, snapshot.targetDurationSeconds));
    outputLines << QStringLiteral("#EXT-X-MEDIA-SEQUENCE:%1").arg(snapshot.mediaSequence + anchorIndex);
    if (anchorEpochMs > 0 && targetEpochMs > 0) {
        outputLines << QStringLiteral("#EXT-X-START:TIME-OFFSET=%1,PRECISE=YES").arg(startOffsetSeconds, 0, 'f', 3);
    }

    for (int index = anchorIndex; index < snapshot.segments.size(); ++index) {
        const auto &segment = snapshot.segments.at(index);
        if (segment.programDateTimeUtc.isValid()) {
            outputLines << QStringLiteral("#EXT-X-PROGRAM-DATE-TIME:%1")
                               .arg(segment.programDateTimeUtc.toUTC().toString(Qt::ISODateWithMs));
        }
        outputLines << QStringLiteral("#EXTINF:%1,").arg(segment.durationSeconds, 0, 'f', 3);
        outputLines << segment.relativePath;
    }

    return outputLines.join(u'\n').toUtf8();
}

QByteArray TimeshiftController::buildPlaybackMasterPlaylist(
    const Session &session,
    const qint64 anchorEpochMs,
    const qint64 targetEpochMs) const
{
    if (session.avMasterPlaylistPath.isEmpty()) {
        if (anchorEpochMs <= 0 && targetEpochMs <= 0) {
            QFile file(session.playlistPath);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                return {};
            }
            return file.readAll();
        }
        return buildDelayedMediaPlaylist(session, session.playlistPath, anchorEpochMs, targetEpochMs);
    }

    QFile file(session.avMasterPlaylistPath.isEmpty() ? session.playlistPath : session.avMasterPlaylistPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    const auto sourceLines = QString::fromUtf8(file.readAll()).split(u'\n');
    QStringList outputLines;
    outputLines.reserve(sourceLines.size() + session.subtitleRenditions.size() + 4);

    bool subtitleMediaInserted = false;
    for (const auto &rawLine : sourceLines) {
        auto line = rawLine;
        if (!subtitleMediaInserted
            && line.startsWith(QStringLiteral("#EXT-X-STREAM-INF:"))
            && !session.subtitleRenditions.isEmpty()) {
            for (int index = 0; index < session.subtitleRenditions.size(); ++index) {
                const auto &subtitle = session.subtitleRenditions.at(index);
                auto mediaLine =
                    QStringLiteral("#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"ts_subs\",NAME=\"%1\",DEFAULT=%2")
                        .arg(attributeSafeName(subtitle.name, QStringLiteral("Subtitle %1").arg(index + 1)))
                        .arg(subtitle.isDefault ? QStringLiteral("YES") : QStringLiteral("NO"));
                if (!subtitle.language.trimmed().isEmpty()) {
                    mediaLine += QStringLiteral(",LANGUAGE=\"%1\"").arg(attributeSafeName(subtitle.language, subtitle.language));
                }
                mediaLine += QStringLiteral(",URI=\"%1\"")
                                 .arg(urlWithPlaybackTimingQuery(subtitle.playlistFileName, anchorEpochMs, targetEpochMs));
                outputLines.push_back(mediaLine);
            }
            subtitleMediaInserted = true;
        }

        if (!session.subtitleRenditions.isEmpty()
            && line.startsWith(QStringLiteral("#EXT-X-STREAM-INF:"))
            && !line.contains(QStringLiteral("SUBTITLES="))) {
            line += QStringLiteral(",SUBTITLES=\"ts_subs\"");
        }
        if (line.startsWith(QStringLiteral("#EXT-X-MEDIA:")) && line.contains(QStringLiteral("URI=\""))) {
            static const QRegularExpression kUriPattern(QStringLiteral("URI=\"([^\"]+)\""));
            const auto match = kUriPattern.match(line);
            if (match.hasMatch()) {
                line.replace(
                    match.capturedStart(1),
                    match.capturedLength(1),
                    urlWithPlaybackTimingQuery(match.captured(1), anchorEpochMs, targetEpochMs));
            }
        }
        if (!line.startsWith(QStringLiteral("#EXT-X-STREAM-INF:"))
            && !line.startsWith(u'#')
            && !line.trimmed().isEmpty()) {
            line = urlWithPlaybackTimingQuery(line.trimmed(), anchorEpochMs, targetEpochMs);
        }
        outputLines.push_back(line);
    }

    return outputLines.join(u'\n').toUtf8();
}

void TimeshiftController::handlePlaybackServerConnection()
{
    while (m_playbackServer.hasPendingConnections()) {
        auto *socket = m_playbackServer.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            handlePlaybackServerRequest(socket);
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void TimeshiftController::handlePlaybackServerRequest(QTcpSocket *socket)
{
    if (socket == nullptr || socket->property("timeshiftRequestHandled").toBool()) {
        return;
    }

    auto requestData = socket->property("timeshiftRequestBuffer").toByteArray();
    requestData += socket->readAll();
    if (!requestData.contains("\r\n\r\n") && !requestData.contains("\n\n")) {
        socket->setProperty("timeshiftRequestBuffer", requestData);
        return;
    }
    socket->setProperty("timeshiftRequestHandled", true);
    socket->setProperty("timeshiftRequestBuffer", QVariant());

    const auto requestLine = requestData.left(requestData.indexOf('\n')).trimmed();
    const auto parts = requestLine.split(' ');
    if (parts.size() < 2) {
        socket->write(buildHttpResponse(400, QByteArrayLiteral("text/plain"), QByteArrayLiteral("bad-request")));
        emit uiTestLocalRequestObserved(QStringLiteral("UNKNOWN"), QStringLiteral("<bad-request>"), 400, 0);
        socket->disconnectFromHost();
        return;
    }

    const auto &method = parts.at(0);
    const auto methodString = QString::fromLatin1(method);
    const auto headOnly = method == QByteArrayLiteral("HEAD");
    if (method != QByteArrayLiteral("GET") && !headOnly) {
        socket->write(buildHttpResponse(405, QByteArrayLiteral("text/plain"), QByteArrayLiteral("method-not-allowed")));
        emit uiTestLocalRequestObserved(methodString, QStringLiteral("<invalid-method>"), 405, 0);
        socket->disconnectFromHost();
        return;
    }

    const auto rawTarget = QString::fromLatin1(parts.at(1));
    const auto parsedTarget = QUrl(QStringLiteral("http://127.0.0.1%1").arg(rawTarget));
    auto query = QUrlQuery(parsedTarget);
    const auto decodedPath = QUrl::fromPercentEncoding(parsedTarget.path().toUtf8());
    const auto pathParts = decodedPath.split(u'/', Qt::SkipEmptyParts);
    if (pathParts.size() < 2) {
        socket->write(buildHttpResponse(404, QByteArrayLiteral("text/plain"), QByteArrayLiteral("not-found"), {}, headOnly));
        emit uiTestLocalRequestObserved(methodString, rawTarget, 404, 0);
        socket->disconnectFromHost();
        return;
    }

    const auto &requestedSessionId = pathParts.first();
    const auto *requestedSession = findAnySessionById(requestedSessionId);
    if (requestedSession == nullptr) {
        socket->write(buildHttpResponse(
            503,
            QByteArrayLiteral("text/plain"),
            QByteArrayLiteral("session-unavailable"),
            {},
            headOnly));
        emit uiTestLocalRequestObserved(methodString, rawTarget, 503, 0);
        socket->disconnectFromHost();
        return;
    }

    const auto relativePath = sanitizeRelativeRequestPath(pathParts.mid(1).join(u'/'));
    if (relativePath.isEmpty()) {
        socket->write(buildHttpResponse(404, QByteArrayLiteral("text/plain"), QByteArrayLiteral("not-found"), {}, headOnly));
        emit uiTestLocalRequestObserved(methodString, rawTarget, 404, 0);
        socket->disconnectFromHost();
        return;
    }

    QByteArray body;
    QList<QByteArray> extraHeaders { QByteArrayLiteral("Accept-Ranges: bytes") };
    auto respondAndClose = [this, socket, &methodString, &rawTarget, headOnly](
                               const int statusCode,
                               const QByteArray &contentType,
                               const QByteArray &responseBody,
                               const QList<QByteArray> &headers = {}) {
        socket->write(buildHttpResponse(statusCode, contentType, responseBody, headers, headOnly));
        emit uiTestLocalRequestObserved(
            methodString,
            rawTarget,
            statusCode,
            headOnly ? 0 : responseBody.size());
        socket->disconnectFromHost();
    };

    if (query.hasQueryItem(QStringLiteral("utc")) || query.hasQueryItem(QStringLiteral("anchor_ms"))) {
        respondAndClose(400, QByteArrayLiteral("text/plain"), QByteArrayLiteral("legacy-anchor-unsupported"));
        return;
    }

    auto anchorEpochMs = 0ll;
    if (query.hasQueryItem(QStringLiteral("pdt"))) {
        const auto pdtValue = query.queryItemValue(QStringLiteral("pdt")).trimmed();
        const auto parsedPdt = parseIsoDateTimeUtc(pdtValue);
        if (!parsedPdt.isValid()) {
            respondAndClose(400, QByteArrayLiteral("text/plain"), QByteArrayLiteral("invalid-pdt"));
            return;
        }
        anchorEpochMs = parsedPdt.toMSecsSinceEpoch();
    }
    auto targetEpochMs = 0ll;
    if (query.hasQueryItem(QStringLiteral("target_pdt"))) {
        const auto targetPdtValue = query.queryItemValue(QStringLiteral("target_pdt")).trimmed();
        const auto parsedTargetPdt = parseIsoDateTimeUtc(targetPdtValue);
        if (!parsedTargetPdt.isValid()) {
            respondAndClose(400, QByteArrayLiteral("text/plain"), QByteArrayLiteral("invalid-target-pdt"));
            return;
        }
        targetEpochMs = parsedTargetPdt.toMSecsSinceEpoch();
    }

    if (relativePath.compare(QStringLiteral("master.m3u8"), Qt::CaseInsensitive) == 0) {
        body = buildPlaybackMasterPlaylist(*requestedSession, anchorEpochMs, targetEpochMs);
        if (body.isEmpty()) {
            respondAndClose(404, QByteArrayLiteral("text/plain"), QByteArrayLiteral("not-found"));
            return;
        }
    } else if (relativePath.endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive)
        && requestedSession->avMasterPlaylistPath.trimmed().size() > 0
        && relativePath.compare(QStringLiteral("master.m3u8"), Qt::CaseInsensitive) != 0) {
        const auto requestedPlaylistPath = QDir(requestedSession->sessionDirectory).filePath(relativePath);
        if (anchorEpochMs > 0) {
            body = buildDelayedMediaPlaylist(*requestedSession, requestedPlaylistPath, anchorEpochMs, targetEpochMs);
        } else {
            QFile file(requestedPlaylistPath);
            if (!file.open(QIODevice::ReadOnly)) {
                respondAndClose(404, QByteArrayLiteral("text/plain"), QByteArrayLiteral("not-found"));
                return;
            }
            body = file.readAll();
        }
    } else {
        QFile file(QDir(requestedSession->sessionDirectory).filePath(relativePath));
        if (!file.open(QIODevice::ReadOnly)) {
            respondAndClose(404, QByteArrayLiteral("text/plain"), QByteArrayLiteral("not-found"));
            return;
        }
        body = file.readAll();
    }
    if (body.isEmpty()) {
        respondAndClose(404, QByteArrayLiteral("text/plain"), QByteArrayLiteral("not-found"));
        return;
    }

    const auto rangeHeader = requestHeaderValue(requestData, QByteArrayLiteral("Range:"));
    const auto range = parseByteRangeHeader(rangeHeader, body.size());
    if (range.requested && !range.satisfiable) {
        extraHeaders.push_back(QByteArrayLiteral("Content-Range: bytes */") + QByteArray::number(body.size()));
        respondAndClose(416, httpContentType(relativePath), {}, extraHeaders);
        return;
    }

    auto responseBody = body;
    auto statusCode = 200;
    if (range.requested) {
        const auto length = range.end - range.start + 1;
        responseBody = body.mid(range.start, length);
        extraHeaders.push_back(
            QByteArrayLiteral("Content-Range: bytes ")
            + QByteArray::number(range.start)
            + QByteArrayLiteral("-")
            + QByteArray::number(range.end)
            + QByteArrayLiteral("/")
            + QByteArray::number(body.size()));
        statusCode = 206;
    }

    respondAndClose(statusCode, httpContentType(relativePath), responseBody, extraHeaders);
}

bool TimeshiftController::channelEligibleForTimeshift(const std::optional<Channel> &channel) const
{
    return channel.has_value() && !m_multiViewController->isActive();
}

bool TimeshiftController::startSessionForCurrentChannel(const bool pauseWhenReady, const QString &reason)
{
    const auto currentChannel = m_playerController->currentChannelValue();
    if (!enabled() || !currentChannel.has_value() || !channelEligibleForTimeshift(currentChannel)) {
        return false;
    }
    const auto &channel = *currentChannel;

    const auto inputUrl = selectInputUrlForChannel(channel);
    if (inputUrl.trimmed().isEmpty()) {
        emit statusMessageRequested(QStringLiteral("Live timeshift could not determine the input stream."));
        return false;
    }

    if (m_session.has_value()
        && m_session->channel.profileId == channel.profileId
        && m_session->channel.id == channel.id
        && m_session->inputUrl.trimmed() == inputUrl.trimmed()) {
        m_session->pauseWhenReady = m_session->pauseWhenReady || pauseWhenReady;
        attachSessionPlaybackIfReady();
        if (!hasRunningIngestSession()) {
            scheduleReconnectGeneration(QStringLiteral("session-reuse-no-ingest"));
        }
        emit stateChanged();
        return true;
    }

    stopSession(false, QStringLiteral("session-replace"));

    if (!ensurePlaybackServer()) {
        emit statusMessageRequested(QStringLiteral("Live timeshift playback server could not start."));
        return false;
    }

    QString admissionFailure;
    if (!ensureDiskAdmission(inputUrl, &admissionFailure)) {
        emit statusMessageRequested(admissionFailure);
        return false;
    }

    auto session = Session {};
    session.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    session.state = SessionState::Starting;
    session.startReason = reason;
    session.channel = channel;
    session.inputUrl = inputUrl;
    session.fallbackPlaybackUrl = inputUrl;
    emit uiTestPlaybackUrlObserved(
        QStringLiteral("timeshift.fallback"),
        redactSensitiveText(session.fallbackPlaybackUrl));
    session.pauseWhenReady = pauseWhenReady;
    session.sessionDirectory = QDir(rootDirectory()).filePath(sessionFolderName(channel));
    QDir().mkpath(session.sessionDirectory);
    ensureSessionMarkerFile(session.sessionDirectory);
    m_session = std::move(session);

    const auto ffmpeg = findFfmpegBinary();
    const auto ffprobe = findFfprobeBinary();
    const auto sessionId = m_session->id;
    const auto finalizeProbe = [this, sessionId, ffmpeg](const StreamLayout &streamLayout) {
        if (!m_session.has_value() || m_session->id != sessionId || m_session->stopRequested) {
            return;
        }
        if (m_session->probeCompletionHandled) {
            return;
        }

        auto &sessionRef = *m_session;
        sessionRef.probeCompletionHandled = true;
        sessionRef.probeProcess.reset();

        const auto segmentSeconds = std::clamp(m_settings->current().timeshiftSegmentSeconds, 2, 60);
        const auto listSize = std::max(8, static_cast<int>(std::ceil(windowSeconds() / static_cast<double>(segmentSeconds))));
        auto defaultAudioIndex = 0;
        for (int audioIndex = 0; audioIndex < streamLayout.audioStreams.size(); ++audioIndex) {
            if (streamLayout.audioStreams.at(audioIndex).isDefault) {
                defaultAudioIndex = audioIndex;
                break;
            }
        }
        auto defaultSubtitleIndex = -1;
        for (int subtitleIndex = 0; subtitleIndex < streamLayout.supportedSubtitleStreams.size(); ++subtitleIndex) {
            if (streamLayout.supportedSubtitleStreams.at(subtitleIndex).isDefault) {
                defaultSubtitleIndex = subtitleIndex;
                break;
            }
        }

        sessionRef.avMasterPlaylistPath = QDir(sessionRef.sessionDirectory).filePath(QStringLiteral("av_master.m3u8"));
        sessionRef.playlistPath = QDir(sessionRef.sessionDirectory).filePath(QStringLiteral("stream_0.m3u8"));
        sessionRef.audioTrackCount = static_cast<int>(streamLayout.audioStreams.size());
        sessionRef.subtitleTrackCount = static_cast<int>(streamLayout.supportedSubtitleStreams.size());
        sessionRef.droppedSubtitleCodecs.clear();
        for (const auto &subtitle : streamLayout.droppedSubtitleStreams) {
            const auto codecName = subtitle.codecName.trimmed().isEmpty() ? QStringLiteral("unknown") : subtitle.codecName.trimmed();
            if (!sessionRef.droppedSubtitleCodecs.contains(codecName)) {
                sessionRef.droppedSubtitleCodecs.push_back(codecName);
            }
        }

        QStringList args {
            QStringLiteral("-y"),
            QStringLiteral("-hide_banner"),
            QStringLiteral("-loglevel"), QStringLiteral("warning"),
            QStringLiteral("-nostdin"),
            QStringLiteral("-i"), sessionRef.inputUrl
        };

        const auto streamLayoutUsable = streamLayout.valid && streamLayout.primaryVideo.has_value();
        if (streamLayoutUsable) {
            args << QStringLiteral("-map")
                 << QStringLiteral("0:%1").arg(streamLayout.primaryVideo->inputStreamIndex);
            for (const auto &audioStream : streamLayout.audioStreams) {
                args << QStringLiteral("-map")
                     << QStringLiteral("0:%1").arg(audioStream.inputStreamIndex);
            }
            args << QStringLiteral("-c") << QStringLiteral("copy");
            for (int audioIndex = 0; audioIndex < streamLayout.audioStreams.size(); ++audioIndex) {
                const auto &audioStream = streamLayout.audioStreams.at(audioIndex);
                if (!audioStream.language.trimmed().isEmpty()) {
                    args << QStringLiteral("-metadata:s:a:%1").arg(audioIndex)
                         << QStringLiteral("language=%1").arg(audioStream.language.trimmed());
                }
                if (!audioStream.title.trimmed().isEmpty()) {
                    args << QStringLiteral("-metadata:s:a:%1").arg(audioIndex)
                         << QStringLiteral("title=%1").arg(audioStream.title.trimmed());
                }
            }
            args << QStringLiteral("-f") << QStringLiteral("hls")
                 << QStringLiteral("-hls_time") << QString::number(segmentSeconds)
                 << QStringLiteral("-hls_list_size") << QString::number(listSize)
                 << QStringLiteral("-hls_flags") << timeshiftHlsFlags()
                 << QStringLiteral("-master_pl_name") << QFileInfo(sessionRef.avMasterPlaylistPath).fileName();

            QStringList variantEntries;
            QString primaryVariantEntry = QStringLiteral("v:0");
            if (!streamLayout.audioStreams.isEmpty()) {
                primaryVariantEntry += QStringLiteral(",agroup:aud");
                for (int audioIndex = 0; audioIndex < streamLayout.audioStreams.size(); ++audioIndex) {
                    const auto &audioStream = streamLayout.audioStreams.at(audioIndex);
                    auto audioEntry = QStringLiteral("a:%1,agroup:aud").arg(audioIndex);
                    if (audioIndex == defaultAudioIndex) {
                        audioEntry += QStringLiteral(",default:yes");
                    }
                    if (!audioStream.language.trimmed().isEmpty()) {
                        audioEntry += QStringLiteral(",language:%1").arg(audioStream.language.trimmed());
                    }
                    variantEntries.push_back(audioEntry);
                }
            }
            variantEntries.push_front(primaryVariantEntry);
            args << QStringLiteral("-var_stream_map") << variantEntries.join(u' ')
                 << QDir(sessionRef.sessionDirectory).filePath(QStringLiteral("stream_%v.m3u8"));

            sessionRef.subtitleRenditions.clear();
            for (int subtitleIndex = 0; subtitleIndex < streamLayout.supportedSubtitleStreams.size(); ++subtitleIndex) {
                const auto &subtitleStream = streamLayout.supportedSubtitleStreams.at(subtitleIndex);
                SubtitleRendition rendition;
                rendition.playlistFileName = QStringLiteral("subtitle_%1.m3u8").arg(subtitleIndex);
                rendition.playlistPath = QDir(sessionRef.sessionDirectory).filePath(rendition.playlistFileName);
                rendition.name = subtitleDisplayName(subtitleStream, subtitleIndex);
                rendition.language = subtitleStream.language.trimmed();
                rendition.codecName = subtitleStream.codecName.trimmed();
                rendition.isDefault = (defaultSubtitleIndex >= 0 && subtitleIndex == defaultSubtitleIndex);
                sessionRef.subtitleRenditions.push_back(rendition);

                args << QStringLiteral("-map")
                     << QStringLiteral("0:%1").arg(subtitleStream.inputStreamIndex)
                     << QStringLiteral("-c:s") << QStringLiteral("webvtt")
                     << QStringLiteral("-f") << QStringLiteral("segment")
                     << QStringLiteral("-segment_time") << QString::number(segmentSeconds)
                     << QStringLiteral("-segment_list") << rendition.playlistPath
                     << QStringLiteral("-segment_list_type") << QStringLiteral("m3u8")
                     << QStringLiteral("-segment_list_flags") << QStringLiteral("+live")
                     << QStringLiteral("-segment_list_size") << QString::number(listSize)
                     << QDir(sessionRef.sessionDirectory).filePath(QStringLiteral("subtitle_%1_%06d.vtt").arg(subtitleIndex));
            }
        } else {
            sessionRef.avMasterPlaylistPath.clear();
            sessionRef.subtitleRenditions.clear();
            sessionRef.audioTrackCount = 0;
            sessionRef.subtitleTrackCount = 0;
            sessionRef.playlistPath = QDir(sessionRef.sessionDirectory).filePath(QStringLiteral("index.m3u8"));
            args << QStringLiteral("-map") << QStringLiteral("0:v?")
                 << QStringLiteral("-map") << QStringLiteral("0:a?")
                 << QStringLiteral("-sn")
                 << QStringLiteral("-c") << QStringLiteral("copy")
                 << QStringLiteral("-f") << QStringLiteral("hls")
                 << QStringLiteral("-hls_time") << QString::number(segmentSeconds)
                 << QStringLiteral("-hls_list_size") << QString::number(listSize)
                 << QStringLiteral("-hls_flags") << timeshiftHlsFlags()
                 << sessionRef.playlistPath;

            Core::DebugLogger::instance().log(
                QStringLiteral("timeshift.session.error"),
                QStringLiteral("Timeshift stream probe failed; falling back to AV-only HLS (%1).")
                    .arg(streamLayout.error.isEmpty() ? QStringLiteral("unknown") : streamLayout.error));
        }

        sessionRef.ingestProcess = std::make_unique<QProcess>(this);
        sessionRef.ingestProcess->setProcessChannelMode(QProcess::MergedChannels);

        auto *process = sessionRef.ingestProcess.get();
        connect(process, &QProcess::readyRead, this, [this, sessionId]() {
            auto *sessionRef = findAnySessionById(sessionId);
            if (sessionRef == nullptr || !sessionRef->ingestProcess) {
                return;
            }
            auto output = QString::fromLocal8Bit(sessionRef->ingestProcess->readAll()).trimmed();
            if (output.isEmpty()) {
                return;
            }
            output.replace(u'\r', u' ');
            output.replace(u'\n', QStringLiteral(" | "));
            Core::DebugLogger::instance().log(
                QStringLiteral("timeshift.session.start"),
                QStringLiteral("ffmpeg output: %1").arg(output.left(600)));
        });
        connect(process, &QProcess::errorOccurred, this, [this, sessionId](const QProcess::ProcessError error) {
            auto *sessionRef = findAnySessionById(sessionId);
            if (sessionRef == nullptr || sessionRef->stopRequested) {
                return;
            }
            Core::DebugLogger::instance().log(
                QStringLiteral("timeshift.session.error"),
                QStringLiteral("ffmpeg process error (%1) in state=%2.")
                    .arg(static_cast<int>(error))
                    .arg(stateName(sessionRef->state)));
            handleRuntimeIngestFailure(
                *sessionRef,
                QStringLiteral("ffmpeg-error"),
                !sessionRef->playbackAttached,
                QStringLiteral("Live timeshift could not start. Falling back to direct playback."));
        });
        connect(process, &QProcess::finished, this, [this, sessionId](const int exitCode, const QProcess::ExitStatus exitStatus) {
            auto *sessionRef = findAnySessionById(sessionId);
            if (sessionRef == nullptr || sessionRef->stopRequested) {
                return;
            }
            Core::DebugLogger::instance().log(
                QStringLiteral("timeshift.session.error"),
                QStringLiteral("ffmpeg exited unexpectedly (exit=%1 status=%2).")
                    .arg(exitCode)
                    .arg(exitStatus == QProcess::NormalExit ? QStringLiteral("normal") : QStringLiteral("crash")));
            handleRuntimeIngestFailure(
                *sessionRef,
                QStringLiteral("ffmpeg-finished"),
                !sessionRef->playbackAttached,
                QStringLiteral("Live timeshift could not start. Falling back to direct playback."));
        });

        Core::DebugLogger::instance().log(
            QStringLiteral("timeshift.session.start"),
            QStringLiteral("Starting timeshift for %1 from %2 (%3). tracks: audio=%4 subtitles=%5 dropped=%6.")
                .arg(sessionRef.channel.name)
                .arg(sessionRef.inputUrl)
                .arg(sessionRef.startReason)
                .arg(sessionRef.audioTrackCount)
                .arg(sessionRef.subtitleTrackCount)
                .arg(sessionRef.droppedSubtitleCodecs.isEmpty()
                    ? QStringLiteral("none")
                    : sessionRef.droppedSubtitleCodecs.join(QStringLiteral(","))));
        process->start(ffmpeg, args);
        m_readyPollTimer.start();
        m_playlistPollTimer.start();
        emit stateChanged();
    };

    m_session->probeProcess = std::make_unique<QProcess>(this);
    m_session->probeProcess->setProcessChannelMode(QProcess::MergedChannels);
    auto *probeProcess = m_session->probeProcess.get();
    connect(probeProcess, &QProcess::errorOccurred, this, [this, sessionId, finalizeProbe](const QProcess::ProcessError error) {
        if (!m_session.has_value() || m_session->id != sessionId || m_session->stopRequested || m_session->probeCompletionHandled) {
            return;
        }
        if (error != QProcess::FailedToStart) {
            return;
        }
        StreamLayout streamLayout;
        streamLayout.error = QStringLiteral("ffprobe failed to start");
        finalizeProbe(streamLayout);
    });
    connect(probeProcess, &QProcess::finished, this, [this, sessionId, finalizeProbe](const int exitCode, const QProcess::ExitStatus exitStatus) {
        if (!m_session.has_value() || m_session->id != sessionId || m_session->stopRequested || m_session->probeCompletionHandled) {
            return;
        }
        const auto payload = QString::fromUtf8(m_session->probeProcess->readAll()).trimmed();
        auto streamLayout = probeStreamLayout(payload);
        if (m_session->probeTimedOut) {
            streamLayout.error = QStringLiteral("ffprobe timed out");
        } else if ((exitStatus != QProcess::NormalExit || exitCode != 0) && streamLayout.error.isEmpty()) {
            streamLayout.error = QStringLiteral("ffprobe exited before returning stream metadata");
        }
        finalizeProbe(streamLayout);
    });
    QTimer::singleShot(kFfprobeTimeoutMs, this, [this, sessionId]() {
        if (!m_session.has_value() || m_session->id != sessionId || !m_session->probeProcess
            || m_session->probeCompletionHandled || m_session->stopRequested) {
            return;
        }
        if (m_session->probeProcess->state() == QProcess::NotRunning) {
            return;
        }
        m_session->probeTimedOut = true;
        Core::DebugLogger::instance().log(
            QStringLiteral("timeshift.session.error"),
            QStringLiteral("ffprobe timed out for session %1; falling back to AV-only startup.").arg(sessionId));
        m_session->probeProcess->kill();
    });

    QStringList probeArgs {
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-show_streams"),
        QStringLiteral("-show_entries"),
            QStringLiteral("stream=index,codec_type,codec_name:stream_tags=language,title:stream_disposition=default"),
        QStringLiteral("-of"), QStringLiteral("json"),
        m_session->inputUrl
    };
    Core::DebugLogger::instance().log(
        QStringLiteral("timeshift.session.start"),
        QStringLiteral("Starting async ffprobe preflight for %1 (%2).")
            .arg(m_session->channel.name)
            .arg(m_session->startReason));
    probeProcess->start(ffprobe, probeArgs);
    emit stateChanged();
    return true;
}

void TimeshiftController::attachSessionPlaybackIfReady()
{
    if (!m_session.has_value()) {
        return;
    }

    refreshSessionPlaylistInfo(*m_session);
    refreshRetainedSessionPlaylists();
    cleanupUnreachableRetainedSessions();

    if (m_session->playlistInfo.valid && m_session->state == SessionState::Starting) {
        m_session->state = SessionState::Running;
    }

    if (m_session->playbackAttached) {
        const auto sessionStartEpochMs = sessionWindowStartEpochMs(*m_session);
        if (sessionStartEpochMs > 0 && m_session->attachedWindowStartEpochMs < sessionStartEpochMs) {
            m_session->attachedWindowStartEpochMs = sessionStartEpochMs;
            m_session->delayedStartEpochMs = sessionStartEpochMs;
            setNoticeText(QStringLiteral("The oldest retained part of the buffer was pruned. Playback moved to the oldest available point."));
        }
        m_session->attachedWindowEndEpochMs =
            std::max(m_session->attachedWindowStartEpochMs, sessionLiveEdgeEpochMs(*m_session));
    }

    if (m_session->state == SessionState::Failed && !m_session->playbackLoadPending) {
        const auto currentEpochMs = currentPlaybackEpochMs();
        const auto currentLiveEpochMs = sessionLiveEdgeEpochMs(*m_session);
        const auto nearDeplete = currentEpochMs > 0
            && currentLiveEpochMs > 0
            && currentEpochMs >= currentLiveEpochMs - 500;
        if (nearDeplete
            && switchToNextGenerationPlayback(QStringLiteral("failed-generation-deplete"), false)) {
            emit stateChanged();
            return;
        }
    }

    const auto startupOffsetSeconds = normalizedPlayerBufferSeconds(m_settings->current().playerBufferSeconds);
    const auto startupAttachThresholdSeconds =
        std::max(0.0, startupOffsetSeconds * kStartupAttachWarmupMultiplier);
    if (!m_session->playlistInfo.valid || m_session->playlistInfo.availableSeconds < startupAttachThresholdSeconds) {
        emit stateChanged();
        return;
    }

    if (!m_session->playbackAttached && !m_session->playbackLoadPending && m_session->state != SessionState::Failed) {
        const auto sessionStartEpochMs = sessionWindowStartEpochMs(*m_session);
        const auto sessionLiveEpochMs = sessionLiveEdgeEpochMs(*m_session);
        const auto startupOffsetMs = static_cast<qint64>(std::llround(startupOffsetSeconds * 1000.0));
        const auto nowUtc = QDateTime::currentDateTimeUtc();
        const auto liveEdgeAgeMs = m_session->lastPlaylistAdvanceUtc.isValid()
            ? m_session->lastPlaylistAdvanceUtc.msecsTo(nowUtc)
            : std::numeric_limits<qint64>::max();
        const auto liveEdgeFresh =
            liveEdgeAgeMs >= 0
            && liveEdgeAgeMs <= kPlaylistStallTimeoutMs;
        auto initialTargetEpochMs = 0LL;
        auto startupTargetMode = QStringLiteral("live-minus-buffer");
        if (sessionStartEpochMs > 0 && sessionLiveEpochMs > sessionStartEpochMs && liveEdgeFresh) {
            initialTargetEpochMs = std::clamp(sessionLiveEpochMs - startupOffsetMs, sessionStartEpochMs, sessionLiveEpochMs);
        }
        if (initialTargetEpochMs <= 0) {
            startupTargetMode = QStringLiteral("head-plus-buffer-fallback");
            if (sessionStartEpochMs > 0 && sessionLiveEpochMs > sessionStartEpochMs) {
                initialTargetEpochMs = std::clamp(sessionStartEpochMs + startupOffsetMs, sessionStartEpochMs, sessionLiveEpochMs);
            }
        }
        if (initialTargetEpochMs <= 0) {
            initialTargetEpochMs = sessionLiveEpochMs > 0
                ? sessionLiveEpochMs
                : sessionStartEpochMs;
        }
        playSessionFromAnchor(initialTargetEpochMs, m_session->pauseWhenReady);
        Core::DebugLogger::instance().log(
            QStringLiteral("timeshift.session.ready"),
            QStringLiteral("Timeshift ready for %1 at %2 (playlist=%3, startup-offset=%4s, warmup-gate=%5s, target-mode=%6, live-age=%7).")
                .arg(m_session->channel.name, m_session->pendingPlaybackUrl, m_session->playlistPath)
                .arg(startupOffsetSeconds, 0, 'f', 1)
                .arg(startupAttachThresholdSeconds, 0, 'f', 1)
                .arg(startupTargetMode)
                .arg(liveEdgeFresh ? QString::number(liveEdgeAgeMs) : QStringLiteral("stale")));
    }

    emit stateChanged();
}

void TimeshiftController::stopSession(
    const bool restoreLivePlayback,
    const QString &reason,
    const bool markForSinglePlaybackRestart,
    const bool forceImmediateProcessKill)
{
    if (!m_session.has_value() && m_retainedSessions.empty()) {
        m_restartWhenSinglePlaybackReturns = m_restartWhenSinglePlaybackReturns || markForSinglePlaybackRestart;
        return;
    }

    m_readyPollTimer.stop();
    m_playlistPollTimer.stop();
    m_noticeClearTimer.stop();
    m_reconnectGenerationTimer.stop();
    m_reconnectGenerationAttempt = 0;
    m_noticeAutoClearSessionId.clear();
    m_noticeAutoClearText.clear();
    m_restartWhenSinglePlaybackReturns = markForSinglePlaybackRestart;

    if (m_session.has_value()) {
        auto session = std::move(m_session.value());
        m_session.reset();

        Core::DebugLogger::instance().log(
            QStringLiteral("timeshift.session.stop"),
            QStringLiteral("Stopping timeshift session %1 (%2).").arg(session.id, reason));

        if (restoreLivePlayback
            && session.playbackAttached
            && m_playerController->currentPlaybackUrl().trimmed() == session.playbackUrl.trimmed()) {
            emit uiTestPlaybackUrlObserved(
                QStringLiteral("timeshift.restore-live"),
                redactSensitiveText(session.fallbackPlaybackUrl));
            m_playerController->playCurrentPlaybackUrl(session.fallbackPlaybackUrl, false);
        }

        cleanupSessionFiles(session, forceImmediateProcessKill);
    }

    for (auto &retained : m_retainedSessions) {
        cleanupSessionFiles(retained, forceImmediateProcessKill);
    }
    m_retainedSessions.clear();

    emit stateChanged();
}

bool TimeshiftController::restoreLivePlaybackPath()
{
    if (!m_session.has_value()) {
        return false;
    }

    emit uiTestPlaybackUrlObserved(
        QStringLiteral("timeshift.restore-live"),
        redactSensitiveText(m_session->fallbackPlaybackUrl));
    m_playerController->playCurrentPlaybackUrl(m_session->fallbackPlaybackUrl, false);
    return true;
}

void TimeshiftController::handleCurrentChannelChanged()
{
    const auto currentChannel = m_playerController->currentChannelValue();
    if (!currentChannel.has_value()) {
        stopSession(false, QStringLiteral("no-current-channel"));
        m_restartWhenSinglePlaybackReturns = false;
        return;
    }

    if (!m_session.has_value()) {
        return;
    }

    if (m_session->channel.profileId != currentChannel->profileId || m_session->channel.id != currentChannel->id) {
        stopSession(false, QStringLiteral("channel-changed"));
    }
}

bool TimeshiftController::handlePrimaryPlaybackActivation()
{
    if (!enabled() || m_multiViewController->isActive()) {
        return false;
    }

    if (startSessionForCurrentChannel(false, QStringLiteral("playback-activated"))) {
        return true;
    }

    const auto currentChannel = m_playerController->currentChannelValue();
    if (!currentChannel.has_value()) {
        return true;
    }

    const auto fallbackUrl = selectInputUrlForChannel(currentChannel.value());
    if (fallbackUrl.trimmed().isEmpty()) {
        return true;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("timeshift.session.error"),
        QStringLiteral("Timeshift preflight failed for %1; falling back to direct playback.")
            .arg(currentChannel->name));
    emit statusMessageRequested(QStringLiteral("Live timeshift could not start. Falling back to direct playback."));
    emit uiTestPlaybackUrlObserved(
        QStringLiteral("timeshift.fallback-direct"),
        redactSensitiveText(fallbackUrl));
    m_playerController->playCurrentPlaybackUrl(fallbackUrl, false);
    return true;
}

void TimeshiftController::handleMultiviewLayoutChanged()
{
    const auto nowActive = m_multiViewController->isActive();
    if (nowActive && !m_lastKnownMultiviewActive) {
        stopSession(true, QStringLiteral("multiview-enter"), enabled());
    } else if (!nowActive && m_lastKnownMultiviewActive && m_restartWhenSinglePlaybackReturns && enabled()) {
        m_restartWhenSinglePlaybackReturns = false;
        startSessionForCurrentChannel(false, QStringLiteral("multiview-exit"));
    }

    m_lastKnownMultiviewActive = nowActive;
}

void TimeshiftController::handlePlaylistPoll()
{
    if (!m_session.has_value()) {
        m_playlistPollTimer.stop();
        return;
    }

    attachSessionPlaybackIfReady();
    if (!m_session.has_value()) {
        return;
    }
    finalizePendingPlaybackLoadIfReady(QStringLiteral("playlist-poll"));
    applyPendingPostLoadSeekIfReady(QStringLiteral("playlist-poll"));

    if (m_session->state != SessionState::Failed
        && m_session->playlistInfo.valid
        && m_session->lastPlaylistAdvanceUtc.isValid()
        && m_session->lastPlaylistAdvanceUtc.msecsTo(QDateTime::currentDateTimeUtc()) >= kPlaylistStallTimeoutMs
        && isActive()
        && !m_playerController->player()->pauseState().value_or(false)) {
        handlePlaybackFailure(QStringLiteral("playlist-stall"));
    }
}

void TimeshiftController::cleanupStaleSessions() const
{
    QDir root(rootDirectory());
    if (!root.exists()) {
        return;
    }

    const auto entries = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Time);
    for (const auto &entry : entries) {
        if (!isManagedTimeshiftSessionDirectory(entry)) {
            continue;
        }
        QDir(entry.absoluteFilePath()).removeRecursively();
    }
}

bool TimeshiftController::ensureDiskAdmission(const QString &inputUrl, QString *failureMessage) const
{
    Q_UNUSED(inputUrl);

    const auto storage = QStorageInfo(rootDirectory());
    if (!storage.isValid() || !storage.isReady()) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("Live timeshift storage is unavailable.");
        }
        return false;
    }

    auto estimatedBitsPerSecond = kDefaultBitrateBitsPerSecond;
    if (const auto videoBits = m_playerController->player()->videoBitrateBitsPerSecond();
        videoBits.has_value() && std::isfinite(videoBits.value()) && videoBits.value() > 0.0) {
        estimatedBitsPerSecond = videoBits.value();
        if (const auto audioBits = m_playerController->player()->audioBitrateBitsPerSecond();
            audioBits.has_value() && std::isfinite(audioBits.value()) && audioBits.value() > 0.0) {
            estimatedBitsPerSecond += audioBits.value();
        }
    }

    const auto estimatedBytes = static_cast<qulonglong>(
        std::ceil((estimatedBitsPerSecond * static_cast<double>(windowSeconds()) / 8.0) * 1.25));
    const auto maxBytes = static_cast<qulonglong>(m_settings->current().timeshiftMaxDiskGb) * 1024ull * 1024ull * 1024ull;
    if (estimatedBytes > maxBytes) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("Live timeshift window exceeds the configured disk quota.");
        }
        return false;
    }
    if (storage.bytesAvailable() < estimatedBytes) {
        if (failureMessage) {
            *failureMessage = QStringLiteral("Not enough free disk space to start live timeshift.");
        }
        return false;
    }
    return true;
}

QString TimeshiftController::selectInputUrlForChannel(const Channel &channel) const
{
    const auto tapUrl = m_dvrController->activeTapPlaybackUrlForChannel(channel);
    return tapUrl.trimmed().isEmpty() ? channel.streamUrl : tapUrl;
}

void TimeshiftController::updateDerivedState()
{
    emit stateChanged();
}

} // namespace OKILTV::App
