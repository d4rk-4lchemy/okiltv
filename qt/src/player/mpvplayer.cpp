#include "mpvplayer.h"

#include "../core/models.h"
#include "../core/debuglogger.h"
#include "catchupstreamsession.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QLibrary>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <type_traits>

namespace OKILTV::Player {

namespace {

constexpr int kMpvFormatString = 1;
constexpr int kMpvFormatFlag = 3;
constexpr int kMpvFormatInt64 = 4;
constexpr int kMpvFormatDouble = 5;
constexpr int kMpvFormatNode = 6;
constexpr int kMpvFormatNodeArray = 7;
constexpr int kMpvFormatNodeMap = 8;

constexpr int kMpvEventNone = 0;
constexpr int kMpvEventShutdown = 1;
constexpr int kMpvEventLogMessage = 2;
constexpr int kMpvEventPropertyChange = 16;
constexpr int kMpvEventEndFile = 7;
constexpr int kMpvEventFileLoaded = 8;
constexpr int kMpvEventVideoReconfig = 17;
constexpr int kMpvEventAudioReconfig = 18;
constexpr int kMpvEventPlaybackRestart = 21;

constexpr int kMpvEndFileEof = 0;      // stream reached end naturally
constexpr int kMpvEndFileStop = 2;     // stop command or loadfile replace
constexpr int kMpvEndFileQuit = 3;     // quit
constexpr int kMpvEndFileError = 4;    // stream load/playback error
constexpr int kMpvEndFileRedirect = 5; // playlist entry replaced

constexpr int kRenderParamInvalid = 0;
constexpr int kRenderParamApiType = 1;
constexpr int kRenderParamOpenGlInitParams = 2;

bool envFlagEnabled(const char *name)
{
    const auto value = qEnvironmentVariable(name).trimmed().toLower();
    return value == QStringLiteral("1")
        || value == QStringLiteral("true")
        || value == QStringLiteral("yes")
        || value == QStringLiteral("on");
}
constexpr int kRenderParamOpenGlFbo = 3;
constexpr int kRenderParamFlipY = 4;

constexpr const char *kRenderApiTypeOpenGl = "opengl";
#if defined(Q_OS_WIN)
constexpr const char *kLibraryName = "mpv-2.dll";
#elif defined(Q_OS_MACOS)
constexpr const char *kLibraryName = "libmpv.dylib";
#else
constexpr const char *kLibraryName = "libmpv.so.2";
#endif
constexpr int kEventPollIntervalMs = 16; // ~60 Hz

struct mpv_handle;
struct mpv_render_context;

struct mpv_event
{
    int event_id;
    int error;
    quint64 reply_userdata;
    void *data;
};

struct mpv_opengl_init_params
{
    void *(*get_proc_address)(void *ctx, const char *name);
    void *get_proc_address_ctx;
    const char *extra_exts;
};

struct mpv_opengl_fbo
{
    int fbo;
    int w;
    int h;
    int internal_format;
};

struct mpv_event_log_message
{
    const char *prefix;
    const char *level;
    const char *text;
    int log_level;
};

struct mpv_event_property
{
    const char *name;
    int format;
    void *data;
};

struct mpv_event_end_file
{
    int reason; // 0=EOF 2=STOP 3=QUIT 4=ERROR 5=REDIRECT
    int error;
};

struct mpv_render_param
{
    int type;
    void *data;
};

struct mpv_stream_cb_info
{
    void *cookie;
    qint64 (*read_fn)(void *cookie, char *buf, quint64 nbytes);
    qint64 (*seek_fn)(void *cookie, qint64 offset);
    qint64 (*size_fn)(void *cookie);
    void (*close_fn)(void *cookie);
    void (*cancel_fn)(void *cookie);
};

struct mpv_node;
struct mpv_node_list
{
    int num;
    mpv_node *values;
    char **keys; // null for arrays, filled for maps
};
struct mpv_node
{
    union {
        char *string;
        int flag;
        qint64 int64;
        double double_;
        mpv_node_list *list;
    } u;
    int format;
};

QString escapeArg(QString value)
{
    value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    value.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(value);
}

qint64 monotonicNowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

double roundToSingleDecimal(const double value)
{
    return std::round(value * 10.0) / 10.0;
}

QString secondsOptionValue(const double value)
{
    return QString::number(value, 'f', 1);
}

constexpr qint64 kMiB = 1024LL * 1024LL;
constexpr qint64 kDemuxerMaxBytesFloor = 8 * kMiB;
constexpr qint64 kDemuxerMaxBytesCeil = 8 * 1024 * kMiB;
constexpr qint64 kSteadyStateDemuxerMaxBytesFloor = 8 * kMiB;
constexpr qint64 kSteadyStateDemuxerMaxBackBytesFloor = 8 * kMiB;
constexpr double kDemuxerBytesPerSecond = 2.0 * static_cast<double>(kMiB);
constexpr double kSteadyStateBackBufferSeconds = 30.0;
constexpr double kMpvNetworkTimeoutFloorSeconds = 20.0;
constexpr double kMpvNetworkTimeoutCeilSeconds = 300.0;
constexpr double kMpvNetworkTimeoutWaitMultiplier = 6.0;
constexpr double kMpvNetworkTimeoutBufferMultiplier = 6.0;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double effectiveMpvNetworkTimeoutSeconds(const double waitForDataStreamSeconds, const double bufferSeconds)
{
    const auto normalizedWait = Core::normalizePlayerWaitForStreamSeconds(waitForDataStreamSeconds);
    const auto normalizedBuffer = Core::normalizePlayerBufferSeconds(bufferSeconds);
    const auto derivedFromWait = normalizedWait * kMpvNetworkTimeoutWaitMultiplier;
    const auto derivedFromBuffer = normalizedBuffer * kMpvNetworkTimeoutBufferMultiplier;
    const auto timeoutSeconds = std::max({derivedFromWait, derivedFromBuffer, kMpvNetworkTimeoutFloorSeconds});
    return std::clamp(timeoutSeconds, kMpvNetworkTimeoutFloorSeconds, kMpvNetworkTimeoutCeilSeconds);
}

using CatchupSessionHandle = std::shared_ptr<CatchupStreamSession>;

qint64 catchupStreamRead(void *cookie, char *buffer, const quint64 maxBytes)
{
    auto *handle = static_cast<CatchupSessionHandle *>(cookie);
    if (handle == nullptr || !(*handle)) {
        return -1;
    }
    return (*handle)->read(buffer, maxBytes);
}

qint64 catchupStreamSeek(void *, qint64)
{
    return -1;
}

qint64 catchupStreamSize(void *)
{
    return -1;
}

void catchupStreamClose(void *cookie)
{
    auto *handle = static_cast<CatchupSessionHandle *>(cookie);
    if (handle == nullptr) {
        return;
    }
    if (*handle) {
        (*handle)->cancelRead();
    }
    delete handle;
}

void catchupStreamCancel(void *cookie)
{
    auto *handle = static_cast<CatchupSessionHandle *>(cookie);
    if (handle != nullptr && *handle) {
        (*handle)->cancelRead();
    }
}

int catchupStreamOpen(void *, char *uri, mpv_stream_cb_info *info)
{
    if (uri == nullptr || info == nullptr) {
        return -1;
    }
    auto session = CatchupStreamSession::find(QString::fromUtf8(uri));
    if (!session) {
        Core::DebugLogger::instance().log(
            QStringLiteral("mpv"),
            QStringLiteral("Catch-up stream callback open failed; unknown URI %1.")
                .arg(QString::fromUtf8(uri)));
        return -1;
    }
    info->cookie = new CatchupSessionHandle(std::move(session));
    info->read_fn = catchupStreamRead;
    info->seek_fn = catchupStreamSeek;
    info->size_fn = catchupStreamSize;
    info->close_fn = catchupStreamClose;
    info->cancel_fn = catchupStreamCancel;
    return 0;
}

} // namespace

struct MpvPlayer::Api
{
    using CreateFn = mpv_handle *(*)();
    using InitializeFn = int (*)(mpv_handle *);
    using TerminateDestroyFn = void (*)(mpv_handle *);
    using SetOptionStringFn = int (*)(mpv_handle *, const char *, const char *);
    using CommandFn = int (*)(mpv_handle *, const char *const[]);
    using CommandStringFn = int (*)(mpv_handle *, const char *);
    using SetPropertyFn = int (*)(mpv_handle *, const char *, int, void *);
    using GetPropertyFn = int (*)(mpv_handle *, const char *, int, void *);
    using ObservePropertyFn = int (*)(mpv_handle *, quint64, const char *, int);
    using RequestLogMessagesFn = int (*)(mpv_handle *, const char *);
    using WaitEventFn = mpv_event *(*)(mpv_handle *, double);
    using ErrorStringFn = const char *(*)(int);
    using FreeFn = void (*)(void *);
    using FreeNodeContentsFn = void (*)(mpv_node *);
    using RenderContextCreateFn = int (*)(mpv_render_context **, mpv_handle *, mpv_render_param *);
    using RenderContextFreeFn = void (*)(mpv_render_context *);
    using RenderContextSetUpdateCallbackFn = void (*)(mpv_render_context *, void (*)(void *), void *);
    using RenderContextRenderFn = int (*)(mpv_render_context *, mpv_render_param *);
    using RenderContextUpdateFn = quint64 (*)(mpv_render_context *);
    using RenderContextReportSwapFn = void (*)(mpv_render_context *);
    using StreamCbAddRoFn = int (*)(mpv_handle *, const char *, void *, int (*)(void *, char *, mpv_stream_cb_info *));

