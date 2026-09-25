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

QByteArray NetworkAccess::get(const QUrl &url, const std::function<bool()> &cancelled) const
{
    if (cancelled && cancelled()) throw std::runtime_error("Request cancelled.");
    auto bytes = get(url);
    if (cancelled && cancelled()) throw std::runtime_error("Request cancelled.");
    return bytes;
}

QByteArray BlockingNetworkAccess::get(const QUrl &url) const
{
    return get(url, {});
}

QByteArray BlockingNetworkAccess::get(const QUrl &url, const std::function<bool()> &cancelled) const
{
    if (cancelled && cancelled()) throw std::runtime_error("Request cancelled.");
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

    QTimer cancellationTimer;
    QObject::connect(&cancellationTimer, &QTimer::timeout, &loop, [&]() {
        if (cancelled && cancelled()) {
            reply->abort();
            loop.quit();
        }
    });
    if (cancelled) cancellationTimer.start(25);
    timer.start(m_timeoutMs);
    loop.exec();
    timer.stop();

    const auto error = reply->error();
    const auto errorString = redactSensitiveText(reply->errorString());
    const auto statusCode =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray payload;
    if (!timedOut && reply->isOpen()) {
        // Timeout path aborts/closes reply; reading from a closed reply emits
        // Qt warning noise: "QIODevice::read ... device not open".
        payload = reply->readAll();
    }
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
        throw std::runtime_error(QStringLiteral("Request timed out for %1").arg(requestObservation.redactedUrl).toStdString());
    }

    if (error != QNetworkReply::NoError) {
        throw std::runtime_error(
            QStringLiteral("Network request failed for %1: %2")
                .arg(requestObservation.redactedUrl, errorString)
                .toStdString());
    }

    if (statusCode >= 400) {
        throw std::runtime_error(
            QStringLiteral("HTTP %1 for %2")
                .arg(statusCode)
                .arg(requestObservation.redactedUrl)
                .toStdString());
    }

    return payload;
}

void NetworkAccess::download(const QUrl &url, QIODevice *destination,
    const std::function<bool()> &cancelled, const std::function<void(qint64)> &progress) const
{
    if (cancelled && cancelled()) throw std::runtime_error("EPG import cancelled.");
    const auto bytes = get(url);
    if (!destination || destination->write(bytes) != bytes.size()) throw std::runtime_error("Cannot write XMLTV download.");
    if (progress) progress(bytes.size());
}

void BlockingNetworkAccess::download(const QUrl &url, QIODevice *destination,
    const std::function<bool()> &cancelled, const std::function<void(qint64)> &progress) const
{
    if (!destination || !destination->isWritable()) throw std::runtime_error("XMLTV destination is not writable.");
    if (cancelled && cancelled()) throw std::runtime_error("EPG import cancelled.");
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto observation = makeObservation(url);
    observation.phase = QStringLiteral("request"); publishNetworkObservation(observation);
    auto *reply = manager.get(request);
    reply->setReadBufferSize(256LL * 1024);
    QEventLoop loop;
    QTimer timer;
    QElapsedTimer total, idle;
    total.start(); idle.start();
    QString failure;
    qint64 bytes = 0;
    const auto drain = [&] {
        char buffer[64 * 1024];
        while (reply->isOpen() && reply->bytesAvailable() > 0 && failure.isEmpty()) {
            const auto n = reply->read(buffer, sizeof(buffer));
            if (n <= 0) break;
            bytes += n;
            if ((cancelled && cancelled()) || bytes > 2LL * 1024 * 1024 * 1024)
                failure = QStringLiteral("XMLTV download cancelled or exceeds the 2 GiB limit.");
            else if (destination->write(buffer, n) != n) failure = QStringLiteral("Cannot write XMLTV download (check free disk space).");
            if (!failure.isEmpty()) { reply->abort(); break; }
            idle.restart();
            if (progress) progress(bytes);
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, drain);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if (cancelled && cancelled()) failure = QStringLiteral("EPG import cancelled.");
        else if (idle.elapsed() >= 30000 || total.elapsed() >= 30LL * 60 * 1000) {
            failure = QStringLiteral("XMLTV download timed out."); observation.timedOut = true;
        }
        if (!failure.isEmpty()) { reply->abort(); loop.quit(); }
    });
    timer.start(100);
    loop.exec();
    drain();
    if (failure.isEmpty() && reply->error() != QNetworkReply::NoError) failure = redactSensitiveText(reply->errorString());
    observation.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (failure.isEmpty() && observation.statusCode >= 400) failure = QStringLiteral("XMLTV HTTP %1").arg(observation.statusCode);
    observation.phase = QStringLiteral("reply"); observation.durationMs = total.elapsed();
    observation.payloadBytes = bytes; observation.errorText = failure; publishNetworkObservation(observation);
    if (!failure.isEmpty()) throw std::runtime_error(failure.toStdString());
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
