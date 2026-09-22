#include "catchupdownloadtransfer.h"
#include "../core/debuglogger.h"

#include <QNetworkRequest>
#include <QRegularExpression>

#include <algorithm>

namespace OKILTV::App {
namespace {
constexpr qint64 kReadBlock = 256LL * 1024;
constexpr qint64 kReadBuffer = 1024LL * 1024;
constexpr qint64 kByteOverlap = 64LL * 1024;
constexpr qint64 kTimeSignature = 32LL * 188;
constexpr qint64 kMaxTimeOverlapBytes = 256LL * 1024 * 1024;
const QString kUnsupported = QStringLiteral("The server returned an unsupported archive format or HLS resource.");
const QString kCannotResume = QStringLiteral("This server cannot resume this download without transferring it again. Downloaded data has been retained.");
bool strongEtag(const QByteArray &tag)
{
    return tag.size() >= 2 && tag.startsWith('"') && tag.endsWith('"');
}
} // namespace

void CatchupDownloadClock::push(const QByteArray &data)
{
    // Small blocks bound the uncertainty at an unannounced clock reset. The
    // source itself is never rewritten or truncated by this observer.
    constexpr qsizetype blockSize = 32LL * 188;
    for (qsizetype offset = 0; offset < data.size(); offset += blockSize) {
        if (!m_clock.valid())
            return; // Not recognized as MPEG-TS; retain byte-based progress.
        const auto block = data.mid(offset, blockSize);
        m_clock.push(block);
        m_known |= m_clock.durationSeconds() > 0;
        if (m_clock.periodPending()) {
            m_elapsed += m_clock.periodDurationSeconds();
            const auto next = m_clock.takeNextPeriod();
            m_clock = {};
            m_clock.push(next);
        } else if (!m_clock.valid() && m_known) {
            m_elapsed += m_clock.durationSeconds();
            m_clock = {};
            m_clock.push(block);
            if (!m_clock.valid()) {
                m_elapsed += m_clock.durationSeconds();
                m_clock = {};
            }
        }
    }
}

CatchupDownloadTransfer::CatchupDownloadTransfer(QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)), m_timeout(new QTimer(this)),
      m_progressTimer(new QTimer(this)), m_resumeRetryTimer(new QTimer(this))
{
    m_file.setParent(this);
    m_progressTimer->setSingleShot(true);
    connect(m_progressTimer, &QTimer::timeout, this, [this] { reportProgress(true); });
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(60000);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        failResponse(QStringLiteral("Download paused: no network progress for 60 seconds. Press Resume to retry."), true);
    });
    m_resumeRetryTimer->setSingleShot(true);
    m_resumeRetryTimer->setTimerType(Qt::PreciseTimer);
    connect(m_resumeRetryTimer, &QTimer::timeout, this, [this] {
        start(m_token, m_url, m_path, m_userAgent, m_allowHls, m_timeline);
    });
}

double CatchupDownloadTransfer::mediaFraction() const
{
    if (m_hlsResponse || m_timeline.durationSeconds <= 0 || !m_clock.known())
        return -1;
    return std::clamp((m_clock.durationSeconds() - static_cast<double>(m_timeline.trimStartSeconds))
        / static_cast<double>(m_timeline.durationSeconds), 0.0, 1.0);
}

bool CatchupDownloadTransfer::prepareTimeResume(QUrl &url)
{
    if (!m_timeline.resumeUrl || m_hlsResponse || !m_clock.known() || m_bytes < kTimeSignature)
        return false;
    // Providers round archive starts to minutes. Replay at most the last minute
    // plus eight seconds, never the entire downloaded prefix.
    const auto offset = std::max<qint64>(0, (static_cast<qint64>(m_clock.durationSeconds()) - 8) / 60 * 60);
    const auto next = m_timeline.resumeUrl(offset);
    if (!supportedUrl(next, false) || (offset > 0 && next == m_url))
        return false;
    if (!m_file.seek(m_bytes - kTimeSignature))
        return false;
    m_signature = m_file.read(kTimeSignature);
    if (m_signature.size() != kTimeSignature)
        return false;
    url = next;
    Core::DebugLogger::instance().log(QStringLiteral("download"),
        QStringLiteral("Preparing archive resume offsetSeconds=%1 savedBytes=%2").arg(offset).arg(m_bytes));
    m_timeResume = true;
    m_joined = true;
    m_total = -1;
    m_etag.clear();
    return true;
}