    QLibrary library;
    CreateFn create = nullptr;
    InitializeFn initialize = nullptr;
    TerminateDestroyFn terminateDestroy = nullptr;
    SetOptionStringFn setOptionString = nullptr;
    CommandFn command = nullptr;
    CommandStringFn commandString = nullptr;
    SetPropertyFn setProperty = nullptr;
    GetPropertyFn getProperty = nullptr;
    ObservePropertyFn observeProperty = nullptr;
    RequestLogMessagesFn requestLogMessages = nullptr;
    WaitEventFn waitEvent = nullptr;
    ErrorStringFn errorString = nullptr;
    FreeFn free = nullptr;
    FreeNodeContentsFn freeNodeContents = nullptr;
    RenderContextCreateFn renderContextCreate = nullptr;
    RenderContextFreeFn renderContextFree = nullptr;
    RenderContextSetUpdateCallbackFn renderContextSetUpdateCallback = nullptr;
    RenderContextRenderFn renderContextRender = nullptr;
    RenderContextUpdateFn renderContextUpdate = nullptr;
    RenderContextReportSwapFn renderContextReportSwap = nullptr;
    StreamCbAddRoFn streamCbAddRo = nullptr;
};

struct MpvPlayer::State
{
    QMutex mutex;
    mpv_handle *handle = nullptr;
    mpv_render_context *renderContext = nullptr;
    bool initialized = false;
};

MpvPlayer::MpvPlayer(QObject *parent)
    : QObject(parent)
    , m_api(std::make_unique<Api>())
    , m_state(std::make_unique<State>())
{
#if defined(Q_OS_WIN)
    m_windowsPacingDiagEnabled = envFlagEnabled("OKILTV_WINDOWS_PACING_DIAG");
    if (m_windowsPacingDiagEnabled) {
        Core::DebugLogger::instance().log(
            QStringLiteral("mpv"),
            QStringLiteral("Windows pacing diagnostics enabled (OKILTV_WINDOWS_PACING_DIAG=1)."));
        m_lastWindowsRenderStatsLogTimestampMs.store(monotonicNowMs());
        m_windowsRenderStatsTimer.setInterval(1000);
        connect(&m_windowsRenderStatsTimer, &QTimer::timeout, this, &MpvPlayer::logWindowsRenderStats);
        m_windowsRenderStatsTimer.start();
    }
#endif
}

MpvPlayer::~MpvPlayer()
{
    unload();
}

qint64 MpvPlayer::demuxerMaxBytesForBufferSeconds(const double bufferSeconds)
{
    const auto normalizedBuffer = Core::normalizePlayerBufferSeconds(bufferSeconds);
    const auto rawBytes = static_cast<qint64>(std::llround(normalizedBuffer * kDemuxerBytesPerSecond));
    return std::clamp(rawBytes, kDemuxerMaxBytesFloor, kDemuxerMaxBytesCeil);
}

double MpvPlayer::cacheWindowSecondsForBufferTarget(const double bufferTargetSeconds)
{
    const auto normalizedTarget = Core::normalizePlayerBufferSeconds(bufferTargetSeconds);
    return std::clamp(std::max(normalizedTarget * 3.0, normalizedTarget + 8.0), 10.0, 120.0);
}

double MpvPlayer::steadyStateBackBufferSeconds()
{
    return kSteadyStateBackBufferSeconds;
}

double MpvPlayer::steadyStateCacheLimitSecondsForBufferTarget(const double bufferTargetSeconds)
{
    return Core::normalizePlayerBufferSeconds(bufferTargetSeconds);
}

double MpvPlayer::steadyStateCacheHysteresisSecondsForBufferTarget(const double bufferTargetSeconds)
{
    const auto normalizedTarget = Core::normalizePlayerBufferSeconds(bufferTargetSeconds);
    return std::clamp(roundToSingleDecimal(normalizedTarget - 1.0), 0.1, normalizedTarget);
}

void MpvPlayer::configureLibraryPath(const QString &path)
{
    const auto normalizedPath = path.trimmed();
    if (m_libraryPath == normalizedPath) {
        return;
    }

    m_libraryPath = normalizedPath;
    QMutexLocker locker(&m_state->mutex);
    if (m_state->initialized) {
        m_reinitializePending = true;
    }
}

void MpvPlayer::configureOptions(const QMap<QString, QString> &options)
{
    if (m_options == options) {
        return;
    }

    m_options = options;
    QMutexLocker locker(&m_state->mutex);
    if (m_state->initialized) {
        m_reinitializePending = true;
    }
}

void MpvPlayer::configurePlaybackTuning(
    const double waitForDataStreamSeconds,
    const bool deinterlaceEnabled,
    const double bufferSeconds)
{
    const auto normalizedWait = Core::normalizePlayerWaitForStreamSeconds(waitForDataStreamSeconds);
    const auto normalizedBuffer = Core::normalizePlayerBufferSeconds(bufferSeconds);
    if (std::abs(m_waitForDataStreamSeconds - normalizedWait) <= 0.0001
        && m_deinterlaceEnabled == deinterlaceEnabled
        && std::abs(m_bufferSeconds - normalizedBuffer) <= 0.0001) {
        return;
    }

    m_waitForDataStreamSeconds = normalizedWait;
    m_deinterlaceEnabled = deinterlaceEnabled;
    m_bufferSeconds = normalizedBuffer;
    QMutexLocker locker(&m_state->mutex);
    if (m_state->initialized) {
        m_reinitializePending = true;
    }
}

void MpvPlayer::configureUserAgent(const QString &userAgent)
{
    const auto normalizedUserAgent = userAgent.trimmed();
    if (m_userAgent == normalizedUserAgent) {
        return;
    }

    m_userAgent = normalizedUserAgent;
    QMutexLocker locker(&m_state->mutex);
    if (m_state->initialized) {
        m_reinitializePending = true;
    }
}

void MpvPlayer::setStartupBufferingStrictMode(const bool enabled)
{
    if (m_startupBufferingStrictMode == enabled) {
        return;
    }

    m_startupBufferingStrictMode = enabled;
    if (!ensureInitialized()) {
        return;
    }

    const auto cachePauseValue = enabled ? QStringLiteral("yes") : QStringLiteral("no");
    const auto cachePauseWaitValue = enabled ? secondsOptionValue(m_bufferSeconds) : QStringLiteral("0.0");
    if (m_api->setProperty != nullptr) {
        int cachePauseFlag = enabled ? 1 : 0;
        const auto setCachePauseResult =
            m_api->setProperty(m_state->handle, "cache-pause", kMpvFormatFlag, &cachePauseFlag);
        if (setCachePauseResult < 0) {
            Core::DebugLogger::instance().log(
                QStringLiteral("mpv"),
                QStringLiteral("Failed to update cache-pause at runtime: %1")
                    .arg(QString::fromUtf8(m_api->errorString(setCachePauseResult))));
        }

        auto cachePauseWaitValueSeconds = enabled ? m_bufferSeconds : 0.0;
        const auto setCachePauseWaitResult =
            m_api->setProperty(m_state->handle, "cache-pause-wait", kMpvFormatDouble, &cachePauseWaitValueSeconds);
        if (setCachePauseWaitResult < 0) {
            Core::DebugLogger::instance().log(
                QStringLiteral("mpv"),
                QStringLiteral("Failed to update cache-pause-wait at runtime: %1")
                    .arg(QString::fromUtf8(m_api->errorString(setCachePauseWaitResult))));
        }
    } else if (m_api->commandString != nullptr) {
        const auto setCachePauseCommand = QStringLiteral("no-osd set cache-pause %1").arg(cachePauseValue);
        const auto setCachePauseResult = m_api->commandString(m_state->handle, setCachePauseCommand.toUtf8().constData());
        if (setCachePauseResult < 0) {
            Core::DebugLogger::instance().log(
                QStringLiteral("mpv"),
                QStringLiteral("Failed to update cache-pause at runtime: %1")
                    .arg(QString::fromUtf8(m_api->errorString(setCachePauseResult))));
        }

        const auto setCachePauseWaitCommand = QStringLiteral("no-osd set cache-pause-wait %1").arg(cachePauseWaitValue);
        const auto setCachePauseWaitResult =
            m_api->commandString(m_state->handle, setCachePauseWaitCommand.toUtf8().constData());
        if (setCachePauseWaitResult < 0) {
            Core::DebugLogger::instance().log(
                QStringLiteral("mpv"),
                QStringLiteral("Failed to update cache-pause-wait at runtime: %1")
                    .arg(QString::fromUtf8(m_api->errorString(setCachePauseWaitResult))));
        }
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("mpv"),
        QStringLiteral("Startup buffering strict mode is now %1.")
            .arg(enabled ? QStringLiteral("enabled") : QStringLiteral("disabled")));
}

bool MpvPlayer::setSteadyStateBufferingPolicy(const SteadyStateBufferingPolicy &policy)
{
    const auto normalizedCacheLimitSeconds =
        std::clamp(roundToSingleDecimal(policy.cacheLimitSeconds), m_bufferSeconds, 120.0);
    const auto normalizedHysteresisSeconds = std::clamp(
        roundToSingleDecimal(policy.hysteresisSeconds),
        0.1,
        normalizedCacheLimitSeconds);
    const auto normalizedMaxBytes = std::clamp(policy.maxBytes, kSteadyStateDemuxerMaxBytesFloor, kDemuxerMaxBytesCeil);
    const auto normalizedMaxBackBytes =
        std::clamp(policy.maxBackBytes, kSteadyStateDemuxerMaxBackBytesFloor, normalizedMaxBytes);

    const auto changed =
        std::abs(m_steadyStateCacheLimitSeconds - normalizedCacheLimitSeconds) > 0.0001
        || std::abs(m_steadyStateCacheHysteresisSeconds - normalizedHysteresisSeconds) > 0.0001
        || m_steadyStateDemuxerMaxBytes != normalizedMaxBytes
        || m_steadyStateDemuxerMaxBackBytes != normalizedMaxBackBytes;

    m_steadyStateCacheLimitSeconds = normalizedCacheLimitSeconds;
    m_steadyStateCacheHysteresisSeconds = normalizedHysteresisSeconds;
    m_steadyStateDemuxerMaxBytes = normalizedMaxBytes;
    m_steadyStateDemuxerMaxBackBytes = normalizedMaxBackBytes;

    QMutexLocker locker(&m_state->mutex);
    if (!changed || !m_state->initialized || m_state->handle == nullptr) {
        return true;
    }

    const auto readaheadOk = setRuntimeDoubleOption("demuxer-readahead-secs", m_steadyStateCacheLimitSeconds);
    const auto cacheOk = setRuntimeDoubleOption("cache-secs", m_steadyStateCacheLimitSeconds);
    const auto hysteresisOk =
        setRuntimeDoubleOption("demuxer-hysteresis-secs", m_steadyStateCacheHysteresisSeconds);
    const auto maxBytesOk = setRuntimeInt64Option("demuxer-max-bytes", m_steadyStateDemuxerMaxBytes);
    const auto maxBackBytesOk =
        setRuntimeInt64Option("demuxer-max-back-bytes", m_steadyStateDemuxerMaxBackBytes);

    Core::DebugLogger::instance().log(
        QStringLiteral("mpv"),
        QStringLiteral(
            "Applied steady-state buffering policy: cache-limit=%1s hysteresis=%2s max-bytes=%3 max-back-bytes=%4.")
            .arg(m_steadyStateCacheLimitSeconds, 0, 'f', 1)
            .arg(m_steadyStateCacheHysteresisSeconds, 0, 'f', 1)
            .arg(m_steadyStateDemuxerMaxBytes)
            .arg(m_steadyStateDemuxerMaxBackBytes));

    return readaheadOk && cacheOk && hysteresisOk && maxBytesOk && maxBackBytesOk;
}

void MpvPlayer::resetSteadyStateBuffering()
{
    const auto cacheLimitSeconds = steadyStateCacheLimitSecondsForBufferTarget(m_bufferSeconds);
    const auto hysteresisSeconds = steadyStateCacheHysteresisSecondsForBufferTarget(m_bufferSeconds);
    const auto maxBackBytes = demuxerMaxBytesForBufferSeconds(steadyStateBackBufferSeconds());
    const auto maxBytes = demuxerMaxBytesForBufferSeconds(cacheLimitSeconds + steadyStateBackBufferSeconds());
    setSteadyStateBufferingPolicy({
        .cacheLimitSeconds = cacheLimitSeconds,
        .hysteresisSeconds = hysteresisSeconds,
        .maxBytes = maxBytes,
        .maxBackBytes = maxBackBytes,
    });
}

QString MpvPlayer::diagnostics() const
{
    return m_diagnostics;
}

bool MpvPlayer::isAvailable() const
{
    return QFileInfo::exists(resolvedLibraryPath()) || QFileInfo::exists(QCoreApplication::applicationDirPath() + u'/' + kLibraryName);
}

bool MpvPlayer::catchupStreamProtocolAvailable() const
{
    return m_api != nullptr && m_api->streamCbAddRo != nullptr;
}

void MpvPlayer::setRenderUpdateTarget(QObject *target)
{
    if (m_updateTarget == target) {
        return;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("mpv"),
        QStringLiteral("Render update target changed: player=%1 target=%2 name=%3")
            .arg(reinterpret_cast<quintptr>(this), 0, 16)
            .arg(reinterpret_cast<quintptr>(target), 0, 16)
            .arg(target != nullptr ? target->objectName() : QStringLiteral("<null>")));
    m_updateTarget = target;
}

bool MpvPlayer::ensureInitialized()
{
    QMutexLocker locker(&m_state->mutex);
    if (m_state->initialized) {
        return true;
    }

    if (!loadApi()) {
        emit errorOccurred(m_diagnostics);
        return false;
    }

    m_state->handle = m_api->create();
    if (m_state->handle == nullptr) {
        m_diagnostics = QStringLiteral("mpv_create returned null.");
        Core::DebugLogger::instance().log(QStringLiteral("mpv"), m_diagnostics);
        emit errorOccurred(m_diagnostics);
        return false;
    }

    const auto applyOption = [&](const char *key, const QString &value) {
        m_api->setOptionString(m_state->handle, key, value.toUtf8().constData());
    };

    applyOption("vo", QStringLiteral("libmpv"));
    applyOption("cache", QStringLiteral("yes"));
    applyOption("idle", QStringLiteral("yes"));
    applyOption("keep-open", QStringLiteral("yes"));
    applyOption("force-window", QStringLiteral("yes"));
    applyOption("gpu-api", QStringLiteral("opengl"));
    applyOption("opengl-es", QStringLiteral("no"));
    // Render-API mode: Qt controls vsync, so don't let mpv fight it with frame-timing adjustments
    applyOption("video-timing-offset", QStringLiteral("0"));
    // Hardware decoding reduces CPU load during UI navigation; user can override via mpvOptions
    applyOption("hwdec", QStringLiteral("auto-safe"));
    if (envFlagEnabled("OKILTV_HEADLESS_TEST")) {
        applyOption("vo", QStringLiteral("null"));
        applyOption("force-window", QStringLiteral("no"));
        applyOption("hwdec", QStringLiteral("no"));
        applyOption("ao", QStringLiteral("null"));
        applyOption("load-scripts", QStringLiteral("no"));
    }

    for (auto it = m_options.cbegin(); it != m_options.cend(); ++it) {
        m_api->setOptionString(m_state->handle, it.key().toUtf8().constData(), it.value().toUtf8().constData());
    }

    m_steadyStateCacheLimitSeconds = steadyStateCacheLimitSecondsForBufferTarget(m_bufferSeconds);
    m_steadyStateCacheHysteresisSeconds = steadyStateCacheHysteresisSecondsForBufferTarget(m_bufferSeconds);
    m_steadyStateDemuxerMaxBackBytes = demuxerMaxBytesForBufferSeconds(steadyStateBackBufferSeconds());
    m_steadyStateDemuxerMaxBytes =
        demuxerMaxBytesForBufferSeconds(m_steadyStateCacheLimitSeconds + steadyStateBackBufferSeconds());
    const auto networkTimeoutSeconds = effectiveMpvNetworkTimeoutSeconds(m_waitForDataStreamSeconds, m_bufferSeconds);
    applyOption("demuxer-max-bytes", QString::number(m_steadyStateDemuxerMaxBytes));
    applyOption("demuxer-max-back-bytes", QString::number(m_steadyStateDemuxerMaxBackBytes));
    applyOption("demuxer-readahead-secs", secondsOptionValue(m_steadyStateCacheLimitSeconds));
    applyOption("demuxer-hysteresis-secs", secondsOptionValue(m_steadyStateCacheHysteresisSeconds));
    applyOption("cache-secs", secondsOptionValue(m_steadyStateCacheLimitSeconds));
    applyOption("cache-pause", m_startupBufferingStrictMode ? QStringLiteral("yes") : QStringLiteral("no"));
    applyOption(
        "cache-pause-wait",
        m_startupBufferingStrictMode ? secondsOptionValue(m_bufferSeconds) : QStringLiteral("0.0"));
    applyOption("network-timeout", secondsOptionValue(networkTimeoutSeconds));
    if (!m_userAgent.isEmpty()) {
        applyOption("user-agent", m_userAgent);
    }
    // Deinterlacing is controlled directly by the user setting. mpv/yadif decides per-frame
    // handling internally; app logic does not gate filter activation by source scan type.
    applyOption("deinterlace", m_deinterlaceEnabled ? QStringLiteral("yes") : QStringLiteral("no"));
    registerCatchupStreamProtocol();

    const auto initCode = m_api->initialize(m_state->handle);
    if (initCode < 0) {
        const auto error = QString::fromUtf8(m_api->errorString(initCode));
        m_diagnostics = QStringLiteral("mpv_initialize failed: %1").arg(error);
        Core::DebugLogger::instance().log(QStringLiteral("mpv"), m_diagnostics);
        emit errorOccurred(m_diagnostics);
        return false;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("mpv"),
        QStringLiteral(
            "Configured stream tuning: wait-for-data=%1s buffer=%2s steady-cache-limit=%3s hysteresis=%4s max-bytes=%5 max-back-bytes=%6 network-timeout=%7s.")
            .arg(m_waitForDataStreamSeconds, 0, 'f', 1)
            .arg(m_bufferSeconds, 0, 'f', 1)
            .arg(m_steadyStateCacheLimitSeconds, 0, 'f', 1)
            .arg(m_steadyStateCacheHysteresisSeconds, 0, 'f', 1)
            .arg(m_steadyStateDemuxerMaxBytes)
            .arg(m_steadyStateDemuxerMaxBackBytes)
            .arg(networkTimeoutSeconds, 0, 'f', 1));

    if (m_api->requestLogMessages != nullptr) {
        const auto logCode = m_api->requestLogMessages(m_state->handle, "debug");
        if (logCode < 0) {
            Core::DebugLogger::instance().log(
                QStringLiteral("mpv"),
                QStringLiteral("mpv_request_log_messages failed: %1")
                    .arg(QString::fromUtf8(m_api->errorString(logCode))));
        }
    }

    if (m_api->observeProperty != nullptr) {
        const auto observeCode = m_api->observeProperty(m_state->handle, 1, "pause", kMpvFormatFlag);
        if (observeCode < 0) {
            Core::DebugLogger::instance().log(
                QStringLiteral("mpv"),
                QStringLiteral("mpv_observe_property(pause) failed: %1")
                    .arg(QString::fromUtf8(m_api->errorString(observeCode))));
        }

        const auto observeBufferingCode = m_api->observeProperty(m_state->handle, 2, "paused-for-cache", kMpvFormatFlag);
        if (observeBufferingCode < 0) {
            Core::DebugLogger::instance().log(
                QStringLiteral("mpv"),
                QStringLiteral("mpv_observe_property(paused-for-cache) failed: %1")
                    .arg(QString::fromUtf8(m_api->errorString(observeBufferingCode))));
        }
    }

    m_state->initialized = true;
    m_diagnostics = QStringLiteral("Loaded mpv from %1").arg(m_api->library.fileName());
    Core::DebugLogger::instance().log(QStringLiteral("mpv"), m_diagnostics);
    if (!envFlagEnabled("OKILTV_HEADLESS_TEST")) {
        startEventThread();
    }
    return true;
}

bool MpvPlayer::ensureRenderContext()
{
    if (!ensureInitialized()) {
        return false;
    }

    QMutexLocker locker(&m_state->mutex);
    if (m_state->renderContext != nullptr) {
        return true;
    }

    auto *context = QOpenGLContext::currentContext();
    if (context == nullptr) {
#if defined(Q_OS_WINDOWS)
        m_diagnostics = QStringLiteral(
            "No current OpenGL context is available for libmpv video rendering. "
            "This Windows build must run with the Qt Quick OpenGL backend "
            "(QT_OPENGL=desktop, QSG_RHI_BACKEND=opengl).");
#else
        m_diagnostics = QStringLiteral("No current OpenGL context available for mpv render context.");
#endif
        Core::DebugLogger::instance().log(QStringLiteral("mpv"), m_diagnostics);
        emit errorOccurred(m_diagnostics);
        return false;
    }

    mpv_opengl_init_params openGlParams {
        &MpvPlayer::getProcAddress,
        this,
        nullptr
    };
    mpv_render_param params[] = {
        { kRenderParamApiType, const_cast<char *>(kRenderApiTypeOpenGl) },
        { kRenderParamOpenGlInitParams, &openGlParams },
        { kRenderParamInvalid, nullptr }
    };

    const auto renderCode = m_api->renderContextCreate(&m_state->renderContext, m_state->handle, params);
    if (renderCode < 0) {
        const auto error = QString::fromUtf8(m_api->errorString(renderCode));
        m_diagnostics = QStringLiteral("mpv_render_context_create failed: %1").arg(error);
        Core::DebugLogger::instance().log(QStringLiteral("mpv"), m_diagnostics);
        emit errorOccurred(m_diagnostics);
        return false;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("mpv"),
        QStringLiteral("Render context created with current OpenGL context %1.")
            .arg(reinterpret_cast<quintptr>(context), 0, 16));
    m_api->renderContextSetUpdateCallback(m_state->renderContext, &MpvPlayer::onRenderUpdate, this);
    return true;
}

bool MpvPlayer::registerCatchupStreamProtocol()
{
    if (m_api->streamCbAddRo == nullptr || m_state->handle == nullptr) {
        Core::DebugLogger::instance().log(
            QStringLiteral("mpv"),
            QStringLiteral("libmpv stream_cb unavailable; catch-up owned stream disabled."));
        return false;
    }
    const auto result = m_api->streamCbAddRo(m_state->handle, "okiltv-catchup", nullptr, &catchupStreamOpen);
    if (result < 0) {
        Core::DebugLogger::instance().log(
            QStringLiteral("mpv"),
            QStringLiteral("mpv_stream_cb_add_ro(okiltv-catchup) failed: %1")
                .arg(QString::fromUtf8(m_api->errorString(result))));
        return false;
    }
    Core::DebugLogger::instance().log(
        QStringLiteral("mpv"),
        QStringLiteral("Registered okiltv-catchup stream callback protocol."));
    return true;
}

void MpvPlayer::play(const QString &url, const QString &loadfileOptions)
{
    if (m_reinitializePending) {
        Core::DebugLogger::instance().log(
            QStringLiteral("mpv"),
            QStringLiteral("Reinitializing libmpv before tune to apply updated player settings."));
        unload();
        m_reinitializePending = false;
    }

    if (!ensureInitialized()) {
        return;
    }
    if (envFlagEnabled("OKILTV_HEADLESS_TEST")) {
        Core::DebugLogger::instance().log(
            QStringLiteral("mpv"),
            QStringLiteral("Skipping loadfile in headless test mode for %1.").arg(url));
        return;
    }
    Core::DebugLogger::instance().log(
        QStringLiteral("mpv"),
        loadfileOptions.trimmed().isEmpty()
            ? QStringLiteral("loadfile %1").arg(url)
            : QStringLiteral("loadfile %1 (%2)").arg(url, loadfileOptions));

    int commandCode = 0;
    if (m_api->command != nullptr) {
        const auto urlUtf8 = url.toUtf8();
        const auto optionsUtf8 = loadfileOptions.toUtf8();
        if (optionsUtf8.isEmpty()) {
            const char *arguments[] = { "loadfile", urlUtf8.constData(), "replace", nullptr };
            commandCode = m_api->command(m_state->handle, arguments);
        } else {
            const char *arguments[] = {
                "loadfile",
                urlUtf8.constData(),
                "replace",
                "-1",
                optionsUtf8.constData(),
                nullptr
            };
            commandCode = m_api->command(m_state->handle, arguments);
        }
    } else {
        const auto command = loadfileOptions.trimmed().isEmpty()
            ? QStringLiteral("loadfile %1 replace").arg(escapeArg(url))
            : QStringLiteral("loadfile %1 replace -1 %2")
                  .arg(escapeArg(url), escapeArg(loadfileOptions));
        commandCode = m_api->commandString(m_state->handle, command.toUtf8().constData());
    }

    if (commandCode < 0) {
        const auto error = QString::fromUtf8(m_api->errorString(commandCode));
        m_diagnostics = QStringLiteral("mpv loadfile failed: %1").arg(error);
        Core::DebugLogger::instance().log(QStringLiteral("mpv"), m_diagnostics);
        emit errorOccurred(m_diagnostics);
    }
}

void MpvPlayer::stop()
{
    stopStreamRecord();
    if (!ensureInitialized()) {
        return;
    }

    m_api->commandString(m_state->handle, "stop");
}

void MpvPlayer::setHwdec(const QString &mode)
{
    QMutexLocker locker(&m_state->mutex);
    if (!m_state->initialized || m_state->handle == nullptr) {
        return;
    }

    const auto modeUtf8 = mode.toUtf8();
    const char *modeStr = modeUtf8.constData();
    const auto result =
        m_api->setProperty(m_state->handle, "hwdec", kMpvFormatString, static_cast<void *>(&modeStr));
    Core::DebugLogger::instance().log(
        QStringLiteral("mpv"),
        QStringLiteral("hwdec set to '%1': result=%2.").arg(mode).arg(result));
}

bool MpvPlayer::takeScreenshot(const QString &outputPath)
{
    if (!m_state || !m_state->handle) {
        return false;
    }
    const auto pathUtf8 = outputPath.toUtf8();
    if (m_api->command != nullptr) {
        const char *args[] = { "screenshot-to-file", pathUtf8.constData(), "video", nullptr };
        return m_api->command(m_state->handle, args) >= 0;
    }
    const auto cmd = QStringLiteral("screenshot-to-file %1 video").arg(escapeArg(outputPath));
    return m_api->commandString(m_state->handle, cmd.toUtf8().constData()) >= 0;
}

bool MpvPlayer::startStreamRecord(const QString &outputPath)
{
    if (!m_state || !m_state->handle) {
        return false;
    }
    const auto pathUtf8 = outputPath.toUtf8();
    const char *pathStr = pathUtf8.constData();
    const auto result =
        m_api->setProperty(m_state->handle, "stream-record", kMpvFormatString, static_cast<void *>(&pathStr));
    m_recording = (result >= 0);
    return m_recording;
}

void MpvPlayer::stopStreamRecord()
{
    m_recording = false;
    if (!m_state || !m_state->handle) {
        return;
    }
    const char *empty = "";
    m_api->setProperty(m_state->handle, "stream-record", kMpvFormatString, static_cast<void *>(&empty));
}

void MpvPlayer::togglePause()
{
    if (!ensureInitialized()) {
        return;
    }

    m_api->commandString(m_state->handle, "cycle pause");
}

void MpvPlayer::setPaused(const bool paused)
{
    if (!ensureInitialized()) {
        return;
    }

    int pausedFlag = paused ? 1 : 0;
    m_api->setProperty(m_state->handle, "pause", kMpvFormatFlag, &pausedFlag);
}

void MpvPlayer::setVolume(const int volume)
{
    m_volumeRequested = std::clamp(volume, 0, 100);
    if (!ensureInitialized()) {
        return;
    }

    auto value = static_cast<double>(m_volumeRequested);
    m_api->setProperty(m_state->handle, "volume", kMpvFormatDouble, &value);
}

void MpvPlayer::setAudioEnabled(const bool enabled)
{
    m_audioEnabledRequested = enabled;
    if (!ensureInitialized()) {
        return;
    }

    const char *trackSelection = enabled ? "auto" : "no";
    const auto result =
        m_api->setProperty(m_state->handle, "aid", kMpvFormatString, static_cast<void *>(&trackSelection));
    if (result < 0) {
        Core::DebugLogger::instance().log(
            QStringLiteral("mpv"),
            QStringLiteral("Failed to set audio %1: %2")
                .arg(enabled ? QStringLiteral("enabled") : QStringLiteral("disabled"),
                     QString::fromUtf8(m_api->errorString(result))));
    }
}

int MpvPlayer::requestedVolume() const
{
    return m_volumeRequested;
}

bool MpvPlayer::audioEnabledRequested() const
{
    return m_audioEnabledRequested;
}

void MpvPlayer::seekRelative(const double seconds)
{
    if (!ensureInitialized()) {
        return;
    }

    const auto command = QStringLiteral("no-osd seek %1 relative").arg(seconds, 0, 'f', 1);
    m_api->commandString(m_state->handle, command.toUtf8().constData());
}

void MpvPlayer::seekAbsolute(const double seconds)
{
    if (!ensureInitialized()) {
        return;
    }

    const auto command = QStringLiteral("no-osd seek %1 absolute").arg(seconds, 0, 'f', 3);
    m_api->commandString(m_state->handle, command.toUtf8().constData());
}

void MpvPlayer::seekAbsoluteFast(const double seconds)
{
    if (!ensureInitialized()) {
        return;
    }

    const auto command = QStringLiteral("no-osd seek %1 absolute+keyframes").arg(seconds, 0, 'f', 3);
    m_api->commandString(m_state->handle, command.toUtf8().constData());
}

bool MpvPlayer::setRuntimeDoubleOption(const char *name, const double value)
{
    if (!m_state->initialized || m_state->handle == nullptr) {
        return false;
    }

    if (m_api->setProperty != nullptr) {
        auto numericValue = value;
        const auto result = m_api->setProperty(m_state->handle, name, kMpvFormatDouble, &numericValue);
        if (result >= 0) {
            return true;
        }
    }

    if (m_api->commandString != nullptr) {
        const auto command = QStringLiteral("no-osd set %1 %2")
                                 .arg(QString::fromUtf8(name))
                                 .arg(secondsOptionValue(value));
        const auto result = m_api->commandString(m_state->handle, command.toUtf8().constData());
        if (result >= 0) {
            return true;
        }
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("mpv"),
        QStringLiteral("Failed to update runtime double option '%1'.").arg(QString::fromUtf8(name)));
    return false;
}

bool MpvPlayer::setRuntimeInt64Option(const char *name, const qint64 value)
{
    if (!m_state->initialized || m_state->handle == nullptr) {
        return false;
    }

    if (m_api->setProperty != nullptr) {
        auto numericValue = value;
        const auto result = m_api->setProperty(m_state->handle, name, kMpvFormatInt64, &numericValue);
        if (result >= 0) {
            return true;
        }
    }

    if (m_api->commandString != nullptr) {
        const auto command = QStringLiteral("no-osd set %1 %2")
                                 .arg(QString::fromUtf8(name))
                                 .arg(QString::number(value));
        const auto result = m_api->commandString(m_state->handle, command.toUtf8().constData());
        if (result >= 0) {
            return true;
        }
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("mpv"),
        QStringLiteral("Failed to update runtime int64 option '%1'.").arg(QString::fromUtf8(name)));
    return false;
}

void MpvPlayer::refreshCachedTelemetryFast()
{
    CachedTelemetry telemetry;
    {
        QMutexLocker locker(&m_state->mutex);
        telemetry = m_cachedTelemetry;
    }

    telemetry.positionSeconds = propertyDouble("time-pos").value_or(-1.0);
    telemetry.seekable = propertyFlag("seekable");
    telemetry.pauseState = propertyFlag("pause");
    telemetry.bufferingState = propertyFlag("paused-for-cache");
    telemetry.volumePercent = propertyDouble("volume");
    telemetry.demuxerCacheDurationSeconds = propertyDouble("demuxer-cache-duration");
    telemetry.demuxerSeekableRangeSeconds = propertyDemuxerSeekableRangeSeconds();
    telemetry.cacheSpeedBytesPerSecond = propertyDouble("cache-speed");

    QMutexLocker locker(&m_state->mutex);
    m_cachedTelemetry = std::move(telemetry);
}

void MpvPlayer::refreshCachedTelemetrySlow(const bool refreshTracks)
{
    CachedTelemetry telemetry;
    {
        QMutexLocker locker(&m_state->mutex);
        telemetry = m_cachedTelemetry;
    }

    telemetry.videoWidth = propertyInt("width");
    telemetry.videoHeight = propertyInt("height");
    telemetry.videoCodec = propertyString("current-tracks/video/codec");
    telemetry.audioCodec = propertyString("current-tracks/audio/codec");
    telemetry.videoBitrateBitsPerSecond = propertyDouble("video-bitrate");
    telemetry.audioBitrateBitsPerSecond = propertyDouble("audio-bitrate");
    telemetry.displayedVideoFramePtsSeconds = propertyNodeDoubleField("video-frame-info", "pts");
    telemetry.estimatedFrameRateFps = propertyDouble("estimated-vf-fps");
    if (!telemetry.estimatedFrameRateFps.has_value()
        || !std::isfinite(telemetry.estimatedFrameRateFps.value())
        || telemetry.estimatedFrameRateFps.value() <= 0.0) {
        telemetry.estimatedFrameRateFps = propertyDouble("container-fps");
    }
    telemetry.sourceFrameRateFps = propertyDouble("container-fps");
    if (telemetry.sourceFrameRateFps.has_value()
        && (!std::isfinite(telemetry.sourceFrameRateFps.value()) || telemetry.sourceFrameRateFps.value() <= 0.0)) {
        telemetry.sourceFrameRateFps = std::nullopt;
    }
    telemetry.droppedFrameCount = queryDroppedFrameCount();

    if (refreshTracks) {
        telemetry.trackList = queryTrackList();
    }

    QMutexLocker locker(&m_state->mutex);
    m_cachedTelemetry = std::move(telemetry);
}

QVariantList MpvPlayer::queryTrackList() const
{
    const auto count = propertyInt("track-list/count");
    if (!count.has_value()) {
        return {};
    }

    QVariantList result;
    for (int i = 0; i < count.value(); ++i) {
        const auto typeKey = QStringLiteral("track-list/%1/type").arg(i).toUtf8();
        const auto idKey = QStringLiteral("track-list/%1/id").arg(i).toUtf8();
        const auto titleKey = QStringLiteral("track-list/%1/title").arg(i).toUtf8();
        const auto langKey = QStringLiteral("track-list/%1/lang").arg(i).toUtf8();
        const auto selectedKey = QStringLiteral("track-list/%1/selected").arg(i).toUtf8();
        const auto defaultKey = QStringLiteral("track-list/%1/default").arg(i).toUtf8();
        const auto type = propertyString(typeKey.constData());
        const auto id = propertyInt(idKey.constData());
        if (!type.has_value() || !id.has_value()) {
            continue;
        }

        QVariantMap track;
        track[QStringLiteral("type")] = type.value();
        track[QStringLiteral("id")] = id.value();
        track[QStringLiteral("title")] = propertyString(titleKey.constData()).value_or(QString {});
        track[QStringLiteral("lang")] = propertyString(langKey.constData()).value_or(QString {});
        track[QStringLiteral("selected")] = propertyFlag(selectedKey.constData()).value_or(false);
        track[QStringLiteral("default")] = propertyFlag(defaultKey.constData()).value_or(false);
        result.append(track);
    }

    return result;
}

std::optional<int> MpvPlayer::queryDroppedFrameCount() const
{
    const auto voDropped = propertyInt("vo-drop-frame-count");
    const auto decoderDropped = propertyInt("decoder-frame-drop-count");
    if (voDropped.has_value() && decoderDropped.has_value()) {
        return std::max(0, voDropped.value()) + std::max(0, decoderDropped.value());
    }
    if (voDropped.has_value()) {
        return std::max(0, voDropped.value());
    }
    if (decoderDropped.has_value()) {
        return std::max(0, decoderDropped.value());
    }
    if (const auto dropped = propertyInt("drop-frame-count"); dropped.has_value()) {
        return std::max(0, dropped.value());
    }
    return std::nullopt;
}

double MpvPlayer::position() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.positionSeconds;
}

