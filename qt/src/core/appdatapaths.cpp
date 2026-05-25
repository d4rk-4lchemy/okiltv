#include "appdatapaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace OKILTV::Core {

namespace {

RuntimeContext &runtimeContextStorage()
{
    static RuntimeContext context;
    return context;
}

QString &migrationAttemptedRootStorage()
{
    static QString rootPath;
    return rootPath;
}

QString defaultRootDirectory()
{
    const auto appData = QProcessEnvironment::systemEnvironment().value(QStringLiteral("APPDATA"));
    if (!appData.isEmpty()) {
        return QDir(appData).filePath(QStringLiteral("OKILTV"));
    }

    const auto fallback = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(fallback).filePath(QStringLiteral("OKILTV"));
}

bool isAbsoluteDirectoryPath(const QString &path)
{
    return !path.trimmed().isEmpty() && QDir::isAbsolutePath(path.trimmed());
}

bool copyMissingRecursively(const QString &sourcePath, const QString &targetPath)
{
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists()) {
        return true;
    }

    if (sourceInfo.isDir()) {
        QDir targetDir(targetPath);
        if (!targetDir.exists() && !QDir().mkpath(targetPath)) {
            return false;
        }

        QDir sourceDir(sourcePath);
        const auto entries = sourceDir.entryInfoList(
            QDir::NoDotAndDotDot | QDir::AllEntries,
            QDir::DirsFirst | QDir::Name);
        for (const auto &entry : entries) {
            const auto sourceChildPath = entry.filePath();
            const auto targetChildPath = targetDir.filePath(entry.fileName());
            if (!copyMissingRecursively(sourceChildPath, targetChildPath)) {
                return false;
            }
        }
        return true;
    }

    if (QFileInfo::exists(targetPath)) {
        return true;
    }

    QDir().mkpath(QFileInfo(targetPath).absolutePath());
    return QFile::copy(sourcePath, targetPath);
}

void migrateLegacyAppDataIfNeeded(const QString &targetRootDirectory)
{
    const auto normalizedTargetRoot = QDir::cleanPath(targetRootDirectory);
    auto &attemptedRoot = migrationAttemptedRootStorage();
    if (attemptedRoot == normalizedTargetRoot) {
        return;
    }
    attemptedRoot = normalizedTargetRoot;

    const auto appData = QProcessEnvironment::systemEnvironment().value(QStringLiteral("APPDATA"));
    if (appData.isEmpty()) {
        return;
    }

    const auto legacyRoot = QDir(appData).filePath(QStringLiteral("IptvPlayer"));
    const auto normalizedLegacyRoot = QDir::cleanPath(legacyRoot);
    if (normalizedLegacyRoot == normalizedTargetRoot || !QDir(normalizedLegacyRoot).exists()) {
        return;
    }

    copyMissingRecursively(normalizedLegacyRoot, normalizedTargetRoot);
}

QString rootDirectory()
{
    const auto &context = runtimeContextStorage();
    if (context.launchMode == LaunchMode::Portable && isAbsoluteDirectoryPath(context.dataRootOverride)) {
        return QDir::cleanPath(context.dataRootOverride.trimmed());
    }

    return defaultRootDirectory();
}

} // namespace

void AppDataPaths::initializeRuntime(const RuntimeContext &context)
{
    runtimeContextStorage() = context;
}

void AppDataPaths::resetRuntimeForTests()
{
    runtimeContextStorage() = {};
    migrationAttemptedRootStorage().clear();
}

RuntimeContext AppDataPaths::runtimeContext()
{
    return runtimeContextStorage();
}

QString AppDataPaths::defaultDataDirectory()
{
    return ensureExists(defaultRootDirectory());
}

QString AppDataPaths::dataDirectory()
{
    const auto root = rootDirectory();
    migrateLegacyAppDataIfNeeded(root);
    return ensureExists(root);
}

QString AppDataPaths::debugDumpDirectory()
{
    return ensureExists(QDir(dataDirectory()).filePath(QStringLiteral("debug-dumps")));
}

QString AppDataPaths::settingsFile()
{
    return QDir(dataDirectory()).filePath(QStringLiteral("settings.json"));
}

QString AppDataPaths::sourceSummariesFile()
{
    return QDir(dataDirectory()).filePath(QStringLiteral("source-summaries.json"));
}

QString AppDataPaths::sourcesDirectory()
{
    return ensureExists(QDir(dataDirectory()).filePath(QStringLiteral("sources")));
}

QString AppDataPaths::sourceDetailFile(const QUuid &profileId)
{
    return QDir(sourcesDirectory()).filePath(
        QStringLiteral("%1.json").arg(profileId.toString(QUuid::WithoutBraces).toLower()));
}

QString AppDataPaths::databaseFile()
{
    return QDir(dataDirectory()).filePath(QStringLiteral("iptv.db"));
}

QString AppDataPaths::epgCacheDirectory()
{
    return ensureExists(QDir(dataDirectory()).filePath(QStringLiteral("epg")));
}

QString AppDataPaths::epgCacheFile(const QUuid &profileId)
{
    return QDir(epgCacheDirectory()).filePath(QStringLiteral("%1.cache").arg(profileId.toString(QUuid::WithoutBraces).toLower()));
}

QString AppDataPaths::iconCacheDirectory()
{
    return ensureExists(QDir(dataDirectory()).filePath(QStringLiteral("icons")));
}

QString AppDataPaths::screenshotsDirectory()
{
    return ensureExists(QDir(dataDirectory()).filePath(QStringLiteral("screenshots")));
}

QString AppDataPaths::recordingsDirectory()
{
    return ensureExists(QDir(dataDirectory()).filePath(QStringLiteral("recordings")));
}

QString AppDataPaths::timeshiftDirectory()
{
    return ensureExists(QDir(dataDirectory()).filePath(QStringLiteral("timeshift")));
}

QString AppDataPaths::ensureExists(const QString &path)
{
    QDir().mkpath(path);
    return path;
}

} // namespace OKILTV::Core