bool CatchupDownloadTransfer::supportedUrl(const QUrl &url, bool allowHls)
{
    const auto path = url.path().toLower();
    return url.isValid() && !url.host().isEmpty() && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"))
        && (allowHls || (!path.endsWith(QStringLiteral(".m3u8")) && !path.endsWith(QStringLiteral(".m3u"))))
        && !path.endsWith(QStringLiteral(".mpd"));
}

void CatchupDownloadTransfer::close()
{
    m_timeout->stop();
    m_progressTimer->stop();
    m_resumeRetryTimer->stop();
    if (m_reply) {
        auto *reply = m_reply.data();
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    m_file.close();
}

// Named file path and HTTP user-agent inputs are kept together at all call sites.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void CatchupDownloadTransfer::start(quint64 token, const QUrl &url, const QString &path, const QString &userAgent,
                                   bool allowHls, const DownloadTimeline &timeline)
{
    close();
    if (token != m_token || m_path != path)
        m_resumeRetryCount = 0;
    m_token = token;
    m_userAgent = userAgent;
    m_allowHls = allowHls;
    m_timeline = timeline;
    if (m_path != path) {
        m_path = path;
        m_etag.clear();
        m_total = -1;
        m_complete = false;
        m_bytes = 0;
        m_hlsResponse = false;
        m_clock = {};
        m_joined = false;
    }
    if (m_url != url)
        m_etag.clear();
    m_url = url;
    if (m_complete) {
        emit completed(m_token);
        return;
    }
    if (!supportedUrl(url, m_allowHls)) {
        fail(kUnsupported);
        return;
    }
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::ReadWrite)) {
        fail(QStringLiteral("Cannot open the download working file."));
        return;
    }
    m_bytes = m_file.size();
    m_savedBytes = m_bytes;
    m_prefix = m_file.read(4096);
    m_timeResume = false;
    m_overlapClock = {};
    m_signature.clear();
    m_search.clear();
    m_received = 0;
    m_responseLength = -1;
    QUrl requestUrl = url;
    if (m_bytes > 0 && (m_joined || !strongEtag(m_etag)))
        prepareTimeResume(requestUrl);
    if (m_joined && !m_timeResume) {
        failResponse(kCannotResume);
        return;
    }
    m_range = m_bytes > 0 && !m_timeResume;
    m_rangeStart = strongEtag(m_etag) ? m_bytes : std::max<qint64>(0, m_bytes - kByteOverlap);
    m_cursor = m_range ? m_rangeStart : 0;
    m_inspected = false;
    QNetworkRequest request(requestUrl);
    if (!userAgent.isEmpty())
        request.setHeader(QNetworkRequest::UserAgentHeader, userAgent);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(10);
    if (m_range) {
        request.setRawHeader("Range", "bytes=" + QByteArray::number(m_rangeStart) + '-');
        if (strongEtag(m_etag))
            request.setRawHeader("If-Range", m_etag);
    }
    m_reply = m_network->get(request);
    Core::DebugLogger::instance().log(QStringLiteral("download"),
        QStringLiteral("Transfer start savedBytes=%1 mode=%2 mediaSeconds=%3")
            .arg(m_bytes).arg(m_timeResume ? QStringLiteral("time-overlap")
                : m_range ? QStringLiteral("byte-range") : QStringLiteral("initial"))
            .arg(m_clock.durationSeconds(), 0, 'f', 2));
    m_reply->setReadBufferSize(kReadBuffer);
    connect(m_reply, &QNetworkReply::readyRead, this, &CatchupDownloadTransfer::drain);
    connect(m_reply, &QNetworkReply::finished, this, &CatchupDownloadTransfer::finishReply);
    m_timeout->start();
    reportProgress(true);
}