std::optional<bool> MpvPlayer::seekable() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.seekable;
}

std::optional<bool> MpvPlayer::pauseState() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.pauseState;
}

std::optional<bool> MpvPlayer::bufferingState() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.bufferingState;
}

std::optional<double> MpvPlayer::volumePercent() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.volumePercent;
}

std::optional<double> MpvPlayer::demuxerCacheDurationSeconds() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.demuxerCacheDurationSeconds;
}

std::optional<std::pair<double, double>> MpvPlayer::demuxerSeekableRangeSeconds() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.demuxerSeekableRangeSeconds;
}

std::optional<double> MpvPlayer::cacheSpeedBytesPerSecond() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.cacheSpeedBytesPerSecond;
}

std::optional<bool> MpvPlayer::demuxerCacheReaderEof() const
{
    return propertyNodeBoolField("demuxer-cache-state", "eof");
}

double MpvPlayer::bufferTargetSeconds() const
{
    return m_bufferSeconds;
}

std::optional<int> MpvPlayer::videoWidth() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.videoWidth;
}

std::optional<int> MpvPlayer::videoHeight() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.videoHeight;
}

std::optional<QString> MpvPlayer::videoCodec() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.videoCodec;
}

std::optional<QString> MpvPlayer::audioCodec() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.audioCodec;
}

