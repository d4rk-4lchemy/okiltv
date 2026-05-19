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

    QList<Channel> loadFromUrl(const QUrl &url, const QUuid &profileId) const;
    QList<Channel> loadFromFile(const QString &path, const QUuid &profileId) const;
    QList<Channel> parse(const QByteArray &data, const QUuid &profileId) const;

private:
    Channel parseEntry(const QString &extinf, const QString &url, int index, const QUuid &profileId) const; // NOLINT(bugprone-easily-swappable-parameters)

    std::shared_ptr<NetworkAccess> m_network;
};

} // namespace OKILTV::Core
