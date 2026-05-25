#include "catchupstreamsession.h"

#include "../core/debuglogger.h"
#include "../core/redaction.h"

#include <QMetaObject>
#include <QNetworkRequest>

#include <algorithm>
#include <cstring>

namespace OKILTV::Player {

namespace {

constexpr qsizetype kDefaultQueueHighWaterBytes = 16 * 1024 * 1024;
constexpr qsizetype kDefaultQueueLowWaterBytes = 8 * 1024 * 1024;
constexpr qint64 kDefaultReplyReadBufferBytes = 16 * 1024 * 1024;
constexpr qint64 kCatchupReadChunkBytes = 256 * 1024;

QString formatBytesMiB(const qsizetype bytes)
{
    return QStringLiteral("%1 MiB").arg(static_cast<double>(std::max<qsizetype>(0, bytes)) / (1024.0 * 1024.0), 0, 'f', 1);
}

} // namespace

QMutex CatchupStreamSession::s_registryMutex;
QHash<QString, std::weak_ptr<CatchupStreamSession>> CatchupStreamSession::s_registry;
std::atomic_uint CatchupStreamSession::s_nextId { 1 };

CatchupStreamSession::CatchupStreamSession(
    QString sourceUrl,
    HeaderList requestHeaders,
    BufferingPolicy bufferingPolicy)
    : m_id(QStringLiteral("%1").arg(s_nextId.fetch_add(1), 8, 16, QLatin1Char('0')))
    , m_sourceUrl(std::move(sourceUrl))
    , m_virtualUrl(QStringLiteral("okiltv-catchup://%1").arg(m_id))
    , m_requestHeaders(std::move(requestHeaders))
    , m_queueHighWaterBytes(std::max<qsizetype>(1, bufferingPolicy.queueHighWaterBytes > 0
            ? bufferingPolicy.queueHighWaterBytes
            : kDefaultQueueHighWaterBytes))
    , m_queueLowWaterBytes(std::clamp<qsizetype>(
          bufferingPolicy.queueLowWaterBytes > 0 ? bufferingPolicy.queueLowWaterBytes : kDefaultQueueLowWaterBytes,
          1,
          m_queueHighWaterBytes))
    , m_replyReadBufferBytes(std::max<qint64>(1, bufferingPolicy.replyReadBufferBytes > 0
            ? bufferingPolicy.replyReadBufferBytes
            : kDefaultReplyReadBufferBytes))
    , m_roleLabel(bufferingPolicy.roleLabel.trimmed().isEmpty() ? QStringLiteral("active") : bufferingPolicy.roleLabel.trimmed())
{
}

CatchupStreamSession::~CatchupStreamSession()
{
    unregisterSession(m_virtualUrl);
    closeProviderConnection(QStringLiteral("session-destroyed"));
    if (m_reply) {
        m_reply->disconnect();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    cancelRead();
}

CatchupStreamSession::Ptr CatchupStreamSession::create(
    const QString &sourceUrl,
    HeaderList requestHeaders,
    BufferingPolicy bufferingPolicy)
{
    auto session = std::make_shared<CatchupStreamSession>(
        sourceUrl.trimmed(),
        std::move(requestHeaders),
        std::move(bufferingPolicy));
    {
        QMutexLocker locker(&s_registryMutex);
        s_registry.insert(session->virtualUrl(), session);
    }
    return session;
}

CatchupStreamSession::Ptr CatchupStreamSession::find(const QString &virtualUrl)
{
    QMutexLocker locker(&s_registryMutex);
    const auto it = s_registry.find(virtualUrl.trimmed());
    if (it == s_registry.end()) {
        return {};
    }
    auto session = it.value().lock();
    if (!session) {
        s_registry.erase(it);
    }
    return session;
}

bool CatchupStreamSession::unregisterSession(const QString &virtualUrl)
{
    QMutexLocker locker(&s_registryMutex);
    return s_registry.remove(virtualUrl.trimmed()) > 0;
}

QString CatchupStreamSession::sourceUrl() const
{
    return m_sourceUrl;
}

QString CatchupStreamSession::virtualUrl() const
{
    return m_virtualUrl;
}

bool CatchupStreamSession::start()
{
    if (m_sourceUrl.isEmpty() || !QUrl(m_sourceUrl).isValid()) {
        failNetwork(QStringLiteral("invalid catch-up source URL"));
        return false;
    }
    if (m_started) {
        return true;
    }
    m_started = true;
    QNetworkRequest request { QUrl(m_sourceUrl) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    for (const auto &[name, value] : m_requestHeaders) {
        if (!name.isEmpty() && !value.isEmpty()) {
            request.setRawHeader(name, value);
        }
    }
    m_reply = m_networkAccess.get(request);
    m_reply->setReadBufferSize(m_replyReadBufferBytes);
    QObject::connect(m_reply, &QIODevice::readyRead, m_reply, [this]() { appendNetworkData(); });
    QObject::connect(m_reply, &QNetworkReply::finished, m_reply, [this]() { finishNetwork(); });
    QObject::connect(
        m_reply,
        &QNetworkReply::errorOccurred,
        m_reply,
        [this](QNetworkReply::NetworkError) {
            {
                QMutexLocker locker(&m_mutex);
                if (m_abortExpected) {
                    return;
                }
            }
            if (m_reply) {
                failNetwork(m_reply->errorString());
            }
        });
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up owned stream started: role=%1 virtual=%2 source=%3 queue=%4/%5 reply-buffer=%6.")
            .arg(m_roleLabel, m_virtualUrl, Core::redactSensitiveUrl(m_sourceUrl))
            .arg(formatBytesMiB(m_queueLowWaterBytes))
            .arg(formatBytesMiB(m_queueHighWaterBytes))
            .arg(formatBytesMiB(static_cast<qsizetype>(m_replyReadBufferBytes))));
    appendNetworkData();
    return true;
}

void CatchupStreamSession::closeProviderConnection(const QString &reason)
{
    const auto normalizedReason = reason.trimmed().isEmpty() ? QStringLiteral("unspecified") : reason.trimmed();
    bool shouldAbort = false;
    if (m_reply && m_reply->isRunning()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Catch-up owned stream closing provider connection (%1): virtual=%2 buffered=%3.")
                .arg(normalizedReason, m_virtualUrl)
                .arg(bufferedBytes()));
        {
            QMutexLocker locker(&m_mutex);
            m_abortExpected = true;
            m_closeRequestedByApp = true;
            m_closeRequestReason = normalizedReason;
            m_closeRequestedTimer.restart();
            shouldAbort = true;
        }
    } else {
        auto alreadyClosed = false;
        QMutexLocker locker(&m_mutex);
        m_closeRequestedByApp = true;
        m_closeRequestReason = normalizedReason;
        m_closeRequestedTimer.restart();
        alreadyClosed = m_networkFinished;
        if (!alreadyClosed) {
            m_providerClosed = true;
            m_replyFinished = true;
            m_networkFinished = true;
        }
        if (alreadyClosed) {
            return;
        }
        wakeReaders();
    }
    if (shouldAbort && m_reply) {
        m_reply->abort();
    }
}

bool CatchupStreamSession::closeRequestedByApp() const
{
    QMutexLocker locker(&m_mutex);
    return m_closeRequestedByApp;
}

QString CatchupStreamSession::closeRequestReason() const
{
    QMutexLocker locker(&m_mutex);
    return m_closeRequestReason;
}

bool CatchupStreamSession::providerConnectionClosed() const
{
    QMutexLocker locker(&m_mutex);
    return m_providerClosed;
}

bool CatchupStreamSession::hasNetworkError() const
{
    QMutexLocker locker(&m_mutex);
    return m_networkError;
}

QString CatchupStreamSession::errorString() const
{
    QMutexLocker locker(&m_mutex);
    return m_errorString;
}

qsizetype CatchupStreamSession::bufferedBytes() const
{
    QMutexLocker locker(&m_mutex);
    return m_bufferedBytes;
}

qsizetype CatchupStreamSession::peakBufferedBytes() const
{
    QMutexLocker locker(&m_mutex);
    return m_peakBufferedBytes;
}

qint64 CatchupStreamSession::read(char *buffer, const quint64 maxBytes)
{
    if (buffer == nullptr || maxBytes == 0) {
        return 0;
    }

    QMutexLocker locker(&m_mutex);
    while (!m_cancelled && m_chunks.empty() && !m_networkFinished) {
        m_dataAvailable.wait(&m_mutex, 250);
    }
    if (m_cancelled || (m_networkError && m_chunks.empty())) {
        return -1;
    }
    if (m_chunks.empty() && m_networkFinished) {
        return 0;
    }

    auto bytesRemaining = static_cast<qsizetype>(maxBytes);
    qint64 totalCopied = 0;
    while (bytesRemaining > 0 && !m_chunks.empty()) {
        auto &frontChunk = m_chunks.front();
        const auto availableInChunk = std::max<qsizetype>(0, frontChunk.size() - m_frontOffset);
        if (availableInChunk <= 0) {
            m_chunks.pop_front();
            m_frontOffset = 0;
            continue;
        }
        const auto bytesToCopy = std::min(bytesRemaining, availableInChunk);
        memcpy(
            buffer + totalCopied,
            frontChunk.constData() + m_frontOffset,
            static_cast<size_t>(bytesToCopy));
        totalCopied += bytesToCopy;
        bytesRemaining -= bytesToCopy;
        m_frontOffset += bytesToCopy;
        m_bufferedBytes -= bytesToCopy;
        if (m_frontOffset >= frontChunk.size()) {
            m_chunks.pop_front();
            m_frontOffset = 0;
        }
    }

    const auto drainedBelowLowWater =
        m_backpressureActive && m_bufferedBytes <= m_queueLowWaterBytes && !m_drainScheduled;
    if (drainedBelowLowWater) {
        m_backpressureActive = false;
        m_drainScheduled = true;
    }
    locker.unlock();

    if (drainedBelowLowWater) {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral(
                "Catch-up owned stream queue drained below low-water: role=%1 virtual=%2 buffered=%3 peak=%4. Resuming reply drain.")
                .arg(m_roleLabel, m_virtualUrl, formatBytesMiB(bufferedBytes()), formatBytesMiB(peakBufferedBytes())));
        scheduleDrainOnReplyThread();
    }
    return totalCopied;
}

