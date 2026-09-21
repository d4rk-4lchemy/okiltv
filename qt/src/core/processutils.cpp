#include "processutils.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

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
