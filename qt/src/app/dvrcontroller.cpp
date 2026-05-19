#include "dvrcontroller.h"

#include "playercontroller.h"

#include "../core/appdatapaths.h"
#include "../core/debuglogger.h"
#include "../core/processutils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QUdpSocket>

#include <algorithm>
#include <cmath>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace OKILTV::App {

using namespace Core;

namespace {

constexpr int kDvrTickMs = 1000;
constexpr int kDvrRestartRetryMs = 1000;
constexpr int kTapStartupTimeoutMs = 8000;
constexpr int kProcessTerminateTimeoutMs = 1500;
constexpr int kProcessKillTimeoutMs = 1200;
constexpr int kProcessForceKillRetryMs = 500;
constexpr int kProcessForceKillRetries = 10;
constexpr int kWindowsOrphanSweepDelayMs = 500;
constexpr int kWindowsOrphanSweepRetries = 12;
constexpr int kTempDeleteRetryDelayMs = 1000;
constexpr int kTempDeleteMaxRetries = 90;
constexpr int kRemuxProbeStartTimeoutMs = 3000;
constexpr int kRemuxProbeTimeoutMs = 10000;
constexpr double kRemuxDurationMatchToleranceSeconds = 1.0;

QString cleanText(const QVariantMap &map, const QString &key)
{
    return map.value(key).toString().trimmed();
}

int cleanInt(const QVariantMap &map, const QString &key, const int fallback = -1)
{
    bool ok = false;
    const auto value = map.value(key).toInt(&ok);
    return ok ? value : fallback;
}

QString processErrorText(const QProcess::ProcessError error)
{
    switch (error) {
    case QProcess::FailedToStart:
        return QStringLiteral("failed-to-start");
    case QProcess::Crashed:
        return QStringLiteral("crashed");
    case QProcess::Timedout:
        return QStringLiteral("timedout");
    case QProcess::ReadError:
        return QStringLiteral("read-error");
    case QProcess::WriteError:
        return QStringLiteral("write-error");
    case QProcess::UnknownError:
    default:
        return QStringLiteral("unknown-error");
    }
}

bool isProcessAlive(const qint64 pid)
{
    if (pid <= 0) {
        return false;
    }
#if defined(Q_OS_WIN)
    const auto handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return false;
    }

    const auto waitResult = WaitForSingleObject(handle, 0);
    CloseHandle(handle);
    return waitResult == WAIT_TIMEOUT;
#else
    const auto result = ::kill(static_cast<pid_t>(pid), 0);
    return result == 0 || errno == EPERM;
#endif
}

std::optional<double> probeMediaDurationSeconds(const QString &path, QString *errorText = nullptr)
{
    const auto normalizedPath = path.trimmed();
    if (normalizedPath.isEmpty()) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("empty-path");
        }
        return std::nullopt;
    }

    if (!QFileInfo::exists(normalizedPath)) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("missing-file");
        }
        return std::nullopt;
    }

    const auto ffprobe = Core::resolveProcessBinary(QStringLiteral("ffprobe"));
    QProcess probe;
    probe.setProcessChannelMode(QProcess::MergedChannels);
    const QStringList args {
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-show_entries"), QStringLiteral("format=duration"),
        QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1:nokey=1"),
        normalizedPath
    };

    probe.start(ffprobe, args);
    if (!probe.waitForStarted(kRemuxProbeStartTimeoutMs)) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("failed-to-start");
        }
        return std::nullopt;
    }

    if (!probe.waitForFinished(kRemuxProbeTimeoutMs)) {
        probe.kill();
        probe.waitForFinished(kProcessKillTimeoutMs);
        if (errorText != nullptr) {
            *errorText = QStringLiteral("timeout");
        }
        return std::nullopt;
    }

    auto payload = QString::fromLocal8Bit(probe.readAll()).trimmed();
    if (payload.isEmpty()) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("empty-output");
        }
        return std::nullopt;
    }

    const auto lines = payload.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (const auto &line : lines) {
        bool ok = false;
        const auto duration = line.trimmed().toDouble(&ok);
        if (ok && std::isfinite(duration) && duration >= 0.0) {
            return duration;
        }
    }

    bool ok = false;
    const auto duration = payload.toDouble(&ok);
    if (ok && std::isfinite(duration) && duration >= 0.0) {
        return duration;
    }

    if (errorText != nullptr) {
        auto compact = payload;
        compact.replace(u'\r', u' ');
        compact.replace(u'\n', QStringLiteral(" | "));
        if (compact.size() > 160) {
            compact = compact.left(160) + QStringLiteral("...");
        }
        *errorText = QStringLiteral("invalid-output=%1").arg(compact);
    }
    return std::nullopt;
}

} // namespace

DvrController::DvrController(SettingsManager *settings, PlayerController *playerController, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_playerController(playerController)
{
    loadSchedulesFromSettings();
    m_tickTimer.setInterval(kDvrTickMs);
    connect(&m_tickTimer, &QTimer::timeout, this, &DvrController::tick);
    m_tickTimer.start();
    tick();
}

DvrController::~DvrController()
{
    shutdownForApplicationExit();
}

int DvrController::scheduledCount() const
{
    return static_cast<int>(m_schedules.size());
}

int DvrController::activeRecordingCount() const
{
    auto count = 0;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        if (it->second && it->second->recordingStarted) {
            ++count;
        }
    }
    return count;
}

