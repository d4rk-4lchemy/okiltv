#pragma once

#include <QString>
#include <QStringView>

#if defined(Q_OS_WIN)
#include <memory>
class QProcess;
#endif

namespace OKILTV::Core {

QString resolveProcessBinary(QStringView baseName);
bool processBinaryAvailable(QStringView baseName);
bool ffmpegToolsAvailable();

#if defined(Q_OS_WIN)
// One job per recording. Configure before start(); never reuse it for another session.
// Closing the owner kills all remaining descendants, including after an app crash.
class WindowsProcessJob final
{
public:
    WindowsProcessJob();
    ~WindowsProcessJob();
    WindowsProcessJob(const WindowsProcessJob &) = delete;
    WindowsProcessJob &operator=(const WindowsProcessJob &) = delete;

    bool configure(QProcess &process, QString *errorText = nullptr);
    bool terminate(QString *errorText = nullptr) const;
    bool hasActiveProcesses(QString *errorText = nullptr) const;

private:
    struct State;
    std::shared_ptr<State> m_state;
};
#endif

} // namespace OKILTV::Core
