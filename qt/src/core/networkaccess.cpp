#include "networkaccess.h"

#include "redaction.h"

#include <QEventLoop>
#include <QHash>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QElapsedTimer>
#include <QMutex>
#include <QTimer>

#include <atomic>
#include <stdexcept>

namespace OKILTV::Core {

namespace {

QMutex &observerMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<quint64, NetworkObserver> &observerMap()
{
    static QHash<quint64, NetworkObserver> observers;
    return observers;
}

std::atomic<quint64> &observerSequence()
{
    static std::atomic<quint64> sequence { 1 };
    return sequence;
}

std::atomic<quint64> &eventSequence()
{
    static std::atomic<quint64> sequence { 1 };
    return sequence;
}

void publishNetworkObservation(const NetworkObservation &observation)
{
    QList<NetworkObserver> observers;
    {
        QMutexLocker locker(&observerMutex());
        observers = observerMap().values();
    }

    for (const auto &observer : observers) {
        if (observer) {
            observer(observation);
        }
    }
}

NetworkObservation makeObservation(const QUrl &url)
{
    NetworkObservation observation;
    observation.sequence = eventSequence().fetch_add(1);
    observation.method = QStringLiteral("GET");
    observation.category = networkCategoryForUrl(url);
    observation.url = url;
    observation.redactedUrl = redactSensitiveUrl(url.toString());
    return observation;
}

} // namespace

BlockingNetworkAccess::BlockingNetworkAccess(const int timeoutMs)
    : m_timeoutMs(timeoutMs)
{
}

QByteArray BlockingNetworkAccess::get(const QUrl &url) const
{
    // A new QNetworkAccessManager is created per call. This method is invoked
    // from background threads (via QtConcurrent::run) and QNetworkAccessManager
    // is not safe to share across threads. The per-call cost is one TCP/TLS
    // handshake; acceptable for the infrequent large fetches this is used for.
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QElapsedTimer elapsed;
    elapsed.start();

    auto requestObservation = makeObservation(url);
    requestObservation.phase = QStringLiteral("request");
    publishNetworkObservation(requestObservation);

    auto *reply = manager.get(request);
    QEventLoop loop;
    QTimer timer;
    bool timedOut = false;

    timer.setSingleShot(true);

    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        reply->abort();
        loop.quit();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    timer.start(m_timeoutMs);
    loop.exec();
    timer.stop();

    const auto payload = reply->readAll();
    const auto error = reply->error();
    const auto errorString = reply->errorString();
    const auto statusCode =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    auto replyObservation = makeObservation(url);
    replyObservation.phase = QStringLiteral("reply");
    replyObservation.durationMs = elapsed.elapsed();
    replyObservation.payloadBytes = payload.size();
    replyObservation.statusCode = statusCode;
    replyObservation.timedOut = timedOut;
    if (timedOut) {
        replyObservation.errorText = QStringLiteral("timed-out");
    } else if (error != QNetworkReply::NoError) {
        replyObservation.errorText = errorString;
    }
    publishNetworkObservation(replyObservation);

    if (timedOut) {
        throw std::runtime_error(QStringLiteral("Request timed out for %1").arg(url.toString()).toStdString());
    }

    if (error != QNetworkReply::NoError) {
        throw std::runtime_error(
            QStringLiteral("Network request failed for %1: %2")
                .arg(url.toString(), errorString)
                .toStdString());
    }

    if (statusCode >= 400) {
        throw std::runtime_error(
            QStringLiteral("HTTP %1 for %2")
                .arg(statusCode)
                .arg(url.toString())
                .toStdString());
    }

    return payload;
}

std::shared_ptr<NetworkAccess> makeDefaultNetworkAccess(const int timeoutMs)
{
    return std::make_shared<BlockingNetworkAccess>(timeoutMs);
}

quint64 addNetworkObserver(const NetworkObserver &observer)
{
    if (!observer) {
        return 0;
    }

    const auto observerId = observerSequence().fetch_add(1);
    QMutexLocker locker(&observerMutex());
    observerMap().insert(observerId, observer);
    return observerId;
}

void removeNetworkObserver(const quint64 observerId)
{
    if (observerId == 0) {
        return;
    }

    QMutexLocker locker(&observerMutex());
    observerMap().remove(observerId);
}

} // namespace OKILTV::Core