std::optional<double> MpvPlayer::videoBitrateBitsPerSecond() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.videoBitrateBitsPerSecond;
}

std::optional<double> MpvPlayer::audioBitrateBitsPerSecond() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.audioBitrateBitsPerSecond;
}

std::optional<double> MpvPlayer::displayedVideoFramePtsSeconds() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.displayedVideoFramePtsSeconds;
}

std::optional<double> MpvPlayer::estimatedFrameRateFps() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.estimatedFrameRateFps;
}

std::optional<double> MpvPlayer::sourceFrameRateFps() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.sourceFrameRateFps;
}

bool MpvPlayer::deinterlaceEnabled() const
{
    return m_deinterlaceEnabled;
}

std::optional<int> MpvPlayer::droppedFrameCount() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.droppedFrameCount;
}

std::optional<bool> MpvPlayer::isInterlaced() const
{
    // Returns the cached source scan type captured by detectAndApplyDeinterlace()
    // before any deinterlacing filter was applied. This reflects the true source,
    // not the post-filter output.
    return m_sourceInterlaced;
}

QVariantList MpvPlayer::trackList() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.trackList;
}

void MpvPlayer::selectAudioTrack(const int id)
{
    if (!ensureInitialized()) {
        return;
    }
    qint64 v = id;
    m_api->setProperty(m_state->handle, "aid", kMpvFormatInt64, &v);
    m_trackListRefreshPending.store(true);
    m_slowTelemetryRefreshPending.store(true);
}

