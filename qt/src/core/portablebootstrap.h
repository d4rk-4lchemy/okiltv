#pragma once

#include <QString>

namespace OKILTV::Core {

struct PortableBootstrapConfig
{
    int schemaVersion { 1 };
    QString dataRootOverride;
};

class PortableBootstrap
{
public:
    static PortableBootstrapConfig load(const QString &bootstrapPath);
    static bool save(const QString &bootstrapPath, const PortableBootstrapConfig &config, QString *errorText = nullptr);
    static QString normalizedDataRootOverride(const QString &path);
    static QString dataRootOverrideError(const QString &path);
};

} // namespace OKILTV::Core
