#pragma once

#include "models.h"
#include "networkaccess.h"

#include <QUrl>

#include <memory>

namespace OKILTV::Core {

class M3UService
{
public:
    explicit M3UService(std::shared_ptr<NetworkAccess> network = makeDefaultNetworkAccess());

    QList<Channel> loadFromUrl(const QUrl &url, const QUuid &profileId, QStringList *epgUrls = nullptr) const;
    QList<Channel> loadFromFile(const QString &path, const QUuid &profileId, QStringList *epgUrls = nullptr) const;
    QList<Channel> parse(const QByteArray &data, const QUuid &profileId,
                         const QUrl &baseUrl = {}, QStringList *epgUrls = nullptr) const;
    static void retainChannelIds(QList<Channel> &channels, const QList<Channel> &previous, qint64 &nextId);

private:
    Channel parseEntry(const QString &extinf, const QString &url, int index, const QUuid &profileId,
                       const QHash<QString, QString> &defaults) const; // NOLINT(bugprone-easily-swappable-parameters)

    std::shared_ptr<NetworkAccess> m_network;
};

} // namespace OKILTV::Core
