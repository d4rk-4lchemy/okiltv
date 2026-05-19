#pragma once

#include "database_service.h"
#include "models.h"
#include "networkaccess.h"

#include <memory>

namespace OKILTV::Core {

class IconCacheService
{
public:
    IconCacheService(DatabaseService &database, std::shared_ptr<NetworkAccess> network = makeDefaultNetworkAccess());

    QString getOrDownload(Channel &channel) const;

private:
    DatabaseService &m_database;
    std::shared_ptr<NetworkAccess> m_network;
};

} // namespace OKILTV::Core
