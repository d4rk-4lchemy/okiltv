#include "sourcestore.h"

#include "appdatapaths.h"

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

SourceStore::SourceStore(QString summariesFilePath, QString sourceDetailsDirectory)
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
        return {};
    }

    const auto bytes = file.readAll();
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }

    const auto root = document.object();
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

    auto profile = serverProfileFromJson(document.object());
    if (profile.id.isNull()) {
        profile.id = profileId;
    }
    return profile;
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

    const auto payload = serializedJson(toJson(profile));
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
