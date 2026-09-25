#include "iconcache_service.h"

#include "appdatapaths.h"
#include "debuglogger.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QUrl>

namespace OKILTV::Core {

namespace {

QString sha1Hex(const QString &value)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString normalizedExtension(const QString &iconUrl)
{
    const auto suffix = QFileInfo(QUrl(iconUrl).path()).suffix().toLower();
    if (suffix == QStringLiteral("png")
        || suffix == QStringLiteral("jpg")
        || suffix == QStringLiteral("jpeg")
        || suffix == QStringLiteral("gif")
        || suffix == QStringLiteral("webp")) {
        return QStringLiteral(".") + suffix;
    }

    return QStringLiteral(".png");
}

} // namespace

IconCacheService::IconCacheService(DatabaseService &database, std::shared_ptr<NetworkAccess> network)
    : m_database(database)
    , m_network(std::move(network))
{
}

QString IconCacheService::getOrDownload(Channel &channel, const std::function<bool()> &cancelled) const
{
    try {
        if (cancelled && cancelled()) return {};
        if (channel.iconUrl.trimmed().isEmpty()) {
            return {};
        }

        if (!channel.cachedIconPath.isEmpty() && QFile::exists(channel.cachedIconPath)) {
            return channel.cachedIconPath;
        }

        const auto hash = sha1Hex(channel.iconUrl);
        auto fromCacheTable = m_database.cachedIconByHash(hash);
        if (!fromCacheTable.isEmpty() && QFile::exists(fromCacheTable)) {
            channel.cachedIconPath = fromCacheTable;
            return fromCacheTable;
        }

        auto localPath =
            QDir(AppDataPaths::iconCacheDirectory()).filePath(hash + normalizedExtension(channel.iconUrl));
        if (QFile::exists(localPath)) {
            m_database.upsertIconCache(hash, localPath, QDateTime::currentSecsSinceEpoch());
            channel.cachedIconPath = localPath;
            return localPath;
        }

        const auto payload = m_network->get(QUrl(channel.iconUrl), cancelled);
        if (cancelled && cancelled()) return {};
        QSaveFile file(localPath);
        if (!file.open(QIODevice::WriteOnly)) {
            Core::DebugLogger::instance().log(QStringLiteral("icons"),
                QStringLiteral("Failed to open icon cache file for writing: %1").arg(localPath));
            return {};
        }

        if (file.write(payload) != payload.size() || !file.commit()) {
            Core::DebugLogger::instance().log(QStringLiteral("icons"),
                QStringLiteral("Failed to commit icon cache file: %1").arg(localPath));
            return {};
        }

        m_database.upsertIconCache(hash, localPath, QDateTime::currentSecsSinceEpoch());
        channel.cachedIconPath = localPath;
        return localPath;
    } catch (const std::exception &e) {
        Core::DebugLogger::instance().log(QStringLiteral("icons"),
            QStringLiteral("Exception while downloading icon for %1: %2")
                .arg(channel.iconUrl, QString::fromUtf8(e.what())));
        return {};
    } catch (...) {
        Core::DebugLogger::instance().log(QStringLiteral("icons"),
            QStringLiteral("Unknown exception while downloading icon for %1").arg(channel.iconUrl));
        return {};
    }
}

} // namespace OKILTV::Core