bool DvrController::exitConfirmationRequired() const
{
    if (activeRecordingCount() > 0) {
        return true;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    const auto threshold = now.addSecs(static_cast<qint64>(15) * 60);
    for (const auto &schedule : m_schedules) {
        const auto [startAt, stopAt] = effectiveWindow(schedule);
        if (!startAt.isValid() || !stopAt.isValid() || stopAt <= startAt || stopAt <= now) {
            continue;
        }
        if (startAt <= threshold) {
            return true;
        }
    }

    return false;
}

bool DvrController::toggleProgramSchedule(const QVariantMap &channel, const QVariantMap &program)
{
    const auto profileId = cleanText(channel, QStringLiteral("profileId"));
    const auto channelId = cleanInt(channel, QStringLiteral("id"), -1);
    const auto streamUrl = cleanText(channel, QStringLiteral("streamUrl"));
    const auto start = parseIsoUtc(cleanText(program, QStringLiteral("start")));
    const auto stop = parseIsoUtc(cleanText(program, QStringLiteral("stop")));
    const auto title = cleanText(program, QStringLiteral("title"));

    if (profileId.isEmpty() || channelId < 0 || streamUrl.isEmpty() || !start.isValid() || !stop.isValid() || stop <= start) {
        return false;
    }

    const auto scheduleId = makeScheduleId(profileId, channelId, start, stop, title);
    const auto existing = std::find_if(m_schedules.begin(), m_schedules.end(), [&scheduleId](const DvrScheduleEntry &entry) {
        return entry.id == scheduleId;
    });

    if (existing != m_schedules.end()) {
        m_schedules.erase(existing);
        persistSchedules();
        tick();
        emit stateChanged();
        emitRecordingChannelsChanged();
        return true;
    }

    DvrScheduleEntry entry;
    entry.id = scheduleId;
    entry.profileId = profileId;
    entry.channelId = channelId;
    entry.channelName = cleanText(channel, QStringLiteral("name"));
    entry.streamUrl = streamUrl;
    entry.tvgId = cleanText(channel, QStringLiteral("tvgId"));
    entry.title = title;
    entry.subTitle = cleanText(program, QStringLiteral("subTitle"));
    entry.description = cleanText(program, QStringLiteral("description"));
    entry.start = start;
    entry.stop = stop;
    entry.createdAt = QDateTime::currentDateTimeUtc();
    m_schedules.push_back(entry);

    std::sort(m_schedules.begin(), m_schedules.end(), [](const DvrScheduleEntry &left, const DvrScheduleEntry &right) {
        if (left.start != right.start) {
            return left.start < right.start;
        }
        return left.id < right.id;
    });

    persistSchedules();
    tick();
    emit stateChanged();
    return true;
}

bool DvrController::isProgramScheduled(const QVariantMap &channel, const QVariantMap &program) const
{
    const auto profileId = cleanText(channel, QStringLiteral("profileId"));
    const auto channelId = cleanInt(channel, QStringLiteral("id"), -1);
    const auto start = cleanText(program, QStringLiteral("start"));
    const auto stop = cleanText(program, QStringLiteral("stop"));
    const auto title = cleanText(program, QStringLiteral("title"));
    return isProgramScheduledByIdentity(profileId, channelId, start, stop, title);
}

bool DvrController::isProgramScheduledByIdentity(
    const QString &profileId,
    const int channelId,
    const QString &startIso,
    const QString &stopIso, // NOLINT(bugprone-easily-swappable-parameters)
    const QString &title) const
{
    const auto start = parseIsoUtc(startIso);
    const auto stop = parseIsoUtc(stopIso);
    if (profileId.trimmed().isEmpty() || channelId < 0 || !start.isValid() || !stop.isValid()) {
        return false;
    }

    const auto scheduleId = makeScheduleId(profileId.trimmed(), channelId, start, stop, title.trimmed());
    return std::any_of(m_schedules.cbegin(), m_schedules.cend(), [&scheduleId](const DvrScheduleEntry &entry) {
        return entry.id == scheduleId;
    });
}

void DvrController::shutdownForApplicationExit()
{
    if (m_exitShutdownStarted) {
        return;
    }
    m_exitShutdownStarted = true;
    m_tickTimer.stop();

    for (auto &entry : m_sessions) {
        if (!entry.second || !entry.second->ingestProcess) {
            continue;
        }

        auto *process = entry.second->ingestProcess.get();
        process->disconnect(this);
        if (process->state() == QProcess::NotRunning) {
            continue;
        }

        process->terminate();
        if (!process->waitForFinished(kProcessTerminateTimeoutMs)) {
            process->kill();
            process->waitForFinished(kProcessKillTimeoutMs);
        }
    }
}

QString DvrController::activeTapPlaybackUrlForChannel(const Channel &channel) const
{
    const auto profileId = guidToString(channel.profileId);
    auto *session = sessionByChannel(profileId, channel.id);
    if (session == nullptr || session->state == SessionState::Failed || session->state == SessionState::Stopping) {
        return {};
    }

    return session->tapUrl;
}

bool DvrController::attachPlaybackForChannel(const Channel &channel)
{
    const auto profileId = guidToString(channel.profileId);
    auto *session = sessionByChannel(profileId, channel.id);
    if (session == nullptr) {
        const auto now = QDateTime::currentDateTimeUtc();
        for (const auto &window : mergedWindows()) {
            if (window.profileId != profileId || window.channelId != channel.id) {
                continue;
            }
            if (!(window.startAt <= now && now < window.stopAt)) {
                continue;
            }

            startSession(window);
            break;
        }
        session = sessionByChannel(profileId, channel.id);
    }
    if (session == nullptr) {
        return false;
    }

    if (session->state == SessionState::Failed || session->state == SessionState::Stopping) {
        return false;
    }

    const auto tapChannel = channelFromWindow(session->window, session->tapUrl);
    const auto current = m_playerController->currentChannelValue();
    if (current.has_value()
        && current->id == tapChannel.id
        && current->profileId == tapChannel.profileId
        && m_playerController->currentPlaybackUrl().trimmed() == tapChannel.streamUrl.trimmed()) {
        return true;
    }

    DebugLogger::instance().log(
        QStringLiteral("dvr"),
        QStringLiteral("Attaching playback to DVR tap for channel %1: %2")
            .arg(session->window.channelName, session->tapUrl));
    m_playerController->playChannel(tapChannel);
    return true;
}

QList<int> DvrController::recordingChannelIdsForProfile(const QString &profileId) const
{
    QList<int> ids;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        if (!it->second || !it->second->recordingStarted) {
            continue;
        }
        if (it->second->window.profileId != profileId) {
            continue;
        }
        if (!ids.contains(it->second->window.channelId)) {
            ids.push_back(it->second->window.channelId);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

QDateTime DvrController::parseIsoUtc(const QString &value)
{
    auto parsed = QDateTime::fromString(value.trimmed(), Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(value.trimmed(), Qt::ISODate);
    }
    return parsed.toUTC();
}

QString DvrController::toIsoUtc(const QDateTime &value)
{
    return value.toUTC().toString(Qt::ISODateWithMs);
}

QString DvrController::sanitizeForFilename(const QString &value)
{
    static const QRegularExpression invalidChars(QStringLiteral(R"([^A-Za-z0-9\-_]+)"));
    auto normalized = value.trimmed();
    normalized.replace(invalidChars, QStringLiteral("_"));
    normalized.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    while (!normalized.isEmpty() && normalized.front() == u'_') {
        normalized.remove(0, 1);
    }
    while (!normalized.isEmpty() && normalized.back() == u'_') {
        normalized.chop(1);
    }
    if (normalized.isEmpty()) {
        normalized = QStringLiteral("programme");
    }
    return normalized.left(64);
}

QString DvrController::makeScheduleId(
    const QString &profileId,
    const int channelId,
    const QDateTime &start,
    const QDateTime &stop,
    const QString &title)
{
    return QStringLiteral("%1|%2|%3|%4|%5")
        .arg(profileId.trimmed())
        .arg(channelId)
        .arg(toIsoUtc(start))
        .arg(toIsoUtc(stop))
        .arg(title.trimmed());
}

QString DvrController::makeChannelKey(const QString &profileId, const int channelId)
{
    return QStringLiteral("%1|%2").arg(profileId.trimmed()).arg(channelId);
}

QString DvrController::makeMergedWindowId(
    const QString &profileId,
    const int channelId,
    const QDateTime &startAt,
    const QDateTime &stopAt)
{
    return QStringLiteral("%1|%2|%3")
        .arg(makeChannelKey(profileId, channelId), toIsoUtc(startAt), toIsoUtc(stopAt));
}

QString DvrController::findFfmpegBinary()
{
    return Core::resolveProcessBinary(QStringLiteral("ffmpeg"));
}

std::optional<quint16> DvrController::reserveUdpPort()
{
    QUdpSocket socket;
    if (!socket.bind(QHostAddress::LocalHost, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        return std::nullopt;
    }
    const auto port = socket.localPort();
    socket.close();
    if (port <= 0) {
        return std::nullopt;
    }
    return static_cast<quint16>(port);
}

std::pair<QDateTime, QDateTime> DvrController::effectiveWindow(const DvrScheduleEntry &entry) const
{
    const auto startOffsetMinutes = m_settings->current().dvrStartOffsetMinutes;
    const auto endOffsetMinutes = m_settings->current().dvrEndOffsetMinutes;
    return {
        entry.start.addSecs(-static_cast<qint64>(startOffsetMinutes) * 60),
        entry.stop.addSecs(static_cast<qint64>(endOffsetMinutes) * 60)
    };
}

QList<DvrController::MergedWindow> DvrController::mergedWindows() const
{
    struct Candidate
    {
        DvrScheduleEntry entry;
        QDateTime startAt;
        QDateTime stopAt;
    };

    QMap<QString, QList<Candidate>> grouped;
    for (const auto &schedule : m_schedules) {
        const auto [startAt, stopAt] = effectiveWindow(schedule);
        if (!startAt.isValid() || !stopAt.isValid() || stopAt <= startAt) {
            continue;
        }
        grouped[makeChannelKey(schedule.profileId, schedule.channelId)].push_back({ schedule, startAt, stopAt });
    }

    QList<MergedWindow> merged;
    for (auto groupIt = grouped.cbegin(); groupIt != grouped.cend(); ++groupIt) {
        auto windows = groupIt.value();
        std::sort(windows.begin(), windows.end(), [](const Candidate &left, const Candidate &right) {
            if (left.startAt != right.startAt) {
                return left.startAt < right.startAt;
            }
            return left.stopAt < right.stopAt;
        });
        if (windows.isEmpty()) {
            continue;
        }

        auto current = windows.front();
        auto currentTitle = current.entry.title.trimmed();
        auto mixedTitles = false;

        auto flushCurrent = [&]() {
            MergedWindow window;
            window.id = makeMergedWindowId(current.entry.profileId, current.entry.channelId, current.startAt, current.stopAt);
            window.channelKey = makeChannelKey(current.entry.profileId, current.entry.channelId);
            window.profileId = current.entry.profileId;
            window.channelId = current.entry.channelId;
            window.channelName = current.entry.channelName;
            window.streamUrl = current.entry.streamUrl;
            window.tvgId = current.entry.tvgId;
            window.displayTitle = mixedTitles || currentTitle.isEmpty() ? QStringLiteral("DVR") : currentTitle;
            window.startAt = current.startAt;
            window.stopAt = current.stopAt;
            merged.push_back(window);
        };

        for (auto index = 1; index < windows.size(); ++index) {
            const auto &next = windows.at(index);
            if (next.startAt <= current.stopAt) {
                if (next.stopAt > current.stopAt) {
                    current.stopAt = next.stopAt;
                }
                const auto nextTitle = next.entry.title.trimmed();
                if (nextTitle != currentTitle) {
                    mixedTitles = true;
                }
                continue;
            }

            flushCurrent();
            current = next;
            currentTitle = current.entry.title.trimmed();
            mixedTitles = false;
        }

        flushCurrent();
    }

    std::sort(merged.begin(), merged.end(), [](const MergedWindow &left, const MergedWindow &right) {
        if (left.startAt != right.startAt) {
            return left.startAt < right.startAt;
        }
        return left.id < right.id;
    });
    return merged;
}

Channel DvrController::channelFromWindow(const MergedWindow &window, const QString &overrideUrl) const
{
    Channel channel;
    channel.id = window.channelId;
    channel.name = window.channelName;
    channel.streamUrl = overrideUrl.trimmed().isEmpty() ? window.streamUrl : overrideUrl.trimmed();
    channel.tvgId = window.tvgId;
    channel.profileId = parseGuid(window.profileId);
    channel.source = ChannelSource::M3U;
    return channel;
}

void DvrController::loadSchedulesFromSettings()
{
    m_schedules = m_settings->current().dvrSchedules;
    std::sort(m_schedules.begin(), m_schedules.end(), [](const DvrScheduleEntry &left, const DvrScheduleEntry &right) {
        if (left.start != right.start) {
            return left.start < right.start;
        }
        return left.id < right.id;
    });
}

void DvrController::persistSchedules()
{
    m_settings->current().dvrSchedules = m_schedules;
    m_settings->save();
}

void DvrController::emitRecordingChannelsChanged()
{
    QMap<QString, QList<int>> currentByProfile;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        if (!it->second || !it->second->recordingStarted) {
            continue;
        }
        auto &ids = currentByProfile[it->second->window.profileId];
        if (!ids.contains(it->second->window.channelId)) {
            ids.push_back(it->second->window.channelId);
        }
    }

    for (auto it = currentByProfile.begin(); it != currentByProfile.end(); ++it) {
        std::sort(it.value().begin(), it.value().end());
    }

    const auto previousProfiles = m_lastRecordingChannelsByProfile.keys();
    const auto currentProfiles = currentByProfile.keys();
    QSet<QString> allProfiles;
    for (const auto &profile : previousProfiles) {
        allProfiles.insert(profile);
    }
    for (const auto &profile : currentProfiles) {
        allProfiles.insert(profile);
    }

    for (const auto &profileId : allProfiles) {
        const auto previous = m_lastRecordingChannelsByProfile.value(profileId);
        const auto current = currentByProfile.value(profileId);
        if (previous != current) {
            emit recordingChannelsChanged(profileId, current);
        }
    }

    m_lastRecordingChannelsByProfile = currentByProfile;
}

void DvrController::tick()
{
    const auto now = QDateTime::currentDateTimeUtc();
    auto schedulesChanged = false;

    QList<DvrScheduleEntry> filteredSchedules;
    filteredSchedules.reserve(m_schedules.size());
    for (const auto &schedule : m_schedules) {
        const auto [startAt, stopAt] = effectiveWindow(schedule);
        if (!startAt.isValid() || !stopAt.isValid() || stopAt <= startAt) {
            schedulesChanged = true;
            continue;
        }
        if (now >= stopAt) {
            schedulesChanged = true;
            continue;
        }
        filteredSchedules.push_back(schedule);
    }

    if (filteredSchedules.size() != m_schedules.size()) {
        m_schedules = filteredSchedules;
        schedulesChanged = true;
    }

    const auto windows = mergedWindows();
    QMap<QString, MergedWindow> activeById;
    for (const auto &window : windows) {
        if (window.startAt <= now && now < window.stopAt) {
            activeById.insert(window.id, window);
        }
    }

    for (auto it = m_restartNotBeforeByWindowId.begin(); it != m_restartNotBeforeByWindowId.end();) {
        if (!activeById.contains(it.key())) {
            it = m_restartNotBeforeByWindowId.erase(it);
            continue;
        }
        ++it;
    }

    for (auto it = activeById.cbegin(); it != activeById.cend(); ++it) {
        if (m_sessions.find(it.key()) == m_sessions.end()) {
            const auto notBefore = m_restartNotBeforeByWindowId.value(it.key());
            if (notBefore.isValid() && now < notBefore) {
                continue;
            }

            if (startSession(it.value())) {
                m_restartNotBeforeByWindowId.remove(it.key());
            } else {
                scheduleRestartForWindow(it.key(), QStringLiteral("start-failed"));
            }
        }
    }

    QList<QString> staleSessionIds;
    QSet<QString> startupTimeoutIds;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        if (!it->second) {
            continue;
        }

        if (!activeById.contains(it->first)) {
            staleSessionIds.push_back(it->first);
            continue;
        }

        if (it->second->state == SessionState::Starting
            && it->second->startRequestedAt.isValid()
            && it->second->startRequestedAt.msecsTo(now) >= kTapStartupTimeoutMs) {
            DebugLogger::instance().log(
                QStringLiteral("dvr"),
                QStringLiteral("DVR tap startup timeout for %1 after %2 ms, restarting session.")
                    .arg(it->first)
                    .arg(it->second->startRequestedAt.msecsTo(now)));
            staleSessionIds.push_back(it->first);
            startupTimeoutIds.insert(it->first);
        }
    }
    for (const auto &id : staleSessionIds) {
        requestStopSession(id, startupTimeoutIds.contains(id) ? QStringLiteral("startup-timeout") : QStringLiteral("tick-stop"));
    }

    if (schedulesChanged) {
        persistSchedules();
    }

    emitRecordingChannelsChanged();
    emit stateChanged();
}

bool DvrController::startSession(const MergedWindow &window)
{
    if (m_sessions.find(window.id) != m_sessions.end()) {
        return true;
    }

    const auto tapPort = reserveUdpPort();
    if (!tapPort.has_value()) {
        DebugLogger::instance().log(
            QStringLiteral("dvr"),
            QStringLiteral("DVR failed to reserve UDP tap port for %1").arg(window.id));
        return false;
    }

    auto session = std::make_unique<Session>();
    session->window = window;
    session->state = SessionState::Starting;
    session->startRequestedAt = QDateTime::currentDateTimeUtc();
    session->stopReason.clear();
    session->tapPort = tapPort.value();
    session->tapUrl = QStringLiteral("udp://127.0.0.1:%1?overrun_nonfatal=1&fifo_size=50000000").arg(session->tapPort);
    session->remuxToMkv = m_settings->current().dvrRemuxToMkv && Core::ffmpegToolsAvailable();
    session->ingestProcess = std::make_unique<QProcess>();
    session->ingestProcess->setProcessChannelMode(QProcess::MergedChannels);

    const auto outputDir = recordingOutputDirectory();
    QDir().mkpath(outputDir);
    const auto timestamp = (window.startAt.isValid() ? window.startAt.toLocalTime() : QDateTime::currentDateTime())
                               .toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const auto baseName = QStringLiteral("%1_%2_%3")
        .arg(sanitizeForFilename(window.channelName), sanitizeForFilename(window.displayTitle), timestamp);
    session->recordTempPath = QDir(outputDir).filePath(baseName + QStringLiteral(".ts"));
    if (session->remuxToMkv) {
        session->recordFinalPath = QDir(outputDir).filePath(baseName + QStringLiteral(".mkv"));
    }

    const auto ffmpeg = findFfmpegBinary();
    const auto udpOutput = QStringLiteral("udp://127.0.0.1:%1?pkt_size=1316").arg(session->tapPort);
    const QStringList args {
        QStringLiteral("-y"),
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("warning"),
        QStringLiteral("-nostdin"),
        QStringLiteral("-i"), window.streamUrl,
        QStringLiteral("-map"), QStringLiteral("0"),
        QStringLiteral("-c"), QStringLiteral("copy"),
        QStringLiteral("-f"), QStringLiteral("mpegts"), session->recordTempPath,
        QStringLiteral("-map"), QStringLiteral("0"),
        QStringLiteral("-c"), QStringLiteral("copy"),
        QStringLiteral("-f"), QStringLiteral("mpegts"), udpOutput
    };

    auto *process = session->ingestProcess.get();
    const auto sessionId = window.id;

    connect(process, &QProcess::readyRead, this, [this, sessionId]() {
        auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end() || !it->second || !it->second->ingestProcess) {
            return;
        }

        auto output = QString::fromLocal8Bit(it->second->ingestProcess->readAll()).trimmed();
        if (output.isEmpty()) {
            return;
        }
        output.replace(u'\r', u' ');
        output.replace(u'\n', QStringLiteral(" | "));
        if (output.size() > 600) {
            output = output.left(600) + QStringLiteral("...");
        }
        DebugLogger::instance().log(
            QStringLiteral("dvr"),
            QStringLiteral("DVR ffmpeg output for %1: %2").arg(sessionId, output));
    });

    connect(process, &QProcess::started, this, [this, sessionId, ffmpeg, args]() {
        auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end() || !it->second) {
            return;
        }

        it->second->recordingStarted = true;
        it->second->failedToStart = false;
        it->second->finishedSignaled = false;
        it->second->processId = static_cast<qint64>(it->second->ingestProcess->processId());
        it->second->state = SessionState::Running;
        it->second->stopReason.clear();
        m_restartNotBeforeByWindowId.remove(sessionId);
        DebugLogger::instance().log(
            QStringLiteral("dvr"),
            QStringLiteral("DVR ffmpeg started for %1 | pid=%2 | tap=%3 | cmd=%4 %5")
                .arg(sessionId)
                .arg(it->second->processId)
                .arg(it->second->tapUrl, ffmpeg, args.join(' ')));
        emitRecordingChannelsChanged();
        emit stateChanged();
        maybeAutoHandoffToTap(*it->second);
    });

    connect(process, &QProcess::errorOccurred, this, [this, sessionId](const QProcess::ProcessError error) {
        auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end() || !it->second) {
            return;
        }

        const auto stopping = it->second->stopRequested || it->second->state == SessionState::Stopping;
        it->second->failedToStart = true;
        if (!stopping) {
            it->second->state = SessionState::Failed;
        }
        DebugLogger::instance().log(
            QStringLiteral("dvr"),
            QStringLiteral("DVR ffmpeg error for %1: %2")
                .arg(sessionId, processErrorText(error)));
        if (error == QProcess::FailedToStart) {
            DebugLogger::instance().log(
                QStringLiteral("dvr"),
                QStringLiteral("DVR ffmpeg failed to start for %1. This usually means ffmpeg is missing; scheduler will retry while window stays active.")
                    .arg(sessionId));
        }
        if (stopping) {
            reconcileStoppingSession(sessionId, QStringLiteral("process-error"));
            return;
        }
        requestStopSession(sessionId, QStringLiteral("process-error"));
    });

    connect(process, &QProcess::finished, this, [this, sessionId](const int exitCode, const QProcess::ExitStatus status) {
        auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end() || !it->second) {
            return;
        }

        it->second->finishedSignaled = true;
        it->second->lastExitCode = exitCode;
        it->second->lastExitStatus = status;
        it->second->processId = 0;

        DebugLogger::instance().log(
            QStringLiteral("dvr"),
            QStringLiteral("DVR ffmpeg finished for %1 (exit=%2 status=%3)")
                .arg(sessionId)
                .arg(exitCode)
                .arg(status == QProcess::NormalExit ? QStringLiteral("normal") : QStringLiteral("crash")));
        finalizeStopSession(sessionId, exitCode, status);
    });

    DebugLogger::instance().log(
        QStringLiteral("dvr"),
        QStringLiteral("Starting DVR ffmpeg for %1 | tap=%2 | cmd=%3 %4")
            .arg(sessionId, session->tapUrl, ffmpeg, args.join(' ')));

    m_sessions.insert_or_assign(window.id, std::move(session));
    auto it = m_sessions.find(window.id);
    if (it == m_sessions.end() || !it->second || !it->second->ingestProcess) {
        return false;
    }

    it->second->ingestProcess->start(ffmpeg, args);
    emit stateChanged();
    return true;
}