void MpvPlayer::selectSubtitleTrack(const int id)
{
    if (!ensureInitialized()) {
        return;
    }
    if (id == 0) {
        const char *no = "no";
        m_api->setProperty(m_state->handle, "sid", kMpvFormatString, static_cast<void *>(&no));
    } else {
        qint64 v = id;
        m_api->setProperty(m_state->handle, "sid", kMpvFormatInt64, &v);
    }
    m_trackListRefreshPending.store(true);
    m_slowTelemetryRefreshPending.store(true);
}

void MpvPlayer::detectAndApplyDeinterlace()
{
    // Keep scan-type telemetry for diagnostics/debug overlay.
    m_sourceInterlaced = propertyFlag("video-frame-info/interlaced");

    QMutexLocker locker(&m_state->mutex);
    if (!m_state->initialized || m_state->handle == nullptr) {
        return;
    }

    const char *deinterlaceValue = m_deinterlaceEnabled ? "yes" : "no";
    const auto setResult =
        m_api->setProperty(m_state->handle, "deinterlace", kMpvFormatString, static_cast<void *>(&deinterlaceValue));
    if (setResult < 0) {
        Core::DebugLogger::instance().log(
            QStringLiteral("mpv"),
            QStringLiteral("Failed to update deinterlace property: %1")
                .arg(QString::fromUtf8(m_api->errorString(setResult))));
        return;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("mpv"),
        QStringLiteral("Applied deinterlace setting on video reconfig: mode=%1 source-scan=%2.")
            .arg(m_deinterlaceEnabled ? QStringLiteral("on") : QStringLiteral("off"))
            .arg(!m_sourceInterlaced.has_value()
                     ? QStringLiteral("unknown")
                     : (m_sourceInterlaced.value() ? QStringLiteral("interlaced") : QStringLiteral("progressive"))));
}