bool CatchupDownloadTransfer::inspectResponse()
{
    if (m_inspected)
        return true;
    if (!m_reply)
        return false;
    const auto status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0)
        return false;
    if (m_range && (status == 200 || status == 416)) {
        // A provider can ignore ranges even with an ETag. Switch to its time
        // window before reading the body; do not silently replay the prefix.
        QUrl next;
        if (prepareTimeResume(next)) {
            const auto agent = m_reply->request().header(QNetworkRequest::UserAgentHeader).toString();
            start(m_token, m_url, m_path, agent, m_allowHls, m_timeline);
            return false;
        }
        if (status == 416 || m_savedBytes > kByteOverlap) {
            failResponse(kCannotResume);
            return false;
        }
    }
    if (status != 200 && status != 206) {
        failResponse(QStringLiteral("The archive server rejected the download (HTTP %1).").arg(status),
             status == 408 || status == 429 || status >= 500);
        return false;
    }
    const auto contentType = m_reply->rawHeader("Content-Type").toLower();
    m_hlsResponse = contentType.contains("mpegurl");
    if (!supportedUrl(m_reply->url(), m_allowHls) || (!m_allowHls && m_hlsResponse) || contentType.contains("dash+xml")) {
        fail(kUnsupported);
        return false;
    }
    const auto encoding = m_reply->rawHeader("Content-Encoding").trimmed().toLower();
    if (!encoding.isEmpty() && encoding != "identity") {
        fail(QStringLiteral("The server returned an unsupported content encoding."));
        return false;
    }
    bool lengthOk = false;
    const auto length = m_reply->rawHeader("Content-Length").toLongLong(&lengthOk);
    m_responseLength = lengthOk ? length : -1;
    const auto etag = m_reply->rawHeader("ETag").trimmed();
    if (status == 206 && m_reply->url() != m_responseUrl) {
        failResponse(QStringLiteral("Cannot resume: the archive resource changed."));
        return false;
    }
    if (status == 206) {
        static const QRegularExpression rangePattern(QStringLiteral(R"(^bytes (\d+)-(\d+)/(\d+)$)"));
        const auto match = rangePattern.match(QString::fromLatin1(m_reply->rawHeader("Content-Range")));
        bool startOk = false;
        bool endOk = false;
        bool totalOk = false;
        const auto start = match.captured(1).toLongLong(&startOk);
        const auto end = match.captured(2).toLongLong(&endOk);
        const auto total = match.captured(3).toLongLong(&totalOk);
        if (!m_range || !match.hasMatch() || !startOk || !endOk || !totalOk
            || start != m_rangeStart || end < start || total <= end || end != total - 1
            || (lengthOk && length != end - start + 1)
            || (strongEtag(m_etag) && etag != m_etag) || (m_total >= 0 && total != m_total)) {
            failResponse(QStringLiteral("Cannot resume: the server returned an inconsistent byte range or changed archive."));
            return false;
        }
        m_total = total;
        m_responseLength = end - start + 1;
    } else if (!m_timeResume) {
        // Only a small bounded overlap may be replayed when ranges are ignored.
        m_cursor = 0;
        if (lengthOk && (length < m_savedBytes || (m_total >= 0 && length != m_total))) {
            failResponse(QStringLiteral("Cannot resume: the archive length changed."));
            return false;
        }
        m_total = lengthOk ? length : -1;
    }
    m_responseUrl = m_reply->url();
    m_etag = !m_joined && strongEtag(etag) ? etag : QByteArray();
    m_inspected = true;
    reportProgress(true);
    return true;
}

void CatchupDownloadTransfer::reportProgress(bool force)
{
    if (!force && m_progressClock.isValid() && m_progressClock.elapsed() < 100) {
        if (!m_progressTimer->isActive())
            m_progressTimer->start(100);
        return;
    }
    m_progressTimer->stop();
    m_progressClock.restart();
    emit progress(m_token, m_bytes, m_total,
                  m_resumeRetryTimer->isActive() || !m_signature.isEmpty() || m_cursor < m_savedBytes);
}

