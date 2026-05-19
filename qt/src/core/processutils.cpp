#include "processutils.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace OKILTV::Core {

QString resolveProcessBinary(const QStringView baseName)
{
    const auto appDir = QCoreApplication::applicationDirPath();
#if defined(Q_OS_WIN)
    const auto fileName = baseName.toString() + QStringLiteral(".exe");
#else
    const auto fileName = baseName.toString();
#endif

    const auto bundledPath = QDir(appDir).filePath(fileName);
    const QFileInfo bundledInfo(bundledPath);
    if (bundledInfo.isFile() && bundledInfo.isExecutable()) {
        return bundledPath;
    }

    const auto pathResolved = QStandardPaths::findExecutable(fileName);
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

} // namespace OKILTV::Core
