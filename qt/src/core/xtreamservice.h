#pragma once

#include "models.h"
#include "networkaccess.h"

#include <optional>

namespace OKILTV::Core {

class XtreamService
{
public:
    struct AuthInfo
    {
        bool authenticated { false };
        QString serverTimezone;
    };

    explicit XtreamService(std::shared_ptr<NetworkAccess> network = makeDefaultNetworkAccess());

    void setProfile(const ServerProfile &profile);

    AuthInfo authenticate() const;
    QList<ChannelCategory> getLiveCategories() const;
    QList<Channel> getLiveStreams(const std::optional<QString> &categoryId = std::nullopt) const;
    QByteArray getXmltvBytes() const;

private:
    QString buildStreamUrl(int streamId, const QString &ext = QStringLiteral("ts")) const;
    QUrl apiUrl(const std::optional<QString> &action) const;
    void ensureProfile() const;
    const ServerProfile &profile() const;

    std::shared_ptr<NetworkAccess> m_network;
    std::optional<ServerProfile> m_profile;
};

} // namespace OKILTV::Core