void CatchupDownloadTransfer::drain()
{
    if (!inspectResponse() || !m_reply)
        return;
    // Bound each event-loop turn, including disk comparison, so pause/cancel
    // cannot sit behind a whole archive download on the worker queue.
    auto data = m_reply->read(kReadBlock);
    if (data.isEmpty())
        return;
    m_timeout->start();
    if (m_responseLength >= 0 && data.size() > m_responseLength - m_received) {
        failResponse(QStringLiteral("The archive response exceeded its declared length."));
        return;
    }
    m_received += data.size();
    if (!m_signature.isEmpty()) {
        const auto retained = m_search.size();
        m_search += data;
        const auto match = m_search.indexOf(m_signature);
        const auto inspected = match < 0 ? data.size()
            : std::clamp<qsizetype>(match + m_signature.size() - retained, 0, data.size());
        m_overlapClock.push(data.first(inspected));
        if (m_overlapClock.durationSeconds() > 90 || m_received >= kMaxTimeOverlapBytes) {
            Core::DebugLogger::instance().log(QStringLiteral("download"),
                QStringLiteral("Archive overlap exceeded bound mediaSeconds=%1 receivedBytes=%2 match=%3")
                    .arg(m_overlapClock.durationSeconds()).arg(m_received).arg(match));
            failResponse(QStringLiteral("Cannot resume: the server did not return the requested archive overlap. Downloaded data has been retained."));
            return;
        }
        if (match < 0) {
            m_search = m_search.right(m_signature.size() - 1);
            data.clear();
        } else {
            Core::DebugLogger::instance().log(QStringLiteral("download"),
                QStringLiteral("Archive overlap verified receivedBytes=%1 savedBytes=%2")
                    .arg(m_received).arg(m_savedBytes));
            data = m_search.mid(match + m_signature.size());
            m_signature.clear();
            m_search.clear();
            m_cursor = m_savedBytes;
        }
    }
    const auto compareBytes = std::min<qint64>(data.size(), std::max<qint64>(0, m_savedBytes - m_cursor));
    if (compareBytes > 0) {
        if (!m_file.seek(m_cursor) || m_file.read(compareBytes) != data.first(compareBytes)) {
            failResponse(QStringLiteral("Cannot resume: the archive content changed. Downloaded data has been retained."));
            return;
        }
    }
    const auto tail = data.sliced(compareBytes);
    if (!tail.isEmpty()) {
        if (m_prefix.size() < 4096) {
            m_prefix.append(tail.first(std::min<qsizetype>(tail.size(), 4096 - m_prefix.size())));
            auto prefix = m_prefix.trimmed().toLower();
            if (prefix.startsWith(QByteArray::fromHex("efbbbf")))
                prefix = prefix.mid(3).trimmed();
            m_hlsResponse |= prefix.startsWith("#extm3u");
            if ((!m_allowHls && m_hlsResponse) || (prefix.startsWith('<') && prefix.contains("<mpd"))) {
                fail(kUnsupported);
                return;
            }
        }
        if (m_allowHls && m_hlsResponse && m_bytes + tail.size() > 8LL * 1024 * 1024) {
            fail(QStringLiteral("The HLS playlist exceeds the supported size."));
            return;
        }
        if (!m_file.seek(m_bytes) || m_file.write(tail) != tail.size()) {
            fail(QStringLiteral("Cannot write download data. Check disk space and destination permissions."));
            return;
        }
        m_bytes += tail.size();
        if (m_timeline.durationSeconds > 0 && !m_hlsResponse)
            m_clock.push(tail); // Observe only newly saved bytes; keep the source representation unchanged.
    }
    m_cursor += data.size();
    if (mediaEndReached()) {
        completeTransfer();
        return;
    }
    reportProgress();
    if (m_reply && (m_reply->bytesAvailable() > 0 || m_reply->isFinished())) {
        const auto token = m_token;
        QTimer::singleShot(0, this, [this, token] {
            if (token != m_token || !m_reply)
                return;
            if (m_reply->isFinished())
                finishReply();
            else
                drain();
        });
    }
}

