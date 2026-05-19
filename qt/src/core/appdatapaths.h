#pragma once

#include <QString>
#include <QUuid>

namespace OKILTV::Core {

enum class LaunchMode
{
    Standard,
    Portable
};

struct RuntimeContext
{
    LaunchMode launchMode { LaunchMode::Standard };
    QString portableBootstrapPath;
    QString dataRootOverride;
};

class AppDataPaths
{
public:
    static void initializeRuntime(const RuntimeContext &context);
    static void resetRuntimeForTests();
    static RuntimeContext runtimeContext();
    static QString defaultDataDirectory();
    static QString dataDirectory();
    static QString debugDumpDirectory();
    static QString settingsFile();
    static QString databaseFile();
    static QString epgCacheDirectory();
    static QString epgCacheFile(const QUuid &profileId);
    static QString iconCacheDirectory();
    static QString screenshotsDirectory();
    static QString recordingsDirectory();
    static QString timeshiftDirectory();

private:
    static QString ensureExists(const QString &path);
};

} // namespace OKILTV::Core
