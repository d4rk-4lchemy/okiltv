#include "mpvplayer.h"

#include "../core/models.h"
#include "../core/debuglogger.h"
#include "../core/trackpreferences.h"
#include "catchupstreamsession.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>
#include <QLibrary>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
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
constexpr int kMpvEventSetPropertyReply = 4;
constexpr int kMpvEventCommandReply = 5;
constexpr int kMpvEventPropertyChange = 22; // MPV_EVENT_PROPERTY_CHANGE; 16 is CLIENT_MESSAGE.
constexpr int kMpvEventEndFile = 7;
constexpr int kMpvEventFileLoaded = 8;
constexpr int kMpvEventVideoReconfig = 17;
constexpr int kMpvEventAudioReconfig = 18;
constexpr int kMpvEventPlaybackRestart = 21;
constexpr quint64 kVolumePropertyRequest = 10;
constexpr quint64 kAudioPropertyRequest = 11;
constexpr quint64 kSubtitlePropertyRequest = 12;

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
constexpr qint64 kDemuxerMaxBytesCeil = 8LL * 1024 * kMiB;
constexpr qint64 kSteadyStateDemuxerMaxBytesFloor = 8 * kMiB;
constexpr qint64 kSteadyStateDemuxerMaxBackBytesFloor = 8 * kMiB;
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

struct CatchupSessionHandle {
    std::shared_ptr<CatchupStreamSession> session;
    quint64 generation;
};

qint64 catchupStreamRead(void *cookie, char *buffer, const quint64 maxBytes)
{
    auto *handle = static_cast<CatchupSessionHandle *>(cookie);
    if (handle == nullptr || !handle->session) {
        return -1;
    }
    return handle->session->read(handle->generation, buffer, maxBytes);
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
    if (handle->session) {
        handle->session->cancelRead(handle->generation);
    }
    delete handle;
}

