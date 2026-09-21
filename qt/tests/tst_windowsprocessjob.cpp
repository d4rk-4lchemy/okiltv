#include "../src/core/processutils.h"

#include <QCoreApplication>
#include <QProcess>
#include <QScopeGuard>
#include <QTimer>
#include <QtTest>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>

using OKILTV::Core::WindowsProcessJob;

namespace {
// Real child processes exercise inherited membership, rather than mocking Win32.
bool startTree(QProcess &process, qint64 &childPid, const QString &mode = QStringLiteral("--tree"))
{
    process.start(QCoreApplication::applicationFilePath(), { mode });
    if (!process.waitForStarted(5000) || !process.waitForReadyRead(5000)) {
        return false;
    }
    const auto fields = process.readLine().trimmed().split(' ');
    if (fields.size() != 2 || fields.at(1) != "1") {
        return false;
    }
    childPid = fields.at(0).toLongLong();
    return childPid > 0;
}
} // namespace

class WindowsProcessJobTests final : public QObject
{
    Q_OBJECT
private slots:
    void terminateTreeKeepsOtherJob();
    void closingJobKillsDescendants();
    void parentExitDoesNotHideDescendants();
    void ownerCrashKillsChildren();
    void failedStartLeavesEmptyJob();
    void invalidJobPreventsChildStart();
};

void WindowsProcessJobTests::terminateTreeKeepsOtherJob()
{
    WindowsProcessJob job;
    WindowsProcessJob otherJob;
    QProcess process;
    QProcess other;
    QString error;
    QVERIFY2(job.configure(process, &error), qPrintable(error));
    QVERIFY2(otherJob.configure(other, &error), qPrintable(error));
    qint64 childPid = 0;
    qint64 otherChildPid = 0;
    QVERIFY(startTree(process, childPid));
    QVERIFY(startTree(other, otherChildPid));
    QVERIFY(job.hasActiveProcesses());
    QVERIFY(otherJob.hasActiveProcesses());
    QVERIFY2(job.terminate(&error), qPrintable(error));
    QVERIFY(process.waitForFinished(5000));
    QTRY_VERIFY_WITH_TIMEOUT(!job.hasActiveProcesses(), 5000);
    QCOMPARE(other.state(), QProcess::Running);
    QVERIFY(otherJob.hasActiveProcesses());
    QVERIFY(otherJob.terminate());
    QVERIFY(other.waitForFinished(5000));
    QTRY_VERIFY_WITH_TIMEOUT(!otherJob.hasActiveProcesses(), 5000);
}

void WindowsProcessJobTests::closingJobKillsDescendants()
{
    auto job = std::make_unique<WindowsProcessJob>();
    QProcess process;
    QVERIFY(job->configure(process));
    qint64 childPid = 0;
    QVERIFY(startTree(process, childPid));
    const auto child = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(childPid));
    QVERIFY(child != nullptr);
    const auto closeChild = qScopeGuard([child] { CloseHandle(child); });
    job.reset();
    QVERIFY(process.waitForFinished(5000));
    QCOMPARE(WaitForSingleObject(child, 5000), DWORD(WAIT_OBJECT_0));
}

void WindowsProcessJobTests::parentExitDoesNotHideDescendants()
{
    WindowsProcessJob job;
    QProcess process;
    QVERIFY(job.configure(process));
    qint64 childPid = 0;
    QVERIFY(startTree(process, childPid, QStringLiteral("--tree-exit")));
    QVERIFY(process.waitForFinished(5000));
    QVERIFY(job.hasActiveProcesses());
    QVERIFY(job.terminate());
    QTRY_VERIFY_WITH_TIMEOUT(!job.hasActiveProcesses(), 5000);
}

void WindowsProcessJobTests::ownerCrashKillsChildren()
{
    QProcess owner;
    qint64 childPid = 0;
    QVERIFY(startTree(owner, childPid, QStringLiteral("--owner")));
    const auto child = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(childPid));
    QVERIFY(child != nullptr);
    const auto closeChild = qScopeGuard([child] { CloseHandle(child); });
    owner.kill(); // No destructors in the owner: Windows must close its job handle.
    QVERIFY(owner.waitForFinished(5000));
    QCOMPARE(WaitForSingleObject(child, 5000), DWORD(WAIT_OBJECT_0));
}

void WindowsProcessJobTests::failedStartLeavesEmptyJob()
{
    WindowsProcessJob job;
    QProcess process;
    QVERIFY(job.configure(process));
    process.start(QStringLiteral("Z:/nonexistent-okiltv-dvr-test/ffmpeg.exe"), {});
    QVERIFY(!process.waitForStarted(5000));
    QCOMPARE(process.error(), QProcess::FailedToStart);
    QVERIFY(!job.hasActiveProcesses());
    QVERIFY(job.terminate());
}

void WindowsProcessJobTests::invalidJobPreventsChildStart()
{
    QProcess process;
    {
        WindowsProcessJob job;
        QVERIFY(job.configure(process));
    }
    // Simulate an assignment failure: CreateProcess must fail atomically, not
    // launch a process that escapes ownership when the supplied job is invalid.
    process.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--leaf") });
    QVERIFY(!process.waitForStarted(5000));
    QCOMPARE(process.error(), QProcess::FailedToStart);
    QCOMPARE(process.state(), QProcess::NotRunning);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto mode = app.arguments().value(1);
    if (mode == QStringLiteral("--leaf")) {
        QTimer::singleShot(30000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    if (mode == QStringLiteral("--tree") || mode == QStringLiteral("--tree-exit")
        || mode == QStringLiteral("--owner")) {
        WindowsProcessJob job;
        QProcess child;
        qint64 childPid = 0;
        BOOL inJob = FALSE;
        if (mode == QStringLiteral("--owner")) {
            if (!job.configure(child)) {
                return 2;
            }
            child.start(app.applicationFilePath(), { QStringLiteral("--leaf") });
            if (!child.waitForStarted(5000)) {
                return 3;
            }
            childPid = child.processId();
            inJob = TRUE;
        } else {
            IsProcessInJob(GetCurrentProcess(), nullptr, &inJob);
            if (!QProcess::startDetached(app.applicationFilePath(), { QStringLiteral("--leaf") }, {}, &childPid)) {
                return 4;
            }
        }
        std::printf("%lld %d\n", static_cast<long long>(childPid), int(inJob));
        std::fflush(stdout);
        if (mode == QStringLiteral("--tree-exit")) {
            QTimer::singleShot(200, &app, [] { ExitProcess(0); });
        }
        QTimer::singleShot(30000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    WindowsProcessJobTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tst_windowsprocessjob.moc"
