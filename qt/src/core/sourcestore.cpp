#include "sourcestore.h"

#include "appdatapaths.h"
#include "secretprotection.h"
#include <stdexcept>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>

namespace OKILTV::Core {

namespace {

QByteArray serializedJson(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

QString normalizeDetailDirectory(const QString &sourceDetailsDirectory)
{
    if (!sourceDetailsDirectory.trimmed().isEmpty()) {
        return sourceDetailsDirectory;
    }
    return AppDataPaths::sourcesDirectory();
}

QJsonArray summariesToJson(const QList<SourceSummary> &summaries)
{
    QJsonArray array;
    for (const auto &summary : summaries) {
        array.push_back(toJson(summary));
    }
    return array;
}

QList<SourceSummary> summariesFromJson(const QJsonArray &array)
{
    QList<SourceSummary> summaries;
    summaries.reserve(array.size());
    for (const auto &value : array) {
        if (!value.isObject()) {
            continue;
        }

        summaries.push_back(sourceSummaryFromJson(value.toObject()));
    }

    return summaries;
}

} // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — explicit file/directory persistence paths.
SourceStore::SourceStore(QString summariesFilePath, const QString &sourceDetailsDirectory)
    : m_summariesFilePath(summariesFilePath.isEmpty() ? AppDataPaths::sourceSummariesFile() : std::move(summariesFilePath))
    , m_sourceDetailsDirectory(normalizeDetailDirectory(sourceDetailsDirectory))
{
}

QList<SourceSummary> SourceStore::loadSummaries() const
{
    QFile file(m_summariesFilePath);
    if (!file.exists()) {
        return {};
    }

    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("Cannot read saved source summaries; original data was preserved.");
    }

    const auto bytes = file.readAll();
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        throw std::runtime_error("Invalid saved source summaries; original data was preserved.");
    }

    const auto root = document.object();
    if (!root.value(QStringLiteral("summaries")).isArray()) throw std::runtime_error("Invalid saved source summaries.");
    return summariesFromJson(root.value(QStringLiteral("summaries")).toArray());
}

bool SourceStore::saveSummaries(const QList<SourceSummary> &summaries, QString *errorText) const
{
    if (errorText != nullptr) {
        errorText->clear();
    }

    const QFileInfo fileInfo(m_summariesFilePath);
    if (!QDir().mkpath(fileInfo.absolutePath())) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Failed to create source summaries directory: %1").arg(fileInfo.absolutePath());
        }
        return false;
    }

    QSaveFile file(m_summariesFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Failed to open source summaries file for write: %1").arg(file.errorString());
        }
        return false;
    }

    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (errorText != nullptr) *errorText = QStringLiteral("Cannot restrict source summaries permissions.");
        file.cancelWriting();
        return false;
    }
    QJsonObject root;
    root.insert(QStringLiteral("summaries"), summariesToJson(summaries));
    const auto payload = serializedJson(root);
    if (file.write(payload) != payload.size()) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Failed to write source summaries file: %1").arg(file.errorString());
        }
        file.cancelWriting();
        return false;
    }

    if (!file.commit()) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Failed to commit source summaries file: %1").arg(file.errorString());
        }
        return false;
    }

    return true;
}

std::optional<ServerProfile> SourceStore::loadDetail(const QUuid &profileId) const
{
    if (profileId.isNull()) {
        return std::nullopt;
    }

    QFile file(detailFilePath(profileId));
    if (!file.exists()) {
        return std::nullopt;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }

    const auto bytes = file.readAll();
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }

    auto object = document.object();
    if (object.contains(QStringLiteral("protectedProfile"))) {
        const auto value = object.value(QStringLiteral("protectedProfile"));
        if (!value.isString() || !isProtectedSecret(value.toString())) {
            throw std::runtime_error("Invalid protected source file.");
        }
        const auto decoded = QJsonDocument::fromJson(unprotectSecret(value.toString()).toUtf8());
        if (!decoded.isObject()) throw std::runtime_error("Invalid protected source data.");
        object = decoded.object();
    }
    auto profile = serverProfileFromJson(object);
    if (profile.id.isNull()) {
        profile.id = profileId;
    }
    if (profile.id != profileId) throw std::runtime_error("Source identity does not match its file; migration stopped.");
    return profile;
}

void SourceStore::migrateLegacyDetails() const
{
    // Include orphaned source files from interrupted historical migrations.
    const auto files = QDir(m_sourceDetailsDirectory).entryInfoList({ QStringLiteral("*.json") }, QDir::Files);
    for (const auto &file : files) {
        const auto id = parseGuid(file.baseName());
        if (id.isNull() || detailIsProtected(id)) continue;
        const auto profile = loadDetail(id);
        if (!profile.has_value()) throw std::runtime_error("Cannot read a legacy source; migration stopped.");
        QString error;
        if (!saveDetail(profile.value(), &error)) throw std::runtime_error(error.toStdString());
    }
}

bool SourceStore::detailIsProtected(const QUuid &profileId) const
{
    QFile file(detailFilePath(profileId));
    if (!file.open(QIODevice::ReadOnly)) return false;
    return QJsonDocument::fromJson(file.readAll()).object().contains(QStringLiteral("protectedProfile"));
}

bool SourceStore::saveDetail(const ServerProfile &profile, QString *errorText) const
{
    if (errorText != nullptr) {
        errorText->clear();
    }

    if (profile.id.isNull()) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Failed to save source detail: profile id is missing.");
        }
        return false;
    }

    if (!QDir().mkpath(m_sourceDetailsDirectory)) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Failed to create source details directory: %1").arg(m_sourceDetailsDirectory);
        }
        return false;
    }

    const auto filePath = detailFilePath(profile.id);
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Failed to open source detail file for write: %1").arg(file.errorString());
        }
        return false;
    }

    QByteArray payload;
    try {
        const auto plain = QString::fromUtf8(serializedJson(toJson(profile)));
        const auto protectedValue = protectSecret(plain);
        if (unprotectSecret(protectedValue) != plain) throw std::runtime_error("Source protection verification failed.");
        payload = serializedJson(QJsonObject { { QStringLiteral("protectedProfile"), protectedValue } });
    } catch (const std::exception &error) {
        if (errorText != nullptr) *errorText = QString::fromUtf8(error.what());
        file.cancelWriting();
        return false;
    }
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (errorText != nullptr) *errorText = QStringLiteral("Cannot restrict source file permissions.");
        file.cancelWriting();
        return false;
    }
    if (file.write(payload) != payload.size()) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Failed to write source detail file: %1").arg(file.errorString());
        }
        file.cancelWriting();
        return false;
    }

    if (!file.commit()) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Failed to commit source detail file: %1").arg(file.errorString());
        }
        return false;
    }

    return true;
}

bool SourceStore::removeDetail(const QUuid &profileId, QString *errorText) const
{
    if (errorText != nullptr) {
        errorText->clear();
    }

    if (profileId.isNull()) {
        return true;
    }

    const auto filePath = detailFilePath(profileId);
    const QFileInfo fileInfo(filePath);
    if (!fileInfo.exists()) {
        return true;
    }

    if (!QFile::remove(filePath)) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("Failed to remove source detail file: %1").arg(filePath);
        }
        return false;
    }

    return true;
}

QString SourceStore::detailFilePath(const QUuid &profileId) const
{
    return QDir(m_sourceDetailsDirectory).filePath(
        QStringLiteral("%1.json").arg(profileId.toString(QUuid::WithoutBraces).toLower()));
}

} // namespace OKILTV::Core