void DvrController::maybeAutoHandoffToTap(const Session &session)
{
    const auto current = m_playerController->currentChannelValue();
    if (!current.has_value()) {
        return;
    }
    if (guidToString(current->profileId) != session.window.profileId || current->id != session.window.channelId) {
        return;
    }
    if (m_playerController->timeshiftActive() || m_playerController->currentPlaybackUrl().trimmed() == session.tapUrl.trimmed()) {
        return;
    }

    auto tapChannel = channelFromWindow(session.window, session.tapUrl);
    tapChannel.name = current->name;

    DebugLogger::instance().log(
        QStringLiteral("dvr"),
        QStringLiteral("Auto-handoff playback to DVR tap for %1: %2")
            .arg(session.window.channelName, session.tapUrl));
    m_playerController->playChannel(tapChannel);
}

void DvrController::scheduleRestartForWindow(const QString &windowId, const QString &reason)
{
    const auto normalizedWindowId = windowId.trimmed();
    if (normalizedWindowId.isEmpty()) {
        return;
    }

    const auto normalizedReason = reason.trimmed().isEmpty() ? QStringLiteral("no-reason") : reason.trimmed();
    const auto restartAt = QDateTime::currentDateTimeUtc().addMSecs(kDvrRestartRetryMs);
    m_restartNotBeforeByWindowId.insert(normalizedWindowId, restartAt);
    DebugLogger::instance().log(
        QStringLiteral("dvr"),
        QStringLiteral("Queued DVR ingest restart for %1 at %2 (%3).")
            .arg(normalizedWindowId, restartAt.toString(Qt::ISODateWithMs), normalizedReason));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void DvrController::requestStopSession(const QString &sessionId, const QString &reason)
{
    auto it = m_sessions.find(sessionId);
    if (it == m_sessions.end() || !it->second) {
        return;
    }
    auto *session = it->second.get();
    if (session->stopRequested || session->state == SessionState::Stopping) {
        return;
    }

    session->stopRequested = true;
    session->state = SessionState::Stopping;
    session->stopReason = reason.trimmed().isEmpty() ? QStringLiteral("no-reason") : reason.trimmed();
#if defined(Q_OS_WIN)
    session->orphanSweepRetriesRemaining = kWindowsOrphanSweepRetries;
#endif
    DebugLogger::instance().log(
        QStringLiteral("dvr"),
        QStringLiteral("Stopping DVR session %1 (%2).")
            .arg(sessionId, session->stopReason));

    auto *process = session->ingestProcess.get();
    if (process == nullptr || process->state() == QProcess::NotRunning) {
        reconcileStoppingSession(sessionId, QStringLiteral("already-not-running"));
        return;
    }

    process->terminate();
    QTimer::singleShot(kProcessTerminateTimeoutMs, this, [this, sessionId]() {
        auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end() || !it->second || !it->second->ingestProcess) {
            return;
        }

        auto *session = it->second.get();
        auto *process = session->ingestProcess.get();
        if (session->state != SessionState::Stopping) {
            return;
        }

        if (process->state() == QProcess::NotRunning) {
            reconcileStoppingSession(sessionId, QStringLiteral("post-terminate-not-running"));
            return;
        }

        DebugLogger::instance().log(
            QStringLiteral("dvr"),
            QStringLiteral("DVR session %1 did not terminate in time, sending kill.").arg(sessionId));
        process->kill();
        forceStopSessionProcess(sessionId, kProcessForceKillRetries);
    });
}

