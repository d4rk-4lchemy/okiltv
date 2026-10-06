#include "processutils.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QProcess>
#include <QRegularExpression>
#include <cmath>

#if defined(Q_OS_WIN)
#include <QProcess>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>
#endif

namespace OKILTV::Core {

QString resolveProcessBinary(const QStringView baseName)
{
    const auto appDir = QCoreApplication::applicationDirPath();
#if defined(Q_OS_WIN)
    auto fileName = baseName.toString() + QStringLiteral(".exe");
#else
    auto fileName = baseName.toString();
#endif

    auto bundledPath = QDir(appDir).filePath(fileName);
    const QFileInfo bundledInfo(bundledPath);
    if (bundledInfo.isFile() && bundledInfo.isExecutable()) {
        return bundledPath;
    }

    auto pathResolved = QStandardPaths::findExecutable(fileName);
    if (!pathResolved.trimmed().isEmpty()) {
        return pathResolved;
    }

    return fileName;
}

bool processBinaryAvailable(const QStringView baseName)
{
    const QFileInfo resolvedInfo(resolveProcessBinary(baseName));
    return resolvedInfo.isFile() && resolvedInfo.isExecutable();
}

bool ffmpegToolsAvailable()
{
    return processBinaryAvailable(QStringLiteral("ffmpeg"))
        && processBinaryAvailable(QStringLiteral("ffprobe"));
}

std::optional<double> probeMediaDurationSeconds(const QString &path, QString *errorText)
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

    const auto ffprobe = resolveProcessBinary(QStringLiteral("ffprobe"));
    QProcess probe;
    probe.setProcessChannelMode(QProcess::MergedChannels);
    const QStringList args {
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-show_entries"), QStringLiteral("format=duration"),
        QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1:nokey=1"),
        normalizedPath
    };

    probe.start(ffprobe, args);
    if (!probe.waitForStarted(3000)) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("failed-to-start");
        }
        return std::nullopt;
    }

    if (!probe.waitForFinished(10000)) {
        probe.kill();
        probe.waitForFinished(1200);
        if (errorText != nullptr) {
            *errorText = QStringLiteral("timeout");
        }
        return std::nullopt;
    }

    if (probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0) {
        if (errorText) *errorText = QStringLiteral("probe-failed");
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
bool recordingRemuxValid(const QString &sourcePath, const QString &outputPath)
{
    if (QFileInfo(outputPath).size() <= 0) return false;
    const auto sourceDuration = probeMediaDurationSeconds(sourcePath);
    const auto outputDuration = probeMediaDurationSeconds(outputPath);
    return sourceDuration && outputDuration && *sourceDuration > 0 && *outputDuration > 0
        && std::abs(*sourceDuration - *outputDuration) <= 1.0;
}

#if defined(Q_OS_WIN)
struct WindowsProcessJob::State
{
    HANDLE job { nullptr };
    STARTUPINFOEXW startupInfo {};
    std::vector<unsigned char> attributes;

    ~State()
    {
        if (startupInfo.lpAttributeList != nullptr) {
            DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
        }
        close();
    }

    void close()
    {
        if (job != nullptr) {
            CloseHandle(job);
            job = nullptr;
        }
    }
};

namespace {
bool jobError(QString *errorText, const char *operation)
{
    const auto code = GetLastError();
    if (errorText != nullptr) {
        *errorText = QStringLiteral("%1 failed (Windows error %2)")
                         .arg(QString::fromLatin1(operation)).arg(code);
    }
    return false;
}
} // namespace

WindowsProcessJob::WindowsProcessJob() = default;

WindowsProcessJob::~WindowsProcessJob()
{
    if (m_state) {
        m_state->close();
    }
}

bool WindowsProcessJob::configure(QProcess &process, QString *errorText)
{
    if (m_state || process.state() != QProcess::NotRunning) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Recording job must be configured once, before process start");
        }
        return false;
    }
    auto state = std::make_shared<State>();
    // Unnamed and non-inheritable: children must not keep the kill-on-close handle alive.
    state->job = CreateJobObjectW(nullptr, nullptr);
    if (state->job == nullptr) {
        return jobError(errorText, "CreateJobObjectW");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(state->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        return jobError(errorText, "SetInformationJobObject");
    }

    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (size == 0) {
        return jobError(errorText, "InitializeProcThreadAttributeList(size)");
    }
    state->attributes.resize(size);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(state->attributes.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
        return jobError(errorText, "InitializeProcThreadAttributeList");
    }
    state->startupInfo.lpAttributeList = attributes;
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                   static_cast<void *>(&state->job), sizeof(state->job), nullptr, nullptr)) {
        return jobError(errorText, "UpdateProcThreadAttribute(job)");
    }

    // Windows 10+: assignment happens atomically in CreateProcess, before any child
    // code executes. Failure is a QProcess FailedToStart, never an unowned process.
    // Retain attribute storage until QProcess releases its modifier.
    process.setCreateProcessArgumentsModifier([state](QProcess::CreateProcessArguments *args) {
        state->startupInfo.StartupInfo = *args->startupInfo;
        state->startupInfo.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        args->startupInfo = &state->startupInfo.StartupInfo;
        args->flags |= EXTENDED_STARTUPINFO_PRESENT;
    });
    m_state = std::move(state);
    return true;
}

bool WindowsProcessJob::terminate(QString *errorText) const
{
    if (!m_state || m_state->job == nullptr) {
        return true;
    }
    return TerminateJobObject(m_state->job, 1) || jobError(errorText, "TerminateJobObject");
}

bool WindowsProcessJob::hasActiveProcesses(QString *errorText) const
{
    if (!m_state || m_state->job == nullptr) {
        return false;
    }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting {};
    if (!QueryInformationJobObject(m_state->job, JobObjectBasicAccountingInformation,
                                   &accounting, sizeof(accounting), nullptr)) {
        jobError(errorText, "QueryInformationJobObject");
        return true; // Never authorize remux/deletion while process state is unknown.
    }
    return accounting.ActiveProcesses != 0;
}
#endif

} // namespace OKILTV::Core