void MpvPlayer::renderToFbo(const int fbo, const int width, const int height)
{
#if defined(Q_OS_WIN)
    const auto renderStartMs = monotonicNowMs();
#endif
    if (!ensureRenderContext()) {
        return;
    }

    QMutexLocker locker(&m_state->mutex);
    if (m_state->renderContext == nullptr) {
        return;
    }

    if (m_api->renderContextUpdate != nullptr) {
        m_api->renderContextUpdate(m_state->renderContext);
    }

    mpv_opengl_fbo renderTarget { fbo, width, height, 0 };
    int flipY = 0;
    mpv_render_param params[] = {
        { kRenderParamOpenGlFbo, &renderTarget },
        { kRenderParamFlipY, &flipY },
        { kRenderParamInvalid, nullptr }
    };

    const auto renderCode = m_api->renderContextRender(m_state->renderContext, params);
    if (renderCode < 0) {
        m_diagnostics = QStringLiteral("mpv_render_context_render failed: %1")
                            .arg(QString::fromUtf8(m_api->errorString(renderCode)));
        Core::DebugLogger::instance().log(QStringLiteral("mpv"), m_diagnostics);
        emit errorOccurred(m_diagnostics);
        return;
    }

    const auto renderCount = ++m_renderCount;
    if (renderCount <= 5) {
        unsigned char sample[4] { 0, 0, 0, 0 };
        if (auto *context = QOpenGLContext::currentContext()) {
            if (auto *functions = context->functions()) {
                functions->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fbo));
                functions->glReadPixels(
                    std::max(0, width / 2),
                    std::max(0, height / 2),
                    1,
                    1,
                    GL_RGBA,
                    GL_UNSIGNED_BYTE,
                    sample);
            }
        }
        Core::DebugLogger::instance().log(
            QStringLiteral("mpv"),
            QStringLiteral("Rendered frame %1 into fbo=%2 size=%3x%4 sample=%5,%6,%7,%8.")
                .arg(renderCount)
                .arg(fbo)
                .arg(width)
                .arg(height)
                .arg(sample[0])
                .arg(sample[1])
                .arg(sample[2])
                .arg(sample[3]));
    }

#if defined(Q_OS_WIN)
    const auto nowMs = monotonicNowMs();
    const auto previousRenderMs = m_lastRenderTimestampMs.exchange(nowMs);
    if (m_windowsPacingDiagEnabled) {
        m_renderCallsSinceLastStats.fetch_add(1);
        const auto gapMs = previousRenderMs >= 0 ? nowMs - previousRenderMs : -1;
        const auto lastLoggedMs = m_lastPacingDiagnosticLogTimestampMs.load();
        if (gapMs >= 80 && (lastLoggedMs < 0 || nowMs - lastLoggedMs >= 1000)) {
            m_lastPacingDiagnosticLogTimestampMs.store(nowMs);
            Core::DebugLogger::instance().log(
                QStringLiteral("mpv"),
                QStringLiteral("Render cadence gap detected: %1 ms between renderToFbo calls.")
                    .arg(gapMs));
        }
        const auto renderMicros =
            static_cast<quint64>(std::max<qint64>(0, (nowMs - renderStartMs) * 1000));
        m_renderTimeTotalMicrosSinceLastStats.fetch_add(renderMicros);
        auto currentMaxMicros = m_renderTimeMaxMicrosSinceLastStats.load();
        const auto renderMicrosInt =
            static_cast<unsigned int>(std::min<quint64>(renderMicros, std::numeric_limits<unsigned int>::max()));
        while (currentMaxMicros < renderMicrosInt
               && !m_renderTimeMaxMicrosSinceLastStats.compare_exchange_weak(currentMaxMicros, renderMicrosInt)) {
        }
    }
#endif
}

void MpvPlayer::reportSwap()
{
    QMutexLocker locker(&m_state->mutex);
    if (m_state->renderContext != nullptr) {
        m_api->renderContextReportSwap(m_state->renderContext);
    }
}

qint64 MpvPlayer::lastRenderTimestampMs() const
{
    return m_lastRenderTimestampMs.load();
}

qint64 MpvPlayer::lastRenderUpdateTimestampMs() const
{
    return m_lastRenderUpdateTimestampMs.load();
}

void MpvPlayer::unload()
{
    stopStreamRecord();
    m_eventThreadRunning = false;
    if (m_eventThread != nullptr && m_eventThread->joinable()) {
        m_eventThread->join();
    }
    m_eventThread.reset();

    QMutexLocker locker(&m_state->mutex);
    if (m_state->renderContext != nullptr) {
        m_api->renderContextFree(m_state->renderContext);
        m_state->renderContext = nullptr;
    }

    if (m_state->handle != nullptr) {
        m_api->terminateDestroy(m_state->handle);
        m_state->handle = nullptr;
    }

    m_state->initialized = false;
    m_cachedTelemetry = {};
    m_trackListRefreshPending.store(false);
    m_slowTelemetryRefreshPending.store(false);
    // Keep libmpv loaded for process lifetime. Repeated unload/reload around player
    // teardown has been a crash source in mpv Lua/script worker threads.
}

bool MpvPlayer::loadApi()
{
    if (m_api->library.isLoaded()) {
        return true;
    }

    m_api->library.setFileName(resolvedLibraryPath());
    if (!m_api->library.load()) {
        m_diagnostics = QStringLiteral("Failed to load %1: %2")
                            .arg(m_api->library.fileName(), m_api->library.errorString());
        return false;
    }

    const auto resolve = [&](auto &target, const char *symbol) {
        target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(m_api->library.resolve(symbol));
        return target != nullptr;
    };

    const auto ok = resolve(m_api->create, "mpv_create")
        && resolve(m_api->initialize, "mpv_initialize")
        && resolve(m_api->terminateDestroy, "mpv_terminate_destroy")
        && resolve(m_api->setOptionString, "mpv_set_option_string")
        && resolve(m_api->setProperty, "mpv_set_property")
        && resolve(m_api->getProperty, "mpv_get_property")
        && resolve(m_api->observeProperty, "mpv_observe_property")
        && resolve(m_api->requestLogMessages, "mpv_request_log_messages")
        && resolve(m_api->waitEvent, "mpv_wait_event")
        && resolve(m_api->errorString, "mpv_error_string")
        && resolve(m_api->renderContextCreate, "mpv_render_context_create")
        && resolve(m_api->renderContextFree, "mpv_render_context_free")
        && resolve(m_api->renderContextSetUpdateCallback, "mpv_render_context_set_update_callback")
        && resolve(m_api->renderContextRender, "mpv_render_context_render")
        && resolve(m_api->renderContextUpdate, "mpv_render_context_update")
        && resolve(m_api->renderContextReportSwap, "mpv_render_context_report_swap");

    if (!ok) {
        m_diagnostics = QStringLiteral("mpv-2.dll is missing required libmpv symbols.");
        Core::DebugLogger::instance().log(QStringLiteral("mpv"), m_diagnostics);
        m_api->library.unload();
        return false;
    }

    resolve(m_api->command, "mpv_command");
    resolve(m_api->commandString, "mpv_command_string");
    resolve(m_api->free, "mpv_free");
    resolve(m_api->freeNodeContents, "mpv_free_node_contents");
    resolve(m_api->streamCbAddRo, "mpv_stream_cb_add_ro");
    if (m_api->command == nullptr && m_api->commandString == nullptr) {
        m_diagnostics = QStringLiteral("mpv-2.dll is missing both mpv_command and mpv_command_string.");
        Core::DebugLogger::instance().log(QStringLiteral("mpv"), m_diagnostics);
        m_api->library.unload();
        return false;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("mpv"),
        QStringLiteral("Loaded libmpv symbols from %1.").arg(m_api->library.fileName()));
    return true;
}