void CatchupStreamSession::cancelRead()
{
    {
        QMutexLocker locker(&m_mutex);
        m_cancelled = true;
    }
    wakeReaders();
}

void CatchupStreamSession::appendNetworkData()
{
    if (!m_reply || !m_reply->isOpen()) {
        return;
    }
    {
        QMutexLocker locker(&m_mutex);
        m_drainScheduled = false;
        if (m_cancelled) {
            return;
        }
    }

    while (m_reply && m_reply->isOpen()) {
        qint64 bytesToRead = 0;
        bool reachedHighWater = false;
        {
            QMutexLocker locker(&m_mutex);
            if (m_cancelled) {
                return;
            }
            if (m_bufferedBytes >= m_queueHighWaterBytes) {
                reachedHighWater = !m_backpressureActive;
                m_backpressureActive = true;
            } else {
                bytesToRead = std::min<qint64>(
                    kCatchupReadChunkBytes,
                    static_cast<qint64>(m_queueHighWaterBytes - m_bufferedBytes));
            }
        }
        if (reachedHighWater) {
            Core::DebugLogger::instance().log(
                QStringLiteral("player"),
                QStringLiteral(
                    "Catch-up owned stream queue hit high-water: role=%1 virtual=%2 buffered=%3 peak=%4. Pausing reply drain.")
                    .arg(m_roleLabel, m_virtualUrl, formatBytesMiB(bufferedBytes()), formatBytesMiB(peakBufferedBytes())));
        }
        if (bytesToRead <= 0) {
            break;
        }

        const auto data = m_reply->read(bytesToRead);
        if (data.isEmpty()) {
            break;
        }

        {
            QMutexLocker locker(&m_mutex);
            if (m_cancelled) {
                return;
            }
            m_chunks.push_back(data);
            m_bufferedBytes += data.size();
            m_peakBufferedBytes = std::max(m_peakBufferedBytes, m_bufferedBytes);
        }
        wakeReaders();
    }

    const auto replyFullyDrained = m_replyFinished && m_reply->bytesAvailable() <= 0;
    if (replyFullyDrained) {
        {
            QMutexLocker locker(&m_mutex);
            m_networkFinished = true;
        }
        wakeReaders();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void CatchupStreamSession::finishNetwork()
{
    {
        QMutexLocker locker(&m_mutex);
        m_replyFinished = true;
    }
    if (m_reply && m_reply->isOpen()) {
        appendNetworkData();
    }
    auto closeRequestedByApp = false;
    auto abortExpected = false;
    QString closeRequestReason;
    qint64 closeRequestDelayMs = -1;
    auto replyErrorCode = static_cast<int>(QNetworkReply::NoError);
    QString replyErrorString = QStringLiteral("none");
    {
        QMutexLocker locker(&m_mutex);
        m_providerClosed = true;
        closeRequestedByApp = m_closeRequestedByApp;
        closeRequestReason = m_closeRequestReason;
        if (m_closeRequestedTimer.isValid()) {
            closeRequestDelayMs = m_closeRequestedTimer.elapsed();
        }
        abortExpected = m_abortExpected;
        if (m_reply) {
            replyErrorCode = static_cast<int>(m_reply->error());
            if (m_reply->error() != QNetworkReply::NoError) {
                replyErrorString = m_reply->errorString();
            }
        }
        if (m_reply && m_reply->error() != QNetworkReply::NoError && !m_networkError && !m_abortExpected) {
            m_networkError = true;
            m_errorString = m_reply->errorString();
        }
        if ((!m_reply || m_reply->bytesAvailable() <= 0) && !m_networkFinished) {
            m_networkFinished = true;
        }
    }
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral(
            "Catch-up owned stream provider closed: role=%1 virtual=%2 buffered=%3 peak=%4 error=%5 replyCode=%6 replyError=%7 appClose=%8 appReason=%9 abortExpected=%10.")
            .arg(m_roleLabel)
            .arg(m_virtualUrl)
            .arg(formatBytesMiB(bufferedBytes()))
            .arg(formatBytesMiB(peakBufferedBytes()))
            .arg(hasNetworkError() ? errorString() : QStringLiteral("none"))
            .arg(replyErrorCode)
            .arg(replyErrorString)
            .arg(closeRequestedByApp ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(closeRequestReason.isEmpty() ? QStringLiteral("none") : closeRequestReason)
            .arg(abortExpected ? QStringLiteral("yes") : QStringLiteral("no"))
            + (closeRequestedByApp && closeRequestDelayMs >= 0
                   ? QStringLiteral(" closeDelayMs=%1").arg(closeRequestDelayMs)
                   : QString {}));
    wakeReaders();
    if (m_reply && m_networkFinished) {
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void CatchupStreamSession::failNetwork(const QString &message)
{
    {
        QMutexLocker locker(&m_mutex);
        m_networkError = true;
        m_errorString = message.trimmed();
        m_providerClosed = true;
        m_replyFinished = true;
        m_networkFinished = true;
    }
    wakeReaders();
}

void CatchupStreamSession::scheduleDrainOnReplyThread()
{
    const auto reply = m_reply;
    if (!reply) {
        return;
    }
    const auto weakSession = weak_from_this();
    QMetaObject::invokeMethod(
        reply,
        [weakSession]() {
            const auto session = weakSession.lock();
            if (!session) {
                return;
            }
            session->appendNetworkData();
        },
        Qt::QueuedConnection);
}

void CatchupStreamSession::wakeReaders()
{
    m_dataAvailable.wakeAll();
}

} // namespace OKILTV::Player
