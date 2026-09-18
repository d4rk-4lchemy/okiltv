#include "catchupstreamsession.h"

#include "../core/debuglogger.h"
#include "../core/redaction.h"
#include "../core/catchupurlresolver.h"

#include <QMetaObject>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <cstring>
#include <limits>
#include <cmath>

namespace OKILTV::Player {

namespace {

constexpr qsizetype kDefaultQueueHighWaterBytes = 16LL * 1024 * 1024;
constexpr qsizetype kDefaultQueueLowWaterBytes = 8LL * 1024 * 1024;
constexpr qint64 kDefaultReplyReadBufferBytes = 16LL * 1024 * 1024;
constexpr qint64 kCatchupReadChunkBytes = 256LL * 1024;
constexpr qint64 kContinuationOverlapSeconds = 8;

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
    const BufferingPolicy &bufferingPolicy)
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
    , m_transferTimeoutMs(std::max(0, bufferingPolicy.transferTimeoutMs))
{
    m_continuationTimer.setSingleShot(true);
    QObject::connect(&m_continuationTimer, &QTimer::timeout, &m_networkAccess,
                     [this]() { openContinuation(); });
    if (bufferingPolicy.normalizeMpegTsTimestamps) {
        m_timestampNormalizer.emplace();
    }
    m_networkIdleTimer.setSingleShot(true);
    QObject::connect(&m_networkIdleTimer, &QTimer::timeout, &m_networkAccess, [this]() {
        if (!m_reply || !m_reply->isRunning()) {
            return;
        }
        {
            QMutexLocker locker(&m_mutex);
            // Pausing playback can deliberately stop network reads. Qt's
            // transfer timeout also counts that time, so use a pause-aware one.
            if (m_backpressureActive || m_drainScheduled || m_reply->bytesAvailable() > 0) {
                m_networkIdleTimer.start(m_transferTimeoutMs);
                return;
            }
            m_networkError = true;
            m_errorString = QStringLiteral("Timed out waiting for stream data.");
        }
        m_reply->abort();
    });
}

CatchupStreamSession::HeaderList CatchupStreamSession::requestHeadersFromOptions(
    const QString &playerUserAgent,
    const QMap<QString, QString> &mpvOptions)
{
    HeaderList headers;
    auto appendHeader = [&headers](QByteArray name, QByteArray value) {
        name = name.trimmed();
        value = value.trimmed();
        if (name.isEmpty() || value.isEmpty()) {
            return;
        }
        for (auto &header : headers) {
            if (header.first.compare(name, Qt::CaseInsensitive) == 0) {
                header.second = value;
                return;
            }
        }
        headers.append(qMakePair(std::move(name), std::move(value)));
    };

    const auto trimmedUserAgent = playerUserAgent.trimmed();
    if (!trimmedUserAgent.isEmpty()) {
        appendHeader(QByteArrayLiteral("User-Agent"), trimmedUserAgent.toUtf8());
    }

    const auto rawHeaderFields = mpvOptions.value(QStringLiteral("http-header-fields")).trimmed();
    if (!rawHeaderFields.isEmpty()) {
        const QRegularExpression splitPattern(
            QStringLiteral(",(?=\\s*[!#$%&'*+.^_`|~0-9A-Za-z-]+\\s*:)"));
        const auto headerEntries = rawHeaderFields.split(splitPattern, Qt::SkipEmptyParts);
        for (const auto &entry : headerEntries) {
            const auto separatorIndex = entry.indexOf(u':');
            if (separatorIndex <= 0) {
                continue;
            }
            appendHeader(
                entry.left(separatorIndex).trimmed().toUtf8(),
                entry.mid(separatorIndex + 1).trimmed().toUtf8());
        }
    }

    const auto referrer = mpvOptions.value(QStringLiteral("referrer")).trimmed();
    if (!referrer.isEmpty()) {
        appendHeader(QByteArrayLiteral("Referer"), referrer.toUtf8());
    }

    return headers;
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
    const BufferingPolicy &bufferingPolicy)
{
    // mpv can release the final callback reference on its demux thread. The
    // network manager and timers must still be destroyed on their Qt thread.
    auto session = Ptr(new CatchupStreamSession(
        sourceUrl.trimmed(),
        std::move(requestHeaders),
        bufferingPolicy), [](CatchupStreamSession *session) {
        if (QThread::currentThread() == session->thread()) {
            delete session;
        } else {
            session->deleteLater();
        }
    });
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
    return openUrl(m_sourceUrl);
}

void CatchupStreamSession::configureContinuous(ContinuousPolicy policy)
{
    if (m_started) {
        return;
    }
    policy.safetySeconds = std::clamp(policy.safetySeconds, 180, 1800);
    m_continuousPolicy = std::move(policy);
    m_readReady = false;
}

bool CatchupStreamSession::continuous() const
{
    QMutexLocker locker(&m_mutex);
    return m_continuousPolicy.has_value() && !m_networkError && !m_cancelled && !m_closeRequestedByApp;
}

bool CatchupStreamSession::failedMediaTransport() const
{
    QMutexLocker locker(&m_mutex);
    return (m_continuousPolicy.has_value() || m_mediaPeriods) && m_networkError && !m_closeRequestedByApp;
}

bool CatchupStreamSession::openUrl(const QString &url)
{
    {
        QMutexLocker locker(&m_mutex);
        if (m_cancelled || m_closeRequestedByApp) {
            return false;
        }
        m_replyFinished = false;
        m_providerClosed = false;
        m_networkFinished = false;
    }
    QNetworkRequest request { QUrl(url) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    for (const auto &[name, value] : m_requestHeaders) {
        if (!name.isEmpty() && !value.isEmpty()) {
            request.setRawHeader(name, value);
        }
    }
    m_reply = m_networkAccess.get(request);
    if (m_transferTimeoutMs > 0) {
        m_networkIdleTimer.start(m_transferTimeoutMs);
    }
    m_reply->setReadBufferSize(m_replyReadBufferBytes);
    QObject::connect(m_reply, &QIODevice::readyRead, m_reply, [this]() { appendNetworkData(); });
    QObject::connect(m_reply, &QNetworkReply::finished, m_reply, [this]() { finishNetwork(); });
    QObject::connect(
        m_reply,
        &QNetworkReply::errorOccurred,
        m_reply,
        [weakSession = weak_from_this()](QNetworkReply::NetworkError) {
            const auto session = weakSession.lock();
            if (!session) {
                return;
            }
            {
                QMutexLocker locker(&session->m_mutex);
                if (session->m_abortExpected) {
                    return;
                }
            }
            if (session->m_reply) {
                QMutexLocker locker(&session->m_mutex);
                if (!session->m_networkError) {
                    session->m_networkError = true;
                    session->m_errorString = session->m_reply->errorString();
                }
            }
        });
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Catch-up owned stream started: role=%1 virtual=%2 source=%3 queue=%4/%5 reply-buffer=%6.")
            .arg(m_roleLabel, m_virtualUrl, Core::redactSensitiveUrl(url))
            .arg(formatBytesMiB(m_queueLowWaterBytes))
            .arg(formatBytesMiB(m_queueHighWaterBytes))
            .arg(formatBytesMiB(static_cast<qsizetype>(m_replyReadBufferBytes))));
    appendNetworkData();
    return true;
}

void CatchupStreamSession::closeProviderConnection(const QString &reason)
{
    m_continuationTimer.stop();
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
        if (!alreadyClosed && !m_reply) {
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

quint64 CatchupStreamSession::readGeneration() const
{
    QMutexLocker locker(&m_mutex);
    return m_readGeneration;
}

std::optional<double> CatchupStreamSession::nextPeriodBaseSeconds() const
{
    QMutexLocker locker(&m_mutex);
    return m_nextPeriodBaseSeconds;
}

void CatchupStreamSession::configureMediaPeriods(const double streamBaseSeconds)
{
    Q_ASSERT(!m_started);
    m_mediaPeriods = true;
    m_mediaPeriodBaseSeconds = streamBaseSeconds;
}

void CatchupStreamSession::allowRetriedForwardGap(std::optional<CatchupTsJoiner::ForwardGap> gap)
{
    Q_ASSERT(!m_started);
    m_joiner.allowRetriedForwardGap(gap);
}

std::optional<CatchupTsJoiner::ForwardGap> CatchupStreamSession::failedForwardGap() const
{
    QMutexLocker locker(&m_mutex);
    return m_failedForwardGap;
}

bool CatchupStreamSession::advancePeriod()
{
    if (!m_continuousPolicy.has_value() && !m_mediaPeriods) {
        return false;
    }
    QByteArray pending;
    double base = 0.0;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_nextPeriodBaseSeconds || !m_chunks.empty() || !m_pendingOutput.isEmpty() || m_closeRequestedByApp || m_networkError) {
            return false;
        }
        base = *m_nextPeriodBaseSeconds;
        pending = std::move(m_nextPeriodBytes);
        m_nextPeriodBaseSeconds.reset();
        ++m_readGeneration;
        m_cancelled = false;
        m_readReady = true; // No archive startup reserve on a buffered transition.
        m_backpressureActive = false;
    }
    m_joiner = CatchupTsJoiner {};
    if (m_timestampNormalizer) {
        m_timestampNormalizer.emplace();
    }
    if (m_continuousPolicy) {
        m_continuousPolicy->streamBaseSeconds = base;
        m_continuousPolicy->initialBufferSeconds = 0.0;
    }
    m_mediaPeriodBaseSeconds = base;
    m_previousResponseDuration = -1.0;
    m_noProgressAttempts = 0;
    if (!enqueueNetworkData(std::move(pending))) {
        return false;
    }
    scheduleDrainOnReplyThread();
    Core::DebugLogger::instance().log(QStringLiteral("player"),
        QStringLiteral("MPEG-TS period advanced: role=%1 base=%2s generation=%3; retaining HTTP response.")
            .arg(m_roleLabel).arg(base, 0, 'f', 3).arg(readGeneration()));
    return true;
}

qint64 CatchupStreamSession::read(char *buffer, const quint64 maxBytes)
{
    return read(readGeneration(), buffer, maxBytes);
}

qint64 CatchupStreamSession::read(const quint64 generation, char *buffer, const quint64 maxBytes)
{
    if (buffer == nullptr || maxBytes == 0) {
        return 0;
    }

    QMutexLocker locker(&m_mutex);
    while (generation == m_readGeneration && !m_cancelled && (m_chunks.empty() || !m_readReady)
           && !m_networkFinished && !m_nextPeriodBaseSeconds) {
        m_dataAvailable.wait(&m_mutex, 250);
    }
    if (generation != m_readGeneration) {
        return 0;
    }
    if (m_cancelled || (m_networkError && m_chunks.empty())) {
        return -1;
    }
    if (m_chunks.empty() && (m_networkFinished || m_nextPeriodBaseSeconds)) {
        return 0;
    }

    auto bytesRemaining = static_cast<qsizetype>(std::min<quint64>(
        maxBytes, static_cast<quint64>(std::numeric_limits<qsizetype>::max())));
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

    queueOutputLocked();
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
    cancelRead(readGeneration());
}

void CatchupStreamSession::cancelRead(const quint64 generation)
{
    {
        QMutexLocker locker(&m_mutex);
        if (generation != m_readGeneration) {
            return;
        }
        m_cancelled = true;
    }
    wakeReaders();
}

void CatchupStreamSession::queueOutputLocked(const QByteArray &data)
{
    m_pendingOutput += data;
    const auto count = std::min(m_queueHighWaterBytes - m_bufferedBytes, m_pendingOutput.size());
    if (count > 0) {
        m_chunks.push_back(m_pendingOutput.left(count));
        m_pendingOutput.remove(0, count);
        m_bufferedBytes += count;
        m_peakBufferedBytes = std::max(m_peakBufferedBytes, m_bufferedBytes);
    }
    if (!m_pendingOutput.isEmpty()) {
        m_backpressureActive = true;
        m_readReady = true;
    }
}

bool CatchupStreamSession::enqueueNetworkData(QByteArray data)
{
    if (m_continuousPolicy.has_value() || m_mediaPeriods) {
        const auto previousRepairs = m_joiner.repairedClockCount();
        data = m_joiner.push(data);
        if (m_joiner.repairedClockCount() != previousRepairs) {
            Core::DebugLogger::instance().log(QStringLiteral("player"),
                QStringLiteral("Isolated MPEG-TS PES clock repaired: role=%1 count=%2 %3; preserved media payload and audio clock.")
                    .arg(m_roleLabel).arg(m_joiner.repairedClockCount()).arg(m_joiner.clockRepairDescription()));
        }
        if (!m_joiner.valid() || (m_joiner.matching() && m_overlapTimer.elapsed() > 90000)) {
            {
                QMutexLocker locker(&m_mutex);
                m_failedForwardGap = m_joiner.failedForwardGap();
            }
            const auto verifiedSeconds = m_joiner.durationSeconds();
            const auto streamBase = m_continuousPolicy ? m_continuousPolicy->streamBaseSeconds : m_mediaPeriodBaseSeconds;
            const auto timelineSeconds = streamBase + verifiedSeconds;
            failNetwork(QStringLiteral("Cannot verify catch-up MPEG-TS continuity: %1; streamBase=%2s verifiedTimeline=%3s estimatedArchiveUtc=%4 matchingOverlap=%5; transport recovery required.")
                .arg(m_joiner.valid() ? QStringLiteral("overlap search timed out") : m_joiner.errorString())
                .arg(streamBase, 0, 'f', 3)
                .arg(timelineSeconds, 0, 'f', 3)
                .arg(m_continuousPolicy ? m_continuousPolicy->programStartUtc.addMSecs(static_cast<qint64>(timelineSeconds * 1000.0)).toUTC().toString(Qt::ISODateWithMs) : QStringLiteral("live"))
                .arg(m_joiner.matching()));
            if (m_reply) { m_reply->abort(); }
            return false;
        }
    }
    const bool boundary = (m_continuousPolicy || m_mediaPeriods) && m_joiner.periodPending();
    if (m_timestampNormalizer.has_value()) {
        data = m_timestampNormalizer->push(data);
        if (boundary) {
            data += m_timestampNormalizer->finish();
        }
    }

    {
        QMutexLocker locker(&m_mutex);
        if (m_cancelled) {
            return false;
        }
        if (boundary) {
            m_nextPeriodBaseSeconds = (m_continuousPolicy ? m_continuousPolicy->streamBaseSeconds : m_mediaPeriodBaseSeconds)
                + m_joiner.periodDurationSeconds();
            m_nextPeriodBytes = m_joiner.takeNextPeriod();
            m_readReady = true;
            m_networkIdleTimer.stop(); // HTTP is deliberately backpressured until cutover.
        }
        queueOutputLocked(data);
        if (m_continuousPolicy.has_value()
            && (m_joiner.durationSeconds() >= m_continuousPolicy->initialBufferSeconds
                || m_bufferedBytes >= m_queueHighWaterBytes)) {
            m_readReady = true;
        }
    }
    wakeReaders();
    if (boundary) {
        Core::DebugLogger::instance().log(QStringLiteral("player"),
            QStringLiteral("MPEG-TS media boundary: role=%1 %2 nextBase=%3s retained=%4 bytes; draining old media before decoder replacement.")
                .arg(m_roleLabel, m_joiner.periodDescription()).arg(nextPeriodBaseSeconds().value_or(0.0), 0, 'f', 3)
                .arg(m_nextPeriodBytes.size()));
    }
    return true;
}

void CatchupStreamSession::appendNetworkData()
{
    if (nextPeriodBaseSeconds().has_value()) {
        return;
    }
    if (!m_reply || !m_reply->isOpen()) {
        return;
    }
    {
        QMutexLocker locker(&m_mutex);
        m_drainScheduled = false;
        if (!m_pendingOutput.isEmpty()) {
            m_backpressureActive = true;
            return;
        }
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

        auto data = m_reply->read(bytesToRead);
        if (data.isEmpty()) {
            break;
        }
        if (m_transferTimeoutMs > 0) {
            m_networkIdleTimer.start(m_transferTimeoutMs);
        }
        if (!enqueueNetworkData(std::move(data)) || nextPeriodBaseSeconds().has_value()) {
            return;
        }
        {
            QMutexLocker locker(&m_mutex);
            if (!m_pendingOutput.isEmpty()) {
                return;
            }
        }
    }

    const auto replyFullyDrained = m_replyFinished && m_reply->bytesAvailable() <= 0;
    if (replyFullyDrained) {
        if (m_continuousPolicy.has_value() || m_mediaPeriods) {
            finishContinuousReply();
            return;
        }
        {
            QMutexLocker locker(&m_mutex);
            if (m_timestampNormalizer.has_value()) {
                auto tail = m_timestampNormalizer->finish();
                if (!tail.isEmpty()) {
                    m_bufferedBytes += tail.size();
                    m_chunks.push_back(std::move(tail));
                    m_peakBufferedBytes = std::max(m_peakBufferedBytes, m_bufferedBytes);
                }
            }
            m_networkFinished = true;
        }
        wakeReaders();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void CatchupStreamSession::finishNetwork()
{
    m_networkIdleTimer.stop();
    if (m_continuousPolicy.has_value() || m_mediaPeriods) {
        {
            QMutexLocker locker(&m_mutex);
            m_replyFinished = true;
            m_providerClosed = true;
        }
        if (m_reply && m_reply->isOpen()) {
            appendNetworkData();
        }
        if (m_reply && (!m_reply->isOpen() || m_reply->bytesAvailable() == 0)) {
            finishContinuousReply();
        }
        return;
    }
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
        // Aborted replies can retain bytesAvailable() even though reads are closed.
        if ((!m_reply || !m_reply->isOpen() || m_reply->bytesAvailable() <= 0) && !m_networkFinished) {
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

void CatchupStreamSession::finishContinuousReply()
{
    if (nextPeriodBaseSeconds().has_value()) {
        return;
    }
    if (!m_reply || (!m_continuousPolicy.has_value() && !m_mediaPeriods)) {
        return;
    }
    m_reply->disconnect();
    m_reply->deleteLater();
    m_reply = nullptr;
    bool terminal = false;
    {
        QMutexLocker locker(&m_mutex);
        terminal = m_cancelled || m_closeRequestedByApp || m_networkError;
        m_providerClosed = true;
        m_readReady = true; // A short final response must not deadlock startup.
    }
    const auto duration = m_joiner.durationSeconds();
    const auto policy = m_continuousPolicy.value_or(ContinuousPolicy {});
    const auto total = static_cast<double>(policy.programStartUtc.secsTo(policy.programStopUtc));
    if (terminal || m_mediaPeriods || !policy.allowContinuation || (!policy.endless && policy.streamBaseSeconds + duration >= total - 1.0)) {
        auto tail = m_joiner.finish();
        if (!m_joiner.valid()) {
            failNetwork(QStringLiteral("Invalid final catch-up transport data: %1").arg(m_joiner.errorString()));
            return;
        }
        if (m_timestampNormalizer.has_value()) {
            tail = m_timestampNormalizer->push(tail);
            tail += m_timestampNormalizer->finish();
        }
        {
            QMutexLocker locker(&m_mutex);
            queueOutputLocked(tail);
            m_networkFinished = true;
        }
        wakeReaders();
        return;
    }
    if (duration <= m_previousResponseDuration + 0.1) {
        ++m_noProgressAttempts;
        m_noProgressTimer.restart();
        if (!policy.templateChannel && !m_minuteContinuation && !m_joiner.matching()) {
            // Some Xtream servers ignore seconds and return the same minute
            // snapshot. Keep exact overlap matching, but request full minutes.
            m_minuteContinuation = true;
            m_noProgressAttempts = 0;
            Core::DebugLogger::instance().log(QStringLiteral("player"),
                QStringLiteral("Continuous catch-up repeated the verified snapshot; switching to minute-aligned continuation."));
        }
    } else {
        m_noProgressAttempts = 0;
    }
    m_previousResponseDuration = duration;
    if (m_joiner.matching() || m_noProgressAttempts >= 3 || !m_joiner.beginContinuation()) {
        failNetwork(QStringLiteral("Catch-up continuation has no verified new media; transport recovery required."));
        return;
    }
    Core::DebugLogger::instance().log(QStringLiteral("player"),
        QStringLiteral("Continuous catch-up response drained at %1s; retaining mpv transport and requesting overlap.")
            .arg(policy.streamBaseSeconds + duration, 0, 'f', 3));
    m_continuationTimer.start(m_noProgressAttempts > 0 ? 5000 : 750);
    wakeReaders();
}

void CatchupStreamSession::openContinuation()
{
    if (!m_continuousPolicy.has_value()) {
        return;
    }
    {
        QMutexLocker locker(&m_mutex);
        if (m_cancelled || m_closeRequestedByApp || m_networkFinished || m_networkError) {
            return;
        }
    }
    const auto &policy = m_continuousPolicy.value();
    const auto edge = Core::CatchupUrlResolver::availableEdge(
        policy.endless ? QDateTime::currentDateTimeUtc() : policy.programStopUtc, policy.safetySeconds);
    const auto available = policy.programStartUtc.secsTo(edge);
    const auto end = policy.streamBaseSeconds + m_joiner.durationSeconds();
    // Minute-only providers need a URL covering the growing final minute.
    // Waiting until that entire minute is behind the safety edge can outlast
    // the current cache. The HTTP range may extend beyond the safe seek edge;
    // only provider-delivered, byte-verified media is ever appended.
    const auto requestAvailable = m_minuteContinuation
        ? ((available + 59) / 60) * 60 : available;
    const auto requestEdge = requestAvailable;
    const auto offset = std::max<qint64>(0, static_cast<qint64>(std::floor(end)) - kContinuationOverlapSeconds - m_noProgressAttempts);
    // Use media progress, not the requested HTTP end: a successful response
    // can be a partial snapshot. Avoid repeating a completed minute for small
    // clock/keyframe rounding differences covered by the overlap allowance.
    // Start second-precision continuation as soon as its overlap anchor is
    // safe. Waiting for the last received timestamp wastes the overlap reserve
    // on the first provider-compatibility probe near the publication edge.
    const auto minimumAvailable = m_minuteContinuation ? end + 0.1 : static_cast<double>(offset);
    if (static_cast<double>(available) <= minimumAvailable
        || (m_minuteContinuation
            && static_cast<double>(requestEdge) <= end + kContinuationOverlapSeconds)) {
        m_continuationTimer.start(5000);
        return;
    }
    // Include clock/keyframe rounding slack. Retry with a distinct start to avoid
    // reusing a provider/CDN snapshot of the same finite URL.
    QString url;
    if (policy.templateChannel) {
        const auto window = Core::CatchupUrlResolver {}.resolveWindow(*policy.templateChannel,
            policy.programStartUtc.addSecs(offset), policy.programStartUtc.addSecs(requestAvailable));
        if (window) {
            url = window->url;
        }
    } else {
        url = Core::CatchupUrlResolver::xtreamWindowUrl(policy.canonicalUrl, offset, requestAvailable, !m_minuteContinuation);
    }
    if (url.isEmpty()) {
        // The viewer may have reached the safely published edge. Wait without EOF;
        // stop/seek must remain able to cancel this wait.
        m_continuationTimer.start(5000);
        return;
    }
    // An unchanged minute URL already proved to contain no new bytes. Wait
    // for a different published window. Retry after 30s as finite/partial
    // responses can grow even when the requested URL does not change.
    if ((m_minuteContinuation || policy.templateChannel) && m_noProgressAttempts > 0 && url == m_lastContinuationUrl
        && m_noProgressTimer.isValid() && m_noProgressTimer.elapsed() < 30000) {
        m_continuationTimer.start(5000);
        return;
    }
    m_overlapTimer.restart();
    m_lastContinuationUrl = url;
    openUrl(url);
}

void CatchupStreamSession::failNetwork(const QString &message)
{
    Core::DebugLogger::instance().log(QStringLiteral("player"),
        QStringLiteral("Catch-up owned stream failed: role=%1 virtual=%2 reason=%3.")
            .arg(m_roleLabel, m_virtualUrl, message.trimmed()));
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
    const auto weakSession = weak_from_this();
    QMetaObject::invokeMethod(
        &m_networkAccess,
        [weakSession]() {
            const auto session = weakSession.lock();
            if (!session) {
                return;
            }
            if (session->m_transferTimeoutMs > 0) {
                session->m_networkIdleTimer.start(session->m_transferTimeoutMs);
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
