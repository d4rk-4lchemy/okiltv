#include "portablebootstrap.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace OKILTV::Core {

namespace {

constexpr int kSchemaVersion = 1;

} // namespace

PortableBootstrapConfig PortableBootstrap::load(const QString &bootstrapPath)
{
    QFile file(bootstrapPath);
    if (bootstrapPath.trimmed().isEmpty() || !file.exists() || !file.open(QIODevice::ReadOnly)) {
        return {};
    }

    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return {};
    }

    const auto object = document.object();
    PortableBootstrapConfig config;
    config.schemaVersion = object.value(QStringLiteral("schemaVersion")).toInt(kSchemaVersion);
    config.dataRootOverride = normalizedDataRootOverride(object.value(QStringLiteral("dataRootOverride")).toString());
    if (config.schemaVersion != kSchemaVersion) {
        return {};
    }

    return config;
}

bool PortableBootstrap::save(const QString &bootstrapPath, const PortableBootstrapConfig &config, QString *errorText)
{
    if (bootstrapPath.trimmed().isEmpty()) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Portable bootstrap path is not configured.");
        }
        return false;
    }

    const auto overrideError = dataRootOverrideError(config.dataRootOverride);
    if (!overrideError.isEmpty()) {
        if (errorText != nullptr) {
            *errorText = overrideError;
        }
        return false;
    }

    const QFileInfo bootstrapInfo(bootstrapPath);
    if (!QDir().mkpath(bootstrapInfo.absolutePath())) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Failed to create portable bootstrap directory.");
        }
        return false;
    }

    QSaveFile file(bootstrapPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Failed to open portable bootstrap file for writing.");
        }
        return false;
    }

    QJsonObject object;
    object.insert(QStringLiteral("schemaVersion"), kSchemaVersion);
    object.insert(QStringLiteral("dataRootOverride"), normalizedDataRootOverride(config.dataRootOverride));
    if (file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Failed to save portable bootstrap file.");
        }
        return false;
    }

    return true;
}

QString PortableBootstrap::normalizedDataRootOverride(const QString &path)
{
    const auto trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    if (!QDir::isAbsolutePath(trimmed)) {
        return {};
    }

    return QDir::cleanPath(trimmed);
}

QString PortableBootstrap::dataRootOverrideError(const QString &path)
{
    const auto trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    if (!QDir::isAbsolutePath(trimmed)) {
        return QStringLiteral("Portable data directory must be an absolute path.");
    }

    return {};
}

} // namespace OKILTV::Core