QString MpvPlayer::resolvedLibraryPath() const
{
    if (!m_libraryPath.isEmpty() && QFileInfo::exists(m_libraryPath)) {
        return m_libraryPath;
    }

    auto bundled = QCoreApplication::applicationDirPath() + u'/' + kLibraryName;
    if (QFileInfo::exists(bundled)) {
        return bundled;
    }

    return QString::fromLatin1(kLibraryName);
}

void MpvPlayer::startEventThread()
{
    if (m_eventThreadRunning) {
        return;
    }

    m_eventThreadRunning = true;
    m_trackListRefreshPending.store(true);
    m_slowTelemetryRefreshPending.store(true);
    m_eventThread = std::make_unique<std::thread>([this]() {
        using namespace std::chrono_literals;
        auto refreshTracks = true;
        auto nextFastTelemetryAt = std::chrono::steady_clock::now();
        auto nextSlowTelemetryAt = std::chrono::steady_clock::now();

        while (m_eventThreadRunning.load()) {
            processEvents();

            const auto now = std::chrono::steady_clock::now();
            if (now >= nextFastTelemetryAt) {
                refreshCachedTelemetryFast();
                nextFastTelemetryAt = now + 100ms;
            }

            const auto trackRefreshRequested = refreshTracks || m_trackListRefreshPending.exchange(false);
            const auto slowRefreshRequested =
                m_slowTelemetryRefreshPending.exchange(false) || trackRefreshRequested;
            if (slowRefreshRequested || now >= nextSlowTelemetryAt) {
                refreshCachedTelemetrySlow(trackRefreshRequested);
                refreshTracks = false;
                nextSlowTelemetryAt = now + 1s;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(kEventPollIntervalMs));
        }
    });
}

void MpvPlayer::processEvents()
{
    if (!m_eventThreadRunning) {
        return;
    }

    const auto queueOnOwnerThread = [this](auto fn) {
        QMetaObject::invokeMethod(this, std::move(fn), Qt::QueuedConnection);
    };

    for (;;) {
        mpv_event *event = nullptr;
        if (m_state->handle != nullptr) {
            event = m_api->waitEvent(m_state->handle, 0.0);
        }
        if (event == nullptr || event->event_id == kMpvEventNone) {
            return;
        }

        switch (event->event_id) {
        case kMpvEventLogMessage:
            if (event->data != nullptr) {
                const auto *message = static_cast<mpv_event_log_message *>(event->data);
                const auto prefix = QString::fromUtf8(message->prefix ? message->prefix : "mpv");
                const auto text = QString::fromUtf8(message->text ? message->text : "");
                if (prefix == QStringLiteral("vo/libmpv")
                    && (text.contains(QStringLiteral("mpv_render_context_render() not being called or stuck."))
                        || text.contains(QStringLiteral("mpv_render_report_swap() not being called.")))) {
                    break;
                }
                if (envFlagEnabled("OKILTV_TRACE_MPV")) {
                    Core::DebugLogger::instance().log(
                        QStringLiteral("mpv-log"),
                        QStringLiteral("[%1] %2").arg(prefix, text));
                }
            }
            break;
        case kMpvEventFileLoaded:
            Core::DebugLogger::instance().log(QStringLiteral("mpv"), QStringLiteral("Received MPV_EVENT_FILE_LOADED."));
            m_trackListRefreshPending.store(true);
            m_slowTelemetryRefreshPending.store(true);
            queueOnOwnerThread([this]() { emit fileLoaded(); });
            break;
        case kMpvEventPropertyChange:
            if (event->data != nullptr) {
                const auto *property = static_cast<mpv_event_property *>(event->data);
                if (property->name != nullptr
                    && QByteArray(property->name) == "pause"
                    && property->format == kMpvFormatFlag
                    && property->data != nullptr) {
                    const auto paused = *static_cast<int *>(property->data) != 0;
                    {
                        QMutexLocker locker(&m_state->mutex);
                        m_cachedTelemetry.pauseState = paused;
                    }
                    queueOnOwnerThread([this, paused]() { emit pauseStateChanged(paused); });
                } else if (property->name != nullptr
                    && QByteArray(property->name) == "paused-for-cache"
                    && property->format == kMpvFormatFlag
                    && property->data != nullptr) {
                    const auto buffering = *static_cast<int *>(property->data) != 0;
                    {
                        QMutexLocker locker(&m_state->mutex);
                        m_cachedTelemetry.bufferingState = buffering;
                    }
                    queueOnOwnerThread([this, buffering]() { emit bufferingStateChanged(buffering); });
                }
            }
            break;
        case kMpvEventEndFile: {
            m_sourceInterlaced = std::nullopt;
            const auto reason = (event->data != nullptr)
                ? static_cast<const mpv_event_end_file *>(event->data)->reason
                : kMpvEndFileEof;
            const auto fileError = (event->data != nullptr && reason == kMpvEndFileError)
                ? static_cast<const mpv_event_end_file *>(event->data)->error
                : 0;
            const auto fileErrorText = (m_api->errorString != nullptr && fileError < 0)
                ? QString::fromUtf8(m_api->errorString(fileError))
                : QStringLiteral("unknown mpv error (%1)").arg(fileError);
            Core::DebugLogger::instance().log(
                QStringLiteral("mpv"),
                reason == kMpvEndFileError
                    ? QStringLiteral("Received MPV_EVENT_END_FILE reason=%1 error=%2 (%3).")
                          .arg(reason)
                          .arg(fileError)
                          .arg(fileErrorText)
                    : QStringLiteral("Received MPV_EVENT_END_FILE reason=%1.").arg(reason));
            if (reason == kMpvEndFileStop || reason == kMpvEndFileRedirect || reason == kMpvEndFileQuit) {
                // Intentional stop/replace — not a stream failure, do not disturb PlayerController state.
                queueOnOwnerThread([this]() { emit playbackStopped(); });
                break;
            }
            if (reason == kMpvEndFileError) {
                const auto message = QStringLiteral("Stream error: %1").arg(fileErrorText);
                queueOnOwnerThread([this, message]() { emit errorOccurred(message); });
                break;
            }
            // EOF (0) or unrecognised reason — treat as natural stream end
            queueOnOwnerThread([this]() { emit playbackEnded(); });
            break;
        }
        case kMpvEventVideoReconfig:
            Core::DebugLogger::instance().log(QStringLiteral("mpv"), QStringLiteral("Received MPV_EVENT_VIDEO_RECONFIG."));
            m_trackListRefreshPending.store(true);
            m_slowTelemetryRefreshPending.store(true);
            queueOnOwnerThread([this]() { emit videoReconfigured(); });
            break;
        case kMpvEventAudioReconfig:
            Core::DebugLogger::instance().log(QStringLiteral("mpv"), QStringLiteral("Received MPV_EVENT_AUDIO_RECONFIG."));
            m_trackListRefreshPending.store(true);
            m_slowTelemetryRefreshPending.store(true);
            break;
        case kMpvEventPlaybackRestart:
            Core::DebugLogger::instance().log(QStringLiteral("mpv"), QStringLiteral("Received MPV_EVENT_PLAYBACK_RESTART."));
            m_trackListRefreshPending.store(true);
            m_slowTelemetryRefreshPending.store(true);
            queueOnOwnerThread([this]() { emit playbackRestarted(); });
            break;
        case kMpvEventShutdown:
            Core::DebugLogger::instance().log(QStringLiteral("mpv"), QStringLiteral("Received MPV_EVENT_SHUTDOWN."));
            m_eventThreadRunning = false;
            break;
        default:
            break;
        }
    }
}

void MpvPlayer::requestFrameUpdate()
{
    if (m_updateTarget == nullptr) {
        return;
    }

    const auto updateCount = ++m_renderUpdateCount;
    if (updateCount <= 10) {
        Core::DebugLogger::instance().log(
            QStringLiteral("mpv"),
            QStringLiteral("Render update callback #%1 received.").arg(updateCount));
    }
#if defined(Q_OS_WIN)
    const auto nowMs = monotonicNowMs();
    const auto previousUpdateMs = m_lastRenderUpdateTimestampMs.exchange(nowMs);
    if (m_windowsPacingDiagEnabled) {
        m_renderUpdateCallbacksSinceLastStats.fetch_add(1);
        const auto gapMs = previousUpdateMs >= 0 ? nowMs - previousUpdateMs : -1;
        const auto lastLoggedMs = m_lastPacingDiagnosticLogTimestampMs.load();
        if (gapMs >= 80 && (lastLoggedMs < 0 || nowMs - lastLoggedMs >= 1000)) {
            m_lastPacingDiagnosticLogTimestampMs.store(nowMs);
            Core::DebugLogger::instance().log(
                QStringLiteral("mpv"),
                QStringLiteral("Render update callback gap detected: %1 ms between callbacks.")
                    .arg(gapMs));
        }
    }
#endif
    if (!QMetaObject::invokeMethod(m_updateTarget, "requestUpdateFromMpv", Qt::QueuedConnection)) {
        Core::DebugLogger::instance().log(
            QStringLiteral("mpv"),
            QStringLiteral("Failed to queue requestUpdateFromMpv: player=%1 target=%2.")
                .arg(reinterpret_cast<quintptr>(this), 0, 16)
                .arg(reinterpret_cast<quintptr>(m_updateTarget.data()), 0, 16));
    }
}