void DvrController::forceStopSessionProcess(const QString &sessionId, int retriesLeft)
{
    auto it = m_sessions.find(sessionId);
    if (it == m_sessions.end() || !it->second || !it->second->ingestProcess) {
        return;
    }

    auto *session = it->second.get();
    auto *process = session->ingestProcess.get();
    if (session->state != SessionState::Stopping) {
        return;
    }

    qint64 pid = session->processId;
    if (pid <= 0 && process != nullptr) {
        pid = static_cast<qint64>(process->processId());
    }
    const auto aliveBeforeSignal = isProcessAlive(pid);
    if (!aliveBeforeSignal && (process == nullptr || process->state() == QProcess::NotRunning)) {
        reconcileStoppingSession(sessionId, QStringLiteral("force-stop-already-dead"));
        return;
    }

    if (pid > 0) {
#if defined(Q_OS_WIN)
        QProcess::startDetached(
            QStringLiteral("taskkill"),
            { QStringLiteral("/PID"), QString::number(pid), QStringLiteral("/T"), QStringLiteral("/F") });
#else
        ::kill(static_cast<pid_t>(pid), SIGKILL);
#endif
    } else {
        if (process != nullptr && process->state() != QProcess::NotRunning) {
            process->kill();
        }
    }

    if (retriesLeft <= 0) {
        const auto aliveAfterRetries = isProcessAlive(pid);
        if (aliveAfterRetries) {
            DebugLogger::instance().log(
                QStringLiteral("dvr"),
                QStringLiteral("Unable to force-stop DVR session %1 after retries; process PID=%2 is still alive.")
                    .arg(sessionId)
                    .arg(pid));
            return;
        }
        reconcileStoppingSession(sessionId, QStringLiteral("force-stop-retries-exhausted"));
        return;
    }

    QTimer::singleShot(kProcessForceKillRetryMs, this, [this, sessionId, retriesLeft]() {
        forceStopSessionProcess(sessionId, retriesLeft - 1);
    });
}

