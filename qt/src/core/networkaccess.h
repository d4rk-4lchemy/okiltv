#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QtGlobal>

#include <functional>
#include <memory>

namespace OKILTV::Core {

struct NetworkObservation
{
    quint64 sequence { 0 };
    QString phase;
    QString method;
    QString category;
    QUrl url;
    QString redactedUrl;
    int statusCode { 0 };
    qint64 durationMs { 0 };
    qint64 payloadBytes { 0 };
    bool timedOut { false };
    QString errorText;
};

using NetworkObserver = std::function<void(const NetworkObservation &)>;

class NetworkAccess
{
public:
    virtual ~NetworkAccess() = default;
    virtual QByteArray get(const QUrl &url) const = 0;
};

class BlockingNetworkAccess final : public NetworkAccess
{
public:
    explicit BlockingNetworkAccess(int timeoutMs = 30000);

    QByteArray get(const QUrl &url) const override;

private:
    int m_timeoutMs { 30000 };
};

quint64 addNetworkObserver(const NetworkObserver &observer);
void removeNetworkObserver(quint64 observerId);

std::shared_ptr<NetworkAccess> makeDefaultNetworkAccess(int timeoutMs = 30000);

} // namespace OKILTV::Core