void MpvPlayer::logWindowsRenderStats()
{
#if defined(Q_OS_WIN)
    if (!m_windowsPacingDiagEnabled) {
        return;
    }

    const auto nowMs = monotonicNowMs();
    const auto previousLogMs = m_lastWindowsRenderStatsLogTimestampMs.exchange(nowMs);
    const auto elapsedMs = previousLogMs >= 0 ? std::max<qint64>(1, nowMs - previousLogMs) : 1000;
    const auto renderCalls = m_renderCallsSinceLastStats.exchange(0);
    const auto renderUpdateCallbacks = m_renderUpdateCallbacksSinceLastStats.exchange(0);
    const auto totalRenderMicros = m_renderTimeTotalMicrosSinceLastStats.exchange(0);
    const auto maxRenderMicros = m_renderTimeMaxMicrosSinceLastStats.exchange(0);
    const auto averageRenderMicros = renderCalls > 0 ? (totalRenderMicros / renderCalls) : 0;

    Core::DebugLogger::instance().log(
        QStringLiteral("mpv"),
        QStringLiteral(
            "Windows render stats over %1 ms: updateCallbacks=%2 renderCalls=%3 avgRender=%4 us maxRender=%5 us "
            "targetBound=%6.")
            .arg(elapsedMs)
            .arg(renderUpdateCallbacks)
            .arg(renderCalls)
            .arg(averageRenderMicros)
            .arg(maxRenderMicros)
            .arg(m_updateTarget != nullptr ? QStringLiteral("true") : QStringLiteral("false")));
#endif
}

std::optional<double> MpvPlayer::propertyDouble(const char *name) const
{
    QMutexLocker locker(&m_state->mutex);
    if (!m_state->initialized || m_state->handle == nullptr) {
        return std::nullopt;
    }

    double value = 0.0;
    if (m_api->getProperty(m_state->handle, name, kMpvFormatDouble, &value) < 0) {
        return std::nullopt;
    }

    return value;
}

std::optional<int> MpvPlayer::propertyInt(const char *name) const
{
    QMutexLocker locker(&m_state->mutex);
    if (!m_state->initialized || m_state->handle == nullptr) {
        return std::nullopt;
    }

    qint64 value = 0;
    if (m_api->getProperty(m_state->handle, name, kMpvFormatInt64, &value) < 0) {
        return std::nullopt;
    }

    if (value < 0 || value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }

    return static_cast<int>(value);
}

std::optional<bool> MpvPlayer::propertyFlag(const char *name) const
{
    QMutexLocker locker(&m_state->mutex);
    if (!m_state->initialized || m_state->handle == nullptr) {
        return std::nullopt;
    }

    int value = 0;
    if (m_api->getProperty(m_state->handle, name, kMpvFormatFlag, &value) < 0) {
        return std::nullopt;
    }

    return value != 0;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<bool> MpvPlayer::propertyNodeBoolField(const char *prop, const char *key) const
{
    QMutexLocker locker(&m_state->mutex);
    if (!m_state->initialized || m_state->handle == nullptr || m_api->freeNodeContents == nullptr) {
        return std::nullopt;
    }

    mpv_node node {};
    if (m_api->getProperty(m_state->handle, prop, kMpvFormatNode, &node) < 0) {
        return std::nullopt;
    }

    std::optional<bool> result;
    if (node.format == kMpvFormatNodeMap && node.u.list != nullptr) {
        const auto *list = node.u.list;
        for (int i = 0; i < list->num; ++i) {
            if (list->keys[i] != nullptr && qstrcmp(list->keys[i], key) == 0) {
                if (list->values[i].format == kMpvFormatFlag) {
                    result = list->values[i].u.flag != 0;
                }
                break;
            }
        }
    }

    m_api->freeNodeContents(&node);
    return result;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<double> MpvPlayer::propertyNodeDoubleField(const char *prop, const char *key) const
{
    QMutexLocker locker(&m_state->mutex);
    if (!m_state->initialized || m_state->handle == nullptr || m_api->freeNodeContents == nullptr) {
        return std::nullopt;
    }

    mpv_node node {};
    if (m_api->getProperty(m_state->handle, prop, kMpvFormatNode, &node) < 0) {
        return std::nullopt;
    }

    std::optional<double> result;
    if (node.format == kMpvFormatNodeMap && node.u.list != nullptr) {
        const auto *list = node.u.list;
        for (int i = 0; i < list->num; ++i) {
            if (list->keys[i] == nullptr || qstrcmp(list->keys[i], key) != 0) {
                continue;
            }

            if (list->values[i].format == kMpvFormatDouble) {
                result = list->values[i].u.double_;
            } else if (list->values[i].format == kMpvFormatInt64) {
                result = static_cast<double>(list->values[i].u.int64);
            }
            break;
        }
    }

    m_api->freeNodeContents(&node);
    return result;
}

std::optional<std::pair<double, double>> MpvPlayer::propertyDemuxerSeekableRangeSeconds() const
{
    QMutexLocker locker(&m_state->mutex);
    if (!m_state->initialized || m_state->handle == nullptr || m_api->freeNodeContents == nullptr) {
        return std::nullopt;
    }

    mpv_node stateNode {};
    if (m_api->getProperty(m_state->handle, "demuxer-cache-state", kMpvFormatNode, &stateNode) < 0) {
        return std::nullopt;
    }

    auto freeAndReturn = [&](const std::optional<std::pair<double, double>> result)
        -> std::optional<std::pair<double, double>> {
        m_api->freeNodeContents(&stateNode);
        return result;
    };

    if (stateNode.format != kMpvFormatNodeMap || stateNode.u.list == nullptr) {
        return freeAndReturn(std::nullopt);
    }

    const auto currentPositionSeconds = m_cachedTelemetry.positionSeconds;
    if (!std::isfinite(currentPositionSeconds) || currentPositionSeconds < 0.0) {
        return freeAndReturn(std::nullopt);
    }

    const mpv_node_list *stateMap = stateNode.u.list;
    const mpv_node *rangesNode = nullptr;
    for (int i = 0; i < stateMap->num; ++i) {
        if (stateMap->keys[i] != nullptr && qstrcmp(stateMap->keys[i], "seekable-ranges") == 0) {
            rangesNode = &stateMap->values[i];
            break;
        }
    }
    if (rangesNode == nullptr
        || rangesNode->format != kMpvFormatNodeArray
        || rangesNode->u.list == nullptr) {
        return freeAndReturn(std::nullopt);
    }

    std::optional<std::pair<double, double>> matchingRange;
    const auto *ranges = rangesNode->u.list;
    for (int i = 0; i < ranges->num; ++i) {
        const auto &rangeNode = ranges->values[i];
        if (rangeNode.format != kMpvFormatNodeMap || rangeNode.u.list == nullptr) {
            continue;
        }

        std::optional<double> start;
        std::optional<double> end;
        const auto *rangeMap = rangeNode.u.list;
        for (int j = 0; j < rangeMap->num; ++j) {
            if (rangeMap->keys[j] == nullptr) {
                continue;
            }

            const auto &valueNode = rangeMap->values[j];
            if (qstrcmp(rangeMap->keys[j], "start") == 0) {
                if (valueNode.format == kMpvFormatDouble) {
                    start = valueNode.u.double_;
                } else if (valueNode.format == kMpvFormatInt64) {
                    start = static_cast<double>(valueNode.u.int64);
                }
            } else if (qstrcmp(rangeMap->keys[j], "end") == 0) {
                if (valueNode.format == kMpvFormatDouble) {
                    end = valueNode.u.double_;
                } else if (valueNode.format == kMpvFormatInt64) {
                    end = static_cast<double>(valueNode.u.int64);
                }
            }
        }

        if (!start.has_value() || !end.has_value()) {
            continue;
        }
        if (!std::isfinite(start.value()) || !std::isfinite(end.value()) || end.value() < start.value()) {
            continue;
        }
        if (currentPositionSeconds < start.value() || currentPositionSeconds > end.value()) {
            continue;
        }

        matchingRange = std::pair<double, double> {
            std::max(0.0, start.value()),
            std::max(0.0, end.value()),
        };
        break;
    }

    return freeAndReturn(matchingRange);
}

std::optional<QString> MpvPlayer::propertyString(const char *name) const
{
    QMutexLocker locker(&m_state->mutex);
    if (!m_state->initialized || m_state->handle == nullptr || m_api->free == nullptr) {
        return std::nullopt;
    }

    char *value = nullptr;
    if (m_api->getProperty(m_state->handle, name, kMpvFormatString, static_cast<void *>(&value)) < 0
        || value == nullptr) {
        return std::nullopt;
    }

    auto result = QString::fromUtf8(value).trimmed();
    m_api->free(value);
    if (result.isEmpty()) {
        return std::nullopt;
    }

    return result;
}

void *MpvPlayer::getProcAddress(void *ctx, const char *name)
{
    Q_UNUSED(ctx);
    if (auto *context = QOpenGLContext::currentContext()) {
        return reinterpret_cast<void *>(context->getProcAddress(QByteArray(name)));
    }
    return nullptr;
}

void MpvPlayer::onRenderUpdate(void *ctx)
{
    // ctx is always `this` as passed to renderContextSetUpdateCallback.
    // libmpv guarantees no further callbacks after mpv_render_context_free(),
    // which is called in unload() before the object is destroyed.
    Q_ASSERT(ctx != nullptr);
    static_cast<MpvPlayer *>(ctx)->requestFrameUpdate();
}

} // namespace OKILTV::Player