void DvrController::sweepWindowsOrphanFfmpeg(const Session &session) const
{
#if defined(Q_OS_WIN)
    const auto fileMarker = QFileInfo(session.recordTempPath).fileName().trimmed();
    const auto tapMarker = QStringLiteral("udp://127.0.0.1:%1?pkt_size=1316").arg(session.tapPort).trimmed();
    if (fileMarker.isEmpty() || tapMarker.isEmpty()) {
        return;
    }

    auto escapedFileMarker = fileMarker;
    auto escapedTapMarker = tapMarker;
    escapedFileMarker.replace(QStringLiteral("'"), QStringLiteral("''"));
    escapedTapMarker.replace(QStringLiteral("'"), QStringLiteral("''"));
    const auto script = QStringLiteral(
                            "$fileNeedle='%1'; "
                            "$tapNeedle='%2'; "
                            "Get-CimInstance Win32_Process -Filter \"Name='ffmpeg.exe'\" | "
                            "Where-Object { "
                            "$_.CommandLine -and "
                            "$_.CommandLine -like ('*' + $fileNeedle + '*') -and "
                            "$_.CommandLine -like ('*' + $tapNeedle + '*') "
                            "} | "
                            "ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }")
                            .arg(escapedFileMarker, escapedTapMarker);
    const auto started = QProcess::startDetached(
        QStringLiteral("powershell.exe"),
        { QStringLiteral("-NoProfile"),
          QStringLiteral("-NonInteractive"),
          QStringLiteral("-ExecutionPolicy"),
          QStringLiteral("Bypass"),
          QStringLiteral("-Command"),
          script });
    DebugLogger::instance().log(
        QStringLiteral("dvr"),
        QStringLiteral("Windows orphan ffmpeg sweep by markers file='%1' tap='%2' started=%3")
            .arg(fileMarker, tapMarker, started ? QStringLiteral("true") : QStringLiteral("false")));
#else
    Q_UNUSED(session);
#endif
}