bool CatchupDownloadTransfer::flush()
{
    if (!m_file.isOpen() || m_file.flush())
        return true;
    fail(QStringLiteral("Cannot flush the download working file. Check disk space."));
    return false;
}

bool CatchupDownloadTransfer::mediaEndReached() const
{
    return m_timeline.finishAtMediaEnd && !m_hlsResponse && m_timeline.durationSeconds > 0
        && m_clock.known() && m_signature.isEmpty() && m_cursor >= m_savedBytes
        && m_clock.durationSeconds() >= static_cast<double>(m_timeline.durationSeconds + m_timeline.trimStartSeconds);
}

void CatchupDownloadTransfer::completeTransfer()
{
    if (!flush())
        return;
    const auto token = m_token;
    reportProgress(true);
    if (token != m_token || !m_reply)
        return;
    m_complete = true;
    close();
    emit completed(m_token);
}

void CatchupDownloadTransfer::finishReply()
{
    if (!m_reply)
        return;
    if (m_reply->bytesAvailable() > 0) {
        drain();
        return;
    }
    const auto error = m_reply->error();
    if (!inspectResponse()) {
        if (m_reply)
            failResponse(QStringLiteral("Download connection failed. Press Resume to retry."), true);
        return;
    }
    if (!m_reply)
        return;
    if (error != QNetworkReply::NoError) {
        failResponse(QStringLiteral("Download connection was interrupted. Press Resume to retry."), true);
        return;
    }
    if (!m_signature.isEmpty()) {
        failResponse(QStringLiteral("Cannot resume: the archive overlap was not found. Downloaded data has been retained."));
        return;
    }
    if (m_cursor < m_savedBytes || (m_responseLength >= 0 && m_received != m_responseLength)) {
        failResponse(QStringLiteral("The archive response ended before all expected data arrived."), true);
        return;
    }
    completeTransfer();
}

void CatchupDownloadTransfer::failResponse(const QString &reason, bool retryable)
{
    // A provider may briefly serve the previous archive window after an abort.
    // Retry only a resume that has not appended anything, preserving the exact
    // saved boundary and all validation on subsequent responses.
    if (m_savedBytes > 0 && m_bytes == m_savedBytes && m_resumeRetryCount < 2) {
        if (!flush())
            return;
        const auto delaySeconds = m_resumeRetryCount == 0 ? 10 : 30;
        ++m_resumeRetryCount;
        close();
        m_network->clearConnectionCache();
        Core::DebugLogger::instance().log(QStringLiteral("download"),
            QStringLiteral("Resume response rejected; retry %1/2 after %2 seconds: %3")
                .arg(m_resumeRetryCount).arg(delaySeconds).arg(reason));
        m_resumeRetryTimer->start(delaySeconds * 1000);
        reportProgress(true);
        return;
    }
    fail(reason, retryable && m_resumeRetryCount == 0);
}

void CatchupDownloadTransfer::fail(const QString &reason, bool retryable)
{
    if (m_file.isOpen() && !m_file.flush())
        retryable = false;
    reportProgress(true);
    close();
    if (retryable)
        emit paused(m_token, reason);
    else
        emit failed(m_token, reason);
}

void CatchupDownloadTransfer::pause(quint64 token)
{
    m_token = token;
    if (m_complete) {
        emit completed(m_token);
        return;
    }
    // A pause may beat the queued finished signal after the last body byte.
    // Finalize locally instead of resuming with an unsatisfiable Range N-.
    if (m_inspected && ((m_total > 0 && m_bytes == m_total && m_cursor == m_total) || mediaEndReached())) {
        if (!flush())
            return;
        m_complete = true;
        close();
        emit completed(m_token);
        return;
    }
    if (!flush())
        return;
    reportProgress(true);
    close();
    emit paused(m_token, {});
}

void CatchupDownloadTransfer::stop(quint64 token)
{
    m_token = token;
    close();
    emit stopped(m_token);
}

} // namespace OKILTV::App