void catchupStreamCancel(void *cookie)
{
    auto *handle = static_cast<CatchupSessionHandle *>(cookie);
    if (handle != nullptr && handle->session) {
        handle->session->cancelRead(handle->generation);
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
    info->cookie = new CatchupSessionHandle {session, session->readGeneration()};
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
    using CommandAsyncFn = int (*)(mpv_handle *, quint64, const char *const[]);
    using WaitAsyncRequestsFn = void (*)(mpv_handle *);
    using CommandStringFn = int (*)(mpv_handle *, const char *);
    using SetPropertyFn = int (*)(mpv_handle *, const char *, int, void *);
    using SetPropertyAsyncFn = int (*)(mpv_handle *, quint64, const char *, int, void *);
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
    CommandAsyncFn commandAsync = nullptr;
    WaitAsyncRequestsFn waitAsyncRequests = nullptr;
    CommandStringFn commandString = nullptr;
    SetPropertyFn setProperty = nullptr;
    SetPropertyAsyncFn setPropertyAsync = nullptr;
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

void MpvPlayer::configurePicturePreset(const QString &preset)
{
    QMutexLocker locker(&m_state->mutex);
    m_picturePreset = Core::normalizePlayerPicturePreset(preset);
    if (m_state->initialized) {
        applyPicturePresetLocked();
    }
}

void MpvPlayer::applyPicturePresetLocked()
{
    QString shaderPath;
    if (m_picturePreset != QStringLiteral("standard")) {
        if (!m_pictureShaderDirectory) {
            m_pictureShaderDirectory = std::make_unique<QTemporaryDir>();
        }
        if (!m_pictureShaderDirectory->isValid()) {
            Core::DebugLogger::instance().log(QStringLiteral("mpv"), QStringLiteral("Cannot create picture preset shader directory."));
            return;
        }
        shaderPath = m_pictureShaderDirectory->filePath(m_picturePreset + QStringLiteral(".glsl"));
        if (!QFile::exists(shaderPath)) {
            // Gentle display-referred RGB grading, after mpv's scaling and tone mapping.
            QString gains = QStringLiteral("1.0, 1.0, 1.0");
            double saturation = 1.0;
            double contrast = 1.0;
            double gamma = 1.0;
            if (m_picturePreset == QStringLiteral("warm")) {
                gains = QStringLiteral("1.0, 0.97, 0.90");
            } else if (m_picturePreset == QStringLiteral("cold")) {
                gains = QStringLiteral("0.91, 0.97, 1.0");
            } else if (m_picturePreset == QStringLiteral("movie")) {
                gains = QStringLiteral("1.0, 0.985, 0.95");
                saturation = 0.94;
                contrast = 1.03;
                gamma = 1.04;
            } else if (m_picturePreset == QStringLiteral("vivid")) {
                saturation = 1.16;
                contrast = 1.06;
            } else if (m_picturePreset == QStringLiteral("sport")) {
                saturation = 1.08;
                contrast = 1.03;
                gamma = 0.96;
            }
            const auto shader = QStringLiteral(
                "//!HOOK OUTPUT\n//!BIND HOOKED\n//!DESC OKILTV picture preset\n"
                "vec4 hook() {\n"
                "    vec4 pixel = HOOKED_tex(HOOKED_pos);\n"
                "    vec3 rgb = pixel.rgb * vec3(%1);\n"
                "    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));\n"
                "    rgb = mix(vec3(luma), rgb, %2);\n"
                "    rgb = (rgb - vec3(0.5)) * %3 + vec3(0.5);\n"
                "    pixel.rgb = pow(clamp(rgb, 0.0, 1.0), vec3(%4));\n"
                "    return pixel;\n}\n")
                                    .arg(gains).arg(saturation, 0, 'f', 3)
                                    .arg(contrast, 0, 'f', 3).arg(gamma, 0, 'f', 3).toUtf8();
            QFile file(shaderPath);
            if (!file.open(QIODevice::WriteOnly) || file.write(shader) != shader.size() || !file.flush()) {
                file.close();
                file.remove();
                Core::DebugLogger::instance().log(QStringLiteral("mpv"), QStringLiteral("Cannot write picture preset shader."));
                return;
            }
        }
    }
    if (shaderPath == m_appliedPictureShader) {
        return;
    }
    const auto changeShader = [this](const char *operation, const QString &path) {
        const auto encoded = path.toUtf8();
        const char *args[] = { "change-list", "glsl-shaders", operation, encoded.constData(), nullptr };
        const auto result = m_api->command(m_state->handle, args);
        if (result < 0) {
            Core::DebugLogger::instance().log(QStringLiteral("mpv"),
                QStringLiteral("Failed to update picture preset: %1").arg(QString::fromUtf8(m_api->errorString(result))));
        }
        return result >= 0;
    };
    // Remove only our shader; preserve advanced user shaders and their ordering.
    if (!m_appliedPictureShader.isEmpty()) {
        if (!changeShader("remove", m_appliedPictureShader)) {
            return;
        }
        m_appliedPictureShader.clear();
    }
    if (!shaderPath.isEmpty() && changeShader("append", shaderPath)) {
        m_appliedPictureShader = shaderPath;
    }
}

void MpvPlayer::configureImageSmoothing(const bool enabled)
{
    QMutexLocker locker(&m_state->mutex);
    m_imageSmoothingEnabled = enabled;
    if (!m_state->initialized) {
        return;
    }

    // The OpenGL render API supports negative sharpening as a GPU softening filter.
    // Restore the advanced user option when smoothing is disabled.
    const auto value = (enabled ? QStringLiteral("-0.5")
                               : m_options.value(QStringLiteral("sharpen"), QStringLiteral("0"))).toUtf8();
    const char *propertyValue = value.constData();
    const auto result = m_api->setProperty(
        m_state->handle, "sharpen", kMpvFormatString, static_cast<void *>(&propertyValue));
    if (result < 0) {
        Core::DebugLogger::instance().log(
            QStringLiteral("mpv"),
            QStringLiteral("Failed to update image smoothing: %1").arg(QString::fromUtf8(m_api->errorString(result))));
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
    if (m_startupBufferingStrictMode == enabled && m_liveRefillSeconds == 0.0) {
        return;
    }

    m_startupBufferingStrictMode = enabled;
    m_liveRefillSeconds = 0.0;
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
        0.0,
        normalizedCacheLimitSeconds);
    const auto normalizedMaxBytes = std::clamp(policy.maxBytes, kSteadyStateDemuxerMaxBytesFloor, kDemuxerMaxBytesCeil);
    const auto normalizedMaxBackBytes =
        std::clamp(policy.maxBackBytes, kSteadyStateDemuxerMaxBackBytesFloor, normalizedMaxBytes);

    const auto refill = policy.refillSeconds.has_value() && std::isfinite(*policy.refillSeconds)
        ? std::clamp(*policy.refillSeconds, 0.0, 2.0) : m_liveRefillSeconds;
    const auto changed =
        refill != m_liveRefillSeconds
        || std::abs(m_steadyStateCacheLimitSeconds - normalizedCacheLimitSeconds) > 0.0001
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

    bool refillOk = true;
    if (policy.refillSeconds.has_value()) {
        int flag = refill > 0.0 ? 1 : 0;
        refillOk = m_api->setProperty != nullptr
            && m_api->setProperty(m_state->handle, "cache-pause", kMpvFormatFlag, &flag) >= 0;
        refillOk = setRuntimeDoubleOption("cache-pause-wait", refill) && refillOk;
        if (refillOk) {
            m_liveRefillSeconds = refill;
        }
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

    return refillOk && readaheadOk && cacheOk && hysteresisOk && maxBytesOk && maxBackBytesOk;
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
    // A system-installed libmpv can be loaded by soname without a file next to
    // the executable. Runtime track selection must also work in that case.
    return m_api->library.isLoaded() || QFileInfo::exists(resolvedLibraryPath())
        || QFileInfo::exists(QCoreApplication::applicationDirPath() + u'/' + kLibraryName);
}

bool MpvPlayer::catchupStreamProtocolAvailable() const
{
    return m_api != nullptr && m_api->streamCbAddRo != nullptr;
}

void MpvPlayer::setRenderUpdateTarget(QObject *target)
{
    if (QThread::currentThread() != thread()) {
        // Called during QQuickFramebufferObject::synchronize(), with the GUI
        // thread blocked. Capture the guard there, then only access our target
        // state on the owner thread, including all callback delivery.
        const QPointer<QObject> guardedTarget(target);
        QMetaObject::invokeMethod(this, [this, guardedTarget]() {
            if (guardedTarget) {
                setRenderUpdateTarget(guardedTarget.data());
            }
        }, Qt::QueuedConnection);
        return;
    }
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
        queueError(m_diagnostics);
        return false;
    }

    m_state->handle = m_api->create();
    if (m_state->handle == nullptr) {
        m_diagnostics = QStringLiteral("mpv_create returned null.");
        Core::DebugLogger::instance().log(QStringLiteral("mpv"), m_diagnostics);
        queueError(m_diagnostics);
        return false;
    }

    const auto applyOption = [&](const char *key, const QString &value) {
        m_api->setOptionString(m_state->handle, key, value.toUtf8().constData());
    };

    applyOption("vo", QStringLiteral("libmpv"));
    applyOption("cache", QStringLiteral("yes"));
    applyOption("idle", QStringLiteral("yes"));
    applyOption("keep-open", QStringLiteral("yes"));
    // IPTV retunes have independent timestamp origins. Recreate the audio output
    // on loadfile replace, just as Stop -> Play does, instead of retaining it via
    // mpv's default weak gapless mode when consecutive streams share a format.
    applyOption("gapless-audio", QStringLiteral("no"));
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
    applyOption("user-agent", m_userAgent.isEmpty() ? Core::defaultPlayerUserAgent() : m_userAgent);
    // Deinterlacing is controlled directly by the user setting. mpv/yadif decides per-frame
    // handling internally; app logic does not gate filter activation by source scan type.
    applyOption("deinterlace", m_deinterlaceEnabled ? QStringLiteral("yes") : QStringLiteral("no"));
    if (m_imageSmoothingEnabled) {
        applyOption("sharpen", QStringLiteral("-0.5"));
    }
    registerCatchupStreamProtocol();

    // Focus/volume can be configured before a standby backend is needed.
    // Restore explicit requests on initialization, including after reconfiguration.
    if (m_volumeConfigured) {
        applyOption("volume", QString::number(m_volumeRequested));
    }
    if (m_audioEnableConfigured) {
        applyOption("aid", m_audioEnabledRequested ? QStringLiteral("auto") : QStringLiteral("no"));
    }

    const auto initCode = m_api->initialize(m_state->handle);
    if (initCode < 0) {
        const auto error = QString::fromUtf8(m_api->errorString(initCode));
        m_diagnostics = QStringLiteral("mpv_initialize failed: %1").arg(error);
        Core::DebugLogger::instance().log(QStringLiteral("mpv"), m_diagnostics);
        m_api->terminateDestroy(m_state->handle);
        m_state->handle = nullptr;
        queueError(m_diagnostics);
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
    m_audioEnableApplied = m_audioEnableConfigured;
    m_appliedPictureShader.clear();
    applyPicturePresetLocked();
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
        queueError(m_diagnostics);
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
        queueError(m_diagnostics);
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
    beginTrackLoad({});
    if (m_stopSubmitted) {
        // libmpv may reorder async commands against a synchronous loadfile.
        // Only reuse of this stopped player needs a fence; closing PiP does not.
        m_api->waitAsyncRequests(m_state->handle);
        m_stopSubmitted = false;
    }
    closeLiveStream(QStringLiteral("channel-switch"));
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
    auto transportUrl = url;
    const QUrl sourceUrl(url);
    bool nativeHttpOptions = false;
    for (auto it = m_options.cbegin(); it != m_options.cend(); ++it) {
        const auto &key = it.key();
        if (key == QStringLiteral("stream-lavf-o") || key == QStringLiteral("cookies")
            || key == QStringLiteral("cookies-file") || key.startsWith(QStringLiteral("tls-"))
            || (key.startsWith(QStringLiteral("http-")) && key != QStringLiteral("http-header-fields"))) {
            nativeHttpOptions = true;
            break;
        }
    }
    if (catchupStreamProtocolAvailable()
        && loadfileOptions.trimmed().isEmpty()
        && !nativeHttpOptions
        && !envFlagEnabled("OKILTV_DISABLE_TS_TIMESTAMP_NORMALIZATION")
        && (sourceUrl.scheme() == QStringLiteral("http") || sourceUrl.scheme() == QStringLiteral("https"))
        && sourceUrl.path().endsWith(QStringLiteral(".ts"), Qt::CaseInsensitive)) {
        CatchupStreamSession::BufferingPolicy policy {};
        policy.queueHighWaterBytes = 2 * kMiB;
        policy.queueLowWaterBytes = kMiB;
        policy.replyReadBufferBytes = 2 * kMiB;
        policy.roleLabel = QStringLiteral("live-mpegts");
        policy.normalizeMpegTsTimestamps = true;
        policy.transferTimeoutMs = static_cast<int>(1000.0
            * effectiveMpvNetworkTimeoutSeconds(m_waitForDataStreamSeconds, m_bufferSeconds));
        const auto userAgent = propertyString("user-agent").value_or(m_userAgent);
        m_liveStream = CatchupStreamSession::create(url,
            CatchupStreamSession::requestHeadersFromOptions(userAgent, m_options), policy);
        m_liveStream->configureMediaPeriods();
        if (!m_liveStream->start()) {
            const auto message = m_liveStream->errorString();
            closeLiveStream(QStringLiteral("start-failed"));
            emit errorOccurred(message);
            return;
        }
        transportUrl = m_liveStream->virtualUrl();
        m_livePeriodTimer.setInterval(100);
        m_livePeriodTimer.disconnect(this);
        connect(&m_livePeriodTimer, &QTimer::timeout, this, [this]() {
            if (propertyFlag("eof-reached").value_or(false)) {
                advanceLiveMediaPeriod();
            }
        });
        m_livePeriodTimer.start();
        Core::DebugLogger::instance().log(QStringLiteral("mpv"),
            QStringLiteral("Live MPEG-TS transport uses a shared dynamic PCR/PTS/DTS origin: %1.").arg(transportUrl));
    } else if (nativeHttpOptions && sourceUrl.path().endsWith(QStringLiteral(".ts"), Qt::CaseInsensitive)) {
        Core::DebugLogger::instance().log(QStringLiteral("mpv"),
            QStringLiteral("Retaining native HTTP transport for custom HTTP/TLS/cookie options."));
    }
    Core::DebugLogger::instance().log(
        QStringLiteral("mpv"),
        loadfileOptions.trimmed().isEmpty()
            ? QStringLiteral("loadfile %1").arg(url)
            : QStringLiteral("loadfile %1 (%2)").arg(url, loadfileOptions));

    m_trackExpectedPath = transportUrl;
    const auto effectiveLoadfileOptions = trackLoadOptions(loadfileOptions);
    int commandCode = 0;
    if (m_api->command != nullptr) {
        const auto urlUtf8 = transportUrl.toUtf8();
        const auto optionsUtf8 = effectiveLoadfileOptions.toUtf8();
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
        const auto command = effectiveLoadfileOptions.trimmed().isEmpty()
            ? QStringLiteral("loadfile %1 replace").arg(escapeArg(transportUrl))
            : QStringLiteral("loadfile %1 replace -1 %2")
                  .arg(escapeArg(transportUrl), escapeArg(effectiveLoadfileOptions));
        commandCode = m_api->commandString(m_state->handle, command.toUtf8().constData());
    }

    if (commandCode < 0) {
        closeLiveStream(QStringLiteral("loadfile-failed"));
        const auto error = QString::fromUtf8(m_api->errorString(commandCode));
        m_diagnostics = QStringLiteral("mpv loadfile failed: %1").arg(error);
        Core::DebugLogger::instance().log(QStringLiteral("mpv"), m_diagnostics);
        emit errorOccurred(m_diagnostics);
    }
}

bool MpvPlayer::advanceLiveMediaPeriod()
{
    if (!m_liveStream || !m_liveStream->nextPeriodBaseSeconds()) {
        return m_livePeriodLoading;
    }
    if (m_pauseRequested || m_livePeriodLoading) {
        return true;
    }
    if (!m_liveStream->advancePeriod()) {
        const auto detail = m_liveStream->errorString();
        closeLiveStream(QStringLiteral("period-advance-failed"));
        // keep-open may never emit END_FILE: explicitly hand real failures
        // back to controller recovery instead of remaining paused at EOF.
        emit errorOccurred(QStringLiteral("Cannot advance live MPEG-TS configuration: %1")
            .arg(detail.isEmpty() ? QStringLiteral("retained media unavailable") : detail));
        return true;
    }
    m_livePeriodLoading = true;
    Core::DebugLogger::instance().log(QStringLiteral("mpv"),
        QStringLiteral("Live MPEG-TS configuration cutover: generation=%1; replacing demuxer/decoders on the same HTTP response.")
            .arg(m_liveStream->readGeneration()));
    beginTrackLoad(m_liveStream->virtualUrl());
    const auto options = trackLoadOptions({});
    const auto command = options.isEmpty()
        ? QStringLiteral("loadfile %1 replace").arg(escapeArg(m_liveStream->virtualUrl()))
        : QStringLiteral("loadfile %1 replace -1 %2").arg(escapeArg(m_liveStream->virtualUrl()), escapeArg(options));
    const auto result = m_api->commandString(m_state->handle, command.toUtf8().constData());
    if (result < 0) {
        closeLiveStream(QStringLiteral("period-load-failed"));
        emit errorOccurred(QStringLiteral("Live configuration cutover failed: %1")
            .arg(QString::fromUtf8(m_api->errorString(result))));
    }
    return true;
}

void MpvPlayer::closeLiveStream(const QString &reason)
{
    m_livePeriodTimer.stop();
    m_livePeriodLoading = false;
    if (!m_liveStream) {
        return;
    }
    m_liveStream->closeProviderConnection(reason);
    m_liveStream->cancelRead();
    // The mpv read callback can still own this session. Keep its last owning
    // reference on the Qt thread so QNetworkAccessManager is destroyed there.
    m_retiredLiveStreams.append(std::move(m_liveStream));
    releaseRetiredLiveStreams();
}

void MpvPlayer::releaseRetiredLiveStreams()
{
    m_retiredLiveStreams.removeIf([](const auto &session) { return session.use_count() == 1; });
    if (!m_retiredLiveStreams.isEmpty() && !m_liveStreamCleanupScheduled) {
        m_liveStreamCleanupScheduled = true;
        QTimer::singleShot(100, this, [this]() {
            m_liveStreamCleanupScheduled = false;
            releaseRetiredLiveStreams();
        });
    }
}

void MpvPlayer::stop()
{
    beginTrackLoad({});
    closeLiveStream(QStringLiteral("stop"));
    stopStreamRecord();
    if (!m_state->initialized) {
        return;
    }

    // Decoder/VO shutdown can wait for rendering. Blocking the GUI thread here
    // also prevents Qt Quick from advancing the other (primary) video surface.
    const char *args[] = { "stop", nullptr };
    const auto result = m_api->commandAsync(m_state->handle, 0, args);
    m_stopSubmitted = m_stopSubmitted || result >= 0;
    if (result < 0) {
        queueError(QStringLiteral("Unable to stop playback: %1")
                       .arg(QString::fromUtf8(m_api->errorString(result))));
    }
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

    m_pauseRequested = !pauseState().value_or(false);
    m_api->commandString(m_state->handle, "cycle pause");
}

void MpvPlayer::setPaused(const bool paused)
{
    m_pauseRequested = paused;
    if (!ensureInitialized()) {
        return;
    }

    int pausedFlag = paused ? 1 : 0;
    m_api->setProperty(m_state->handle, "pause", kMpvFormatFlag, &pausedFlag);
}

void MpvPlayer::setVolume(const int volume)
{
    m_volumeRequested = std::clamp(volume, 0, 100);
    m_volumeConfigured = true;
    if (!m_state->initialized) {
        return;
    }
    auto value = static_cast<double>(m_volumeRequested);
    setPlaybackPropertyAsync("volume", kMpvFormatDouble, &value, kVolumePropertyRequest);
}

void MpvPlayer::setAudioEnabled(const bool enabled)
{
    // Volume/focus changes must not reset an explicitly selected track to "auto".
    if (m_audioEnableApplied && m_audioEnabledRequested == enabled) {
        return;
    }
    if (enabled && !m_audioEnabledRequested) {
        m_restoredTrackTypes.remove(QStringLiteral("audio"));
    }
    m_audioEnabledRequested = enabled;
    m_audioEnableConfigured = true;
    if (!m_state->initialized) {
        return;
    }
    const char *trackSelection = enabled ? "auto" : "no";
    m_audioEnableApplied = setPlaybackPropertyAsync(
        "aid", kMpvFormatString, static_cast<void *>(&trackSelection), kAudioPropertyRequest);
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

void MpvPlayer::seekAbsoluteExact(const double seconds)
{
    if (!ensureInitialized()) {
        return;
    }
    const auto command = QStringLiteral("no-osd seek %1 absolute+exact").arg(seconds, 0, 'f', 3);
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
    telemetry.demuxerSeekableRangeSeconds = propertyDemuxerSeekableRangeSeconds(&telemetry.cacheReadState);
    telemetry.cacheSpeedBytesPerSecond = propertyDouble("cache-speed");

    QMutexLocker locker(&m_state->mutex);
    m_cachedTelemetry = std::move(telemetry);
}

void MpvPlayer::refreshCachedTelemetrySlow(const bool refreshTracks)
{
    const auto trackGeneration = m_trackGeneration.load();
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

    const auto publishTracks = refreshTracks && trackGeneration != 0 && m_tracksLoadedGeneration.load() == trackGeneration;
    const auto tracks = publishTracks ? telemetry.trackList : QVariantList {};
    const auto path = publishTracks ? propertyString("path").value_or(QString {}) : QString {};
    {
        QMutexLocker locker(&m_state->mutex);
        m_cachedTelemetry = std::move(telemetry);
    }
    if (publishTracks) {
        QMetaObject::invokeMethod(this, [this, trackGeneration, path, tracks]() {
            acceptTrackSnapshot(trackGeneration, path, tracks);
        }, Qt::QueuedConnection);
    }
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

std::optional<MpvPlayer::CacheReadState> MpvPlayer::cacheReadState() const
{
    QMutexLocker locker(&m_state->mutex);
    return m_cachedTelemetry.cacheReadState;
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

void MpvPlayer::configureTrackPreferences(const QString &profileId, const QString &channelKey,
                                        const QJsonObject &preferences, const bool discardMissing)
{
    if (m_trackProfileId != profileId || m_trackChannelKey != channelKey) {
        beginTrackLoad({});
        m_trackProfileId = profileId;
        m_trackChannelKey = channelKey;
    }
    for (const auto &type : { QStringLiteral("audio"), QStringLiteral("sub") }) {
        if (m_trackPreferences.value(type) != preferences.value(type)
            || (!m_discardMissingTrackPreferences && discardMissing)) {
            m_restoredTrackTypes.remove(type);
        }
    }
    m_trackPreferences = preferences;
    m_discardMissingTrackPreferences = discardMissing;
    m_trackListRefreshPending.store(true);
}

bool MpvPlayer::managesTrackPreferences() const
{
    return !Core::parseGuid(m_trackProfileId).isNull() && !m_trackChannelKey.isEmpty();
}

void MpvPlayer::beginTrackLoad(const QString &url)
{
    ++m_trackGeneration;
    m_tracksLoadedGeneration.store(0);
    m_readyTrackGeneration = 0;
    m_trackExpectedPath = url;
    m_readyTracks.clear();
    m_defaultTrackIds.clear();
    m_restoredTrackTypes.clear();
    m_pendingTrackChoices.clear();
}

QString MpvPlayer::trackLoadOptions(const QString &options) const
{
    if (!managesTrackPreferences()) {
        return options;
    }
    auto result = options.trimmed();
    for (const auto &name : { QStringLiteral("aid"), QStringLiteral("sid") }) {
        auto value = m_options.value(name, QStringLiteral("auto")).trimmed();
        bool numeric = false;
        value.toInt(&numeric);
        if (!numeric && value != QLatin1String("auto") && value != QLatin1String("no")) {
            value = QStringLiteral("auto");
        }
        if (name == QLatin1String("aid") && !m_audioEnabledRequested) {
            value = QStringLiteral("no");
        }
        if (!result.isEmpty()) {
            result += QLatin1Char(',');
        }
        result += name + QLatin1Char('=') + value;
    }
    return result;
}

void MpvPlayer::updateTrackPreference(const QString &type, const QJsonObject &preference)
{
    if (preference.isEmpty()) {
        m_trackPreferences.remove(type);
    } else {
        m_trackPreferences.insert(type, preference);
    }
    emit trackPreferenceChanged(m_trackProfileId, m_trackChannelKey, type, preference);
}

void MpvPlayer::acceptTrackSnapshot(const quint64 generation, const QString &path, const QVariantList &tracks)
{
    // A cached/queued predecessor snapshot, stop or failed load cannot erase a
    // preference. FILE_LOADED and the current path must both agree with it.
    const auto normalizedPath = [](const QString &value) {
        const QUrl url(value);
        return url.isLocalFile() ? url.toLocalFile() : value;
    };
    if (generation != m_trackGeneration.load() || generation != m_tracksLoadedGeneration.load()
        || m_trackExpectedPath.isEmpty() || normalizedPath(path) != normalizedPath(m_trackExpectedPath) || tracks.isEmpty()) {
        return;
    }
    m_readyTrackGeneration = generation;
    m_readyTracks = tracks;
    emit trackListReady(generation, tracks);
    if (!managesTrackPreferences()) {
        return;
    }
    for (const auto &type : { QStringLiteral("audio"), QStringLiteral("sub") }) {
        const auto selected = Core::selectedTrackId(tracks, type);
        // Audio can be temporarily disabled by multiview ownership. Wait for
        // its automatic selection before capturing a baseline or restoring it.
        if (type == QLatin1String("audio") && (!m_audioEnabledRequested
            || (!m_defaultTrackIds.contains(type) && selected == 0
                && m_options.value(QStringLiteral("aid")) != QLatin1String("no")
                && !Core::trackLayout(tracks, type).isEmpty()))) {
            continue;
        }
        if (!m_defaultTrackIds.contains(type)) {
            m_defaultTrackIds.insert(type, selected);
        }
        const auto pending = m_pendingTrackChoices.constFind(type);
        if (pending != m_pendingTrackChoices.cend()) {
            if (selected == pending->id) {
                const auto preference = pending->preference;
                m_pendingTrackChoices.remove(type);
                m_restoredTrackTypes.insert(type);
                updateTrackPreference(type, preference);
            }
            continue;
        }
        if (m_restoredTrackTypes.contains(type)) {
            continue;
        }
        m_restoredTrackTypes.insert(type);
        const auto preference = m_trackPreferences.value(type).toObject();
        auto target = preference.isEmpty() ? m_defaultTrackIds.value(type, 0)
            : Core::matchTrackPreference(tracks, type, preference);
        if (target < 0) {
            if (m_discardMissingTrackPreferences) {
                updateTrackPreference(type, {});
            }
            target = m_defaultTrackIds.value(type, 0);
        }
        if (target != selected) {
            if (type == QLatin1String("audio")) {
                if (target > 0) {
                    selectAudioTrack(target);
                } else {
                    const char *no = "no";
                    setPlaybackPropertyAsync("aid", kMpvFormatString, static_cast<void *>(&no), kAudioPropertyRequest);
                }
            } else if (type == QLatin1String("sub")) {
                selectSubtitleTrack(target);
            }
        }
    }
}

bool MpvPlayer::prepareRememberedTrack(const QString &type, const int id)
{
    if (!managesTrackPreferences()) {
        return true;
    }
    if (m_readyTrackGeneration == 0 || m_readyTrackGeneration != m_trackGeneration.load()) {
        return false;
    }
    auto preference = Core::makeTrackPreference(m_readyTracks, type, id);
    if (preference.isEmpty()) {
        return false;
    }
    if (id > 0 && m_defaultTrackIds.value(type, -1) == id) {
        preference = {}; // Explicitly choosing the baseline cancels the override.
    }
    m_pendingTrackChoices.insert(type, { id, preference });
    return true;
}

void MpvPlayer::selectAudioTrack(const int id, const bool remember)
{
    if (remember && !prepareRememberedTrack(QStringLiteral("audio"), id)) {
        return;
    }
    qint64 v = id;
    if (setPlaybackPropertyAsync("aid", kMpvFormatInt64, &v, kAudioPropertyRequest)) {
        m_audioEnabledRequested = true;
        m_audioEnableApplied = true;
    } else if (remember) {
        m_pendingTrackChoices.remove(QStringLiteral("audio"));
    }
}

void MpvPlayer::selectSubtitleTrack(const int id, const bool remember)
{
    if (remember && !prepareRememberedTrack(QStringLiteral("sub"), id)) {
        return;
    }
    bool submitted = false;
    if (id == 0) {
        const char *no = "no";
        submitted = setPlaybackPropertyAsync("sid", kMpvFormatString, static_cast<void *>(&no), kSubtitlePropertyRequest);
    } else {
        qint64 v = id;
        submitted = setPlaybackPropertyAsync("sid", kMpvFormatInt64, &v, kSubtitlePropertyRequest);
    }
    if (!submitted && remember) {
        m_pendingTrackChoices.remove(QStringLiteral("sub"));
    }
}

bool MpvPlayer::setPlaybackPropertyAsync(const char *name, const int format, void *value, const quint64 requestId)
{
    if (!ensureInitialized()) {
        return false;
    }
    // Synchronous audio reconfiguration can wait for rendering, while Qt's render
    // thread waits for the UI thread. Queue the request so rendering keeps running.
    const auto result = m_api->setPropertyAsync(m_state->handle, requestId, name, format, value);
    if (result < 0) {
        Core::DebugLogger::instance().log(
            QStringLiteral("mpv"),
            QStringLiteral("Failed to queue property %1: %2")
                .arg(QString::fromUtf8(name), QString::fromUtf8(m_api->errorString(result))));
    }
    return result >= 0;
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
        queueError(m_diagnostics);
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

    const auto nowMs = monotonicNowMs();
    [[maybe_unused]] const auto previousRenderMs = m_lastRenderTimestampMs.exchange(nowMs);
#if defined(Q_OS_WIN)
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
    closeLiveStream(QStringLiteral("player-unload"));
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
    m_retiredLiveStreams.clear();

    m_state->initialized = false;
    m_stopSubmitted = false;
    m_audioEnableApplied = false;
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
        && resolve(m_api->commandAsync, "mpv_command_async")
        && resolve(m_api->waitAsyncRequests, "mpv_wait_async_requests")
        && resolve(m_api->setProperty, "mpv_set_property")
        && resolve(m_api->setPropertyAsync, "mpv_set_property_async")
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

void MpvPlayer::queueError(const QString &message)
{
    // Error handlers may query player state; run them on the owner thread after unlocking.
    QMetaObject::invokeMethod(this, [this, message]() { emit errorOccurred(message); }, Qt::QueuedConnection);
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
        case kMpvEventCommandReply:
            if (event->error < 0) {
                queueError(QStringLiteral("Unable to stop playback: %1")
                               .arg(QString::fromUtf8(m_api->errorString(event->error))));
            }
            break;
        case kMpvEventSetPropertyReply:
            if (event->error < 0) {
                Core::DebugLogger::instance().log(
                    QStringLiteral("mpv"),
                    QStringLiteral("Async playback property request %1 failed: %2")
                        .arg(event->reply_userdata)
                        .arg(QString::fromUtf8(m_api->errorString(event->error))));
            }
            if (event->reply_userdata == kAudioPropertyRequest
                || event->reply_userdata == kSubtitlePropertyRequest) {
                m_trackListRefreshPending.store(true);
                m_slowTelemetryRefreshPending.store(true);
            }
            break;
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
            m_tracksLoadedGeneration.store(m_trackGeneration.load());
            Core::DebugLogger::instance().log(QStringLiteral("mpv"), QStringLiteral("Received MPV_EVENT_FILE_LOADED."));
            m_trackListRefreshPending.store(true);
            m_slowTelemetryRefreshPending.store(true);
            queueOnOwnerThread([this]() {
                if (m_livePeriodLoading) {
                    m_livePeriodLoading = false;
                    // The old item can enter keep-open pause after loadfile was
                    // requested. Restore intent only once the new item is loaded.
                    setPaused(m_pauseRequested);
                    emit liveMediaPeriodChanged();
                }
                emit fileLoaded();
            });
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
            queueOnOwnerThread([this]() { if (!advanceLiveMediaPeriod()) { emit playbackEnded(); } });
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
    const auto updateCount = ++m_renderUpdateCount;
    if (updateCount <= 10) {
        Core::DebugLogger::instance().log(
            QStringLiteral("mpv"),
            QStringLiteral("Render update callback #%1 received.").arg(updateCount));
    }
    const auto nowMs = monotonicNowMs();
    [[maybe_unused]] const auto previousUpdateMs = m_lastRenderUpdateTimestampMs.exchange(nowMs);
#if defined(Q_OS_WIN)
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
    // mpv's callback never reads a QPointer or touches a QQuickItem. Coalesce
    // notifications and resolve the current target on its owning GUI thread.
    if (!m_frameUpdateQueued.exchange(true)) {
        QMetaObject::invokeMethod(this, [this]() {
            m_frameUpdateQueued.store(false);
            if (m_updateTarget) {
                QMetaObject::invokeMethod(m_updateTarget.data(), "requestUpdateFromMpv", Qt::DirectConnection);
            }
        }, Qt::QueuedConnection);
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

std::optional<std::pair<double, double>> MpvPlayer::propertyDemuxerSeekableRangeSeconds(std::optional<CacheReadState> *readState) const
{
    if (readState != nullptr) {
        readState->reset();
    }
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

    if (readState != nullptr) {
        std::optional<double> end;
        std::optional<bool> idle;
        std::optional<bool> eof;
        const auto *fields = stateNode.u.list;
        for (int i = 0; i < fields->num; ++i) {
            if (fields->keys[i] == nullptr) {
                continue;
            }
            const auto &value = fields->values[i];
            if (qstrcmp(fields->keys[i], "cache-end") == 0 && value.format == kMpvFormatDouble) {
                end = value.u.double_;
            } else if (qstrcmp(fields->keys[i], "idle") == 0 && value.format == kMpvFormatFlag) {
                idle = value.u.flag != 0;
            } else if (qstrcmp(fields->keys[i], "eof") == 0 && value.format == kMpvFormatFlag) {
                eof = value.u.flag != 0;
            }
        }
        if (end.has_value() && std::isfinite(*end) && idle.has_value() && eof.has_value()) {
            *readState = CacheReadState { *end, *idle, *eof };
        }
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