void DvrController::reconcileStoppingSession(const QString &sessionId, const QString &reason)
{
    auto it = m_sessions.find(sessionId);
    if (it == m_sessions.end() || !it->second) {
        return;
    }

    auto *session = it->second.get();
    auto *process = session->ingestProcess.get();
    qint64 pid = session->processId;
    if (pid <= 0 && process != nullptr) {
        pid = static_cast<qint64>(process->processId());
    }

    const auto qtRunning = process != nullptr && process->state() != QProcess::NotRunning;
    const auto osRunning = isProcessAlive(pid);
    if (qtRunning || osRunning) {
        forceStopSessionProcess(sessionId, kProcessForceKillRetries);
        return;
    }

#if defined(Q_OS_WIN)
    if (!session->finishedSignaled && session->orphanSweepRetriesRemaining > 0) {
        session->orphanSweepRetriesRemaining -= 1;
        DebugLogger::instance().log(
            QStringLiteral("dvr"),
            QStringLiteral("No finished signal for stopped DVR session %1; running orphan sweep (%2 retries left).")
                .arg(sessionId)
                .arg(session->orphanSweepRetriesRemaining));
        sweepWindowsOrphanFfmpeg(*session);
        QTimer::singleShot(kWindowsOrphanSweepDelayMs, this, [this, sessionId]() {
            reconcileStoppingSession(sessionId, QStringLiteral("windows-orphan-sweep"));
        });
        return;
    }
#endif

    const auto exitCode = session->finishedSignaled ? session->lastExitCode : -1;
    const auto exitStatus = session->finishedSignaled ? session->lastExitStatus : QProcess::CrashExit;
    DebugLogger::instance().log(
        QStringLiteral("dvr"),
        QStringLiteral(
            "Reconciling stopped DVR session %1 (%2), finalizing without live process (pid=%3 qtRunning=%4 osRunning=%5 exit=%6 status=%7).")
            .arg(sessionId)
            .arg(reason)
            .arg(pid)
            .arg(qtRunning ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(osRunning ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(exitCode)
            .arg(exitStatus == QProcess::NormalExit ? QStringLiteral("normal") : QStringLiteral("crash")));
    finalizeStopSession(sessionId, exitCode, exitStatus);
}

void DvrController::finalizeStopSession(const QString &sessionId, const int exitCode, const QProcess::ExitStatus status)
{
    auto it = m_sessions.find(sessionId);
    if (it == m_sessions.end() || !it->second) {
        return;
    }

    auto session = std::move(it->second);
    m_sessions.erase(it);
    if (!session) {
        emitRecordingChannelsChanged();
        emit stateChanged();
        return;
    }

    const auto wasRecording = session->recordingStarted;
    session->recordingStarted = false;

    const auto current = m_playerController->currentChannelValue();
    if (wasRecording
        && current.has_value()
        && guidToString(current->profileId) == session->window.profileId
        && current->id == session->window.channelId
        && !m_playerController->timeshiftActive()
        && m_playerController->currentPlaybackUrl().trimmed() == session->tapUrl.trimmed()) {
        auto sourceChannel = channelFromWindow(session->window, session->window.streamUrl);
        sourceChannel.name = current->name;
        DebugLogger::instance().log(
            QStringLiteral("dvr"),
            QStringLiteral("DVR tap ended, falling back to source stream for %1")
                .arg(session->window.channelName));
        m_playerController->playChannel(sourceChannel);
    }

    const auto now = QDateTime::currentDateTimeUtc();
    auto activeWindowStillPresent = false;
    const auto windows = mergedWindows();
    for (const auto &window : windows) {
        if (window.id != session->window.id) {
            continue;
        }
        if (window.startAt <= now && now < window.stopAt) {
            activeWindowStillPresent = true;
        }
        break;
    }

    if (!activeWindowStillPresent) {
        m_restartNotBeforeByWindowId.remove(session->window.id);
    }

    if (wasRecording && !activeWindowStillPresent) {
        maybeStartRemux(*session);
    } else if (wasRecording && activeWindowStillPresent) {
        DebugLogger::instance().log(
            QStringLiteral("dvr"),
            QStringLiteral("Skipping DVR remux for %1 because merged window is still active; scheduler may restart ingest.")
                .arg(sessionId));
    }

    if (activeWindowStillPresent) {
        const auto stopReason = session->stopReason.trimmed().isEmpty()
            ? QStringLiteral("finalize-active-window")
            : session->stopReason.trimmed();
        scheduleRestartForWindow(session->window.id, stopReason);
    }

    DebugLogger::instance().log(
        QStringLiteral("dvr"),
        QStringLiteral("Finalized DVR session %1 (exit=%2 status=%3).")
            .arg(sessionId)
            .arg(exitCode)
            .arg(status == QProcess::NormalExit ? QStringLiteral("normal") : QStringLiteral("crash")));
    emitRecordingChannelsChanged();
    emit stateChanged();
}

DvrController::Session *DvrController::sessionByChannel(const QString &profileId, const int channelId) const
{
    Session *candidateRunning = nullptr;
    Session *candidateStarting = nullptr;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        if (!it->second || it->second->window.profileId != profileId || it->second->window.channelId != channelId) {
            continue;
        }
        if (it->second->state == SessionState::Stopping || it->second->state == SessionState::Failed) {
            continue;
        }
        if (it->second->recordingStarted) {
            candidateRunning = it->second.get();
            continue;
        }
        if (!it->second->failedToStart
            && it->second->state == SessionState::Starting
            && candidateStarting == nullptr) {
            candidateStarting = it->second.get();
        }
    }
    return candidateRunning != nullptr ? candidateRunning : candidateStarting;
}

void DvrController::maybeStartRemux(const Session &session)
{
    if (!session.remuxToMkv || session.recordTempPath.isEmpty() || session.recordFinalPath.isEmpty()) {
        return;
    }
    if (!Core::ffmpegToolsAvailable()) {
        return;
    }
    if (!QFile::exists(session.recordTempPath)) {
        return;
    }

    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    const auto ffmpeg = findFfmpegBinary();
    const QStringList args = {
        QStringLiteral("-y"),
        QStringLiteral("-fflags"), QStringLiteral("+genpts+discardcorrupt"),
        QStringLiteral("-i"), session.recordTempPath,
        QStringLiteral("-map"), QStringLiteral("0"),
        QStringLiteral("-c"), QStringLiteral("copy"),
        QStringLiteral("-copytb"), QStringLiteral("1"),
        QStringLiteral("-avoid_negative_ts"), QStringLiteral("make_zero"),
        session.recordFinalPath
    };

    connect(process, &QProcess::readyRead, this, [this, process, tempPath = session.recordTempPath]() {
        auto output = QString::fromLocal8Bit(process->readAll()).trimmed();
        if (output.isEmpty()) {
            return;
        }
        output.replace(u'\r', u' ');
        output.replace(u'\n', QStringLiteral(" | "));
        if (output.size() > 600) {
            output = output.left(600) + QStringLiteral("...");
        }
        DebugLogger::instance().log(
            QStringLiteral("dvr"),
            QStringLiteral("DVR remux output for %1: %2").arg(tempPath, output));
    });

    connect(process, &QProcess::finished, this, [this, process, tempPath = session.recordTempPath, finalPath = session.recordFinalPath](int exitCode, QProcess::ExitStatus exitStatus) {
        const QFileInfo finalInfo(finalPath);
        const auto finalExists = finalInfo.exists();
        const auto finalSize = finalExists ? finalInfo.size() : 0;
        const auto normalExit = exitStatus == QProcess::NormalExit;
        std::optional<double> tempDurationSeconds;
        std::optional<double> finalDurationSeconds;
        QString tempDurationError;
        QString finalDurationError;
        auto durationDeltaSeconds = -1.0;
        auto durationMatches = false;

        if (finalExists && finalSize > 0) {
            tempDurationSeconds = probeMediaDurationSeconds(tempPath, &tempDurationError);
            finalDurationSeconds = probeMediaDurationSeconds(finalPath, &finalDurationError);
            if (tempDurationSeconds.has_value() && finalDurationSeconds.has_value()) {
                durationDeltaSeconds = std::abs(tempDurationSeconds.value() - finalDurationSeconds.value());
                durationMatches = durationDeltaSeconds <= kRemuxDurationMatchToleranceSeconds;
            }
        }

        const auto remuxAccepted = finalExists && finalSize > 0 && durationMatches;
        if (remuxAccepted) {
            DebugLogger::instance().log(
                QStringLiteral("dvr"),
                QStringLiteral("DVR remux accepted (exit=%1 status=%2 size=%3 ts-duration=%4 mkv-duration=%5 delta=%6): %7")
                    .arg(exitCode)
                    .arg(normalExit ? QStringLiteral("normal") : QStringLiteral("crash"))
                    .arg(finalSize)
                    .arg(tempDurationSeconds.value(), 0, 'f', 3)
                    .arg(finalDurationSeconds.value(), 0, 'f', 3)
                    .arg(durationDeltaSeconds, 0, 'f', 3)
                    .arg(finalPath));
            scheduleDeleteTempRecording(tempPath, kTempDeleteMaxRetries);
        } else {
            DebugLogger::instance().log(
                QStringLiteral("dvr"),
                QStringLiteral("DVR remux rejected (exit=%1 status=%2 output-exists=%3 size=%4 duration-match=%5 ts-duration=%6 mkv-duration=%7 ts-probe=%8 mkv-probe=%9): keeping %10")
                    .arg(exitCode)
                    .arg(normalExit ? QStringLiteral("normal") : QStringLiteral("crash"))
                    .arg(finalExists ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(finalSize)
                    .arg(durationMatches ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(tempDurationSeconds.has_value() ? QString::number(tempDurationSeconds.value(), 'f', 3) : QStringLiteral("n/a"))
                    .arg(finalDurationSeconds.has_value() ? QString::number(finalDurationSeconds.value(), 'f', 3) : QStringLiteral("n/a"))
                    .arg(tempDurationError.isEmpty() ? QStringLiteral("ok") : tempDurationError)
                    .arg(finalDurationError.isEmpty() ? QStringLiteral("ok") : finalDurationError)
                    .arg(tempPath));
            if (finalExists) {
                scheduleDeleteInvalidRemuxOutput(finalPath, kTempDeleteMaxRetries);
            }
        }
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [process, tempPath = session.recordTempPath](QProcess::ProcessError error) {
        DebugLogger::instance().log(
            QStringLiteral("dvr"),
            QStringLiteral("DVR remux process error for %1: %2")
                .arg(tempPath, processErrorText(error)));
        process->deleteLater();
    });
    DebugLogger::instance().log(
        QStringLiteral("dvr"),
        QStringLiteral("Starting DVR remux: %1 -> %2").arg(session.recordTempPath, session.recordFinalPath));
    process->start(ffmpeg, args);
}

void DvrController::scheduleDeleteTempRecording(const QString &path, const int retriesLeft) const
{
    const auto normalizedPath = path.trimmed();
    if (normalizedPath.isEmpty()) {
        return;
    }

    if (QFile::remove(normalizedPath)) {
        DebugLogger::instance().log(
            QStringLiteral("dvr"),
            QStringLiteral("Deleted DVR temp recording: %1").arg(normalizedPath));
        return;
    }

    if (retriesLeft <= 0) {
        DebugLogger::instance().log(
            QStringLiteral("dvr"),
            QStringLiteral("Failed to delete DVR temp recording after retries: %1").arg(normalizedPath));
        return;
    }

    QTimer::singleShot(kTempDeleteRetryDelayMs, this, [this, normalizedPath, retriesLeft]() {
        scheduleDeleteTempRecording(normalizedPath, retriesLeft - 1);
    });
}

void DvrController::scheduleDeleteInvalidRemuxOutput(const QString &path, const int retriesLeft) const
{
    const auto normalizedPath = path.trimmed();
    if (normalizedPath.isEmpty()) {
        return;
    }

    if (QFile::remove(normalizedPath)) {
        DebugLogger::instance().log(
            QStringLiteral("dvr"),
            QStringLiteral("Deleted invalid DVR remux output: %1").arg(normalizedPath));
        return;
    }

    if (retriesLeft <= 0) {
        DebugLogger::instance().log(
            QStringLiteral("dvr"),
            QStringLiteral("Failed to delete invalid DVR remux output after retries: %1").arg(normalizedPath));
        return;
    }

    QTimer::singleShot(kTempDeleteRetryDelayMs, this, [this, normalizedPath, retriesLeft]() {
        scheduleDeleteInvalidRemuxOutput(normalizedPath, retriesLeft - 1);
    });
}

QString DvrController::recordingOutputDirectory() const
{
    auto configured = m_settings->current().dvrRecordingsDirectory.trimmed();
    if (!configured.isEmpty()) {
        return configured;
    }
    return AppDataPaths::recordingsDirectory();
}

} // namespace OKILTV::App
