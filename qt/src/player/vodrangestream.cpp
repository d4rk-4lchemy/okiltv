#include "vodrangestream.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace OKILTV::Player {
QMutex VodRangeStream::s_mutex;
QHash<QString, std::weak_ptr<VodRangeStream>> VodRangeStream::s_streams;

VodRangeStream::VodRangeStream(QUrl url, QByteArray userAgent)
    : m_effectiveUrl(std::move(url)), m_userAgent(std::move(userAgent)),
      m_virtualUrl(QStringLiteral("okiltv-vod://%1/media").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
    m_elapsed.start();
    m_worker = new QObject;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();
}
VodRangeStream::Ptr VodRangeStream::create(const QUrl &url, const QByteArray &userAgent)
{
    Ptr stream(new VodRangeStream(url, userAgent), [](VodRangeStream *value) {
        if (QThread::currentThread() == value->thread()) delete value;
        else value->deleteLater();
    });
    QMutexLocker lock(&s_mutex);
    s_streams.insert(stream->virtualUrl(), stream);
    return stream;
}
VodRangeStream::Ptr VodRangeStream::find(const QString &url)
{
    QMutexLocker lock(&s_mutex);
    return s_streams.value(url).lock();
}
VodRangeStream::~VodRangeStream()
{
    cancel();
    QMetaObject::invokeMethod(m_worker, [this]() { closeReply(); }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
    QMutexLocker lock(&s_mutex);
    s_streams.remove(m_virtualUrl);
}
void VodRangeStream::cancel()
{
    m_cancelled.store(true);
    // Also close a paused/backpressured response when no read is in progress.
    // During fetch its interrupt timer owns cancellation, avoiding reentrancy.
    QMetaObject::invokeMethod(m_worker, [this]() { if (!m_fetching) closeReply(); }, Qt::QueuedConnection);
}
void VodRangeStream::closeReply()
{
    auto *reply = std::exchange(m_reply, nullptr);
    if (reply) {
        if (!reply->isFinished()) reply->abort();
        delete reply;
        m_network->clearConnectionCache();
    }
    m_replyPosition = -1;
    m_replyValidated = false;
}

void VodRangeStream::prepare(std::function<void(bool)> completion)
{
    QMetaObject::invokeMethod(m_worker, [this, completion = std::move(completion)]() {
        auto first = fetch(0);
        QMetaObject::invokeMethod(this, [this, first = std::move(first), completion]() {
            const bool ready = !first.isEmpty() && !m_cancelled.load();
            if (ready) m_blocks.push_front({0, first});
            completion(ready);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

bool VodRangeStream::validateReply(qint64 offset)
{
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (m_size < 0) m_nativeFallback = status == 200 || status == 416;
    static const QRegularExpression range(QStringLiteral("^bytes ([0-9]+)-([0-9]+)/([0-9]+)$"));
    const auto match = range.match(QString::fromLatin1(m_reply->rawHeader("Content-Range")));
    bool startOk = false, endOk = false, sizeOk = false;
    const auto start = match.captured(1).toLongLong(&startOk);
    const auto end = match.captured(2).toLongLong(&endOk);
    const auto total = match.captured(3).toLongLong(&sizeOk);
    const auto length = m_reply->rawHeader("Content-Length");
    bool lengthOk = false;
    const auto declaredLength = length.toLongLong(&lengthOk);
    const bool valid = status == 206 && startOk && endOk && sizeOk && start == offset && total > offset
        && total <= std::numeric_limits<qint64>::max() - BlockSize && end == total - 1
        && (length.isEmpty() || (lengthOk && declaredLength == total - offset))
        && (m_size < 0 || total == m_size)
        && (m_validator.isEmpty() || m_reply->rawHeader(m_validatorHeader) == m_validator);
    if (valid && m_size < 0) {
        m_size = total;
        const auto etag = m_reply->rawHeader("ETag");
        m_validatorHeader = !etag.isEmpty() && !etag.startsWith("W/") ? "ETag" : "Last-Modified";
        m_validator = m_reply->rawHeader(m_validatorHeader);
    }
    if (valid) m_effectiveUrl = m_reply->url();
    return valid;
}

QByteArray VodRangeStream::fetch(qint64 offset)
{
    QElapsedTimer duration;
    duration.start();
    qint64 headersMs = -1;
    qint64 firstByteMs = -1;
    const bool continuing = m_reply && m_replyPosition == offset;
    QScopedValueRollback fetching(m_fetching, true);
    if (m_cancelled.load()) { closeReply(); return {}; }
    if (!m_network) m_network = new QNetworkAccessManager(m_worker);
    if (!m_reply || m_replyPosition != offset) {
        // One upstream response at a time. A sequential cache miss continues
        // reading the existing response; only a real jump reopens HTTP.
        const bool replacingActiveResponse = m_reply && !m_reply->isFinished();
        closeReply();
        if (replacingActiveResponse) {
            // abort() completes the reply immediately, but socket teardown is
            // dispatched to Qt's HTTP thread. Give that teardown time to reach
            // the peer before opening the replacement on single-slot sources.
            QEventLoop release;
            QTimer::singleShot(100, &release, &QEventLoop::quit);
            release.exec();
        }
        if (m_cancelled.load()) return {};
        QNetworkRequest request(m_effectiveUrl);
        request.setRawHeader("Range", "bytes=" + QByteArray::number(offset) + '-');
        request.setRawHeader("Accept-Encoding", "identity");
        request.setRawHeader("User-Agent", m_userAgent);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setMaximumRedirectsAllowed(5);
        if (!m_validator.isEmpty()) request.setRawHeader("If-Range", m_validator);
        m_reply = m_network->get(request);
        // Backpressure stops read-ahead without ending the provider response.
        m_reply->setReadBufferSize(BlockSize);
        m_replyPosition = offset;
    }
    QEventLoop loop;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer interrupt;
    interrupt.setInterval(25);
    QByteArray bytes;
    bool invalid = false;
    qint64 wanted = BlockSize;
    const auto consume = [&]() {
        if (!m_reply) return;
        if (!m_replyValidated) {
            if (!m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid()) return;
            // Qt may publish redirect metadata before following the redirect.
            const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status >= 300 && status < 400 && !m_reply->isFinished()) return;
            if (!validateReply(offset)) { invalid = true; loop.quit(); return; }
            m_replyValidated = true;
            headersMs = duration.elapsed();
        }
        wanted = std::min(BlockSize, m_size - offset);
        if (m_reply->bytesAvailable() > m_size - offset - bytes.size()
            || m_reply->error() != QNetworkReply::NoError) {
            invalid = true; loop.quit(); return;
        }
        if (m_reply->isOpen()) bytes += m_reply->read(wanted - bytes.size());
        if (!bytes.isEmpty() && firstByteMs < 0) firstByteMs = duration.elapsed();
        if (bytes.size() == wanted || m_reply->isFinished()) loop.quit();
    };
    connect(m_reply, &QNetworkReply::readyRead, &loop, consume);
    connect(m_reply, &QNetworkReply::metaDataChanged, &loop, consume);
    connect(m_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&interrupt, &QTimer::timeout, &loop, [&]() {
        if (m_cancelled.load() || elapsed.elapsed() >= 30000) { invalid = true; loop.quit(); }
    });
    interrupt.start();
    consume();
    if (!invalid && bytes.size() < wanted && !m_reply->isFinished()) loop.exec();
    if (!invalid) consume();
    const bool valid = !invalid && !m_cancelled.load() && m_replyValidated && bytes.size() == wanted;
    if (m_trace) {
        qInfo("VOD range: elapsed=%lld offset=%lld bytes=%lld continuing=%d headers=%lld first-byte=%lld fetch=%lld valid=%d",
            static_cast<long long>(m_elapsed.elapsed()), static_cast<long long>(offset),
            static_cast<long long>(bytes.size()), continuing, static_cast<long long>(headersMs),
            static_cast<long long>(firstByteMs), static_cast<long long>(duration.elapsed()), valid);
    }
    if (!valid) {
        if (!m_cancelled.load() && !m_nativeFallback) {
            m_failed.store(true);
            qWarning("VOD range read rejected: HTTP %d, offset %lld, bytes %lld, network error %d.",
                m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                static_cast<long long>(offset), static_cast<long long>(bytes.size()), static_cast<int>(m_reply->error()));
        }
        closeReply();
        return {};
    }
    m_replyPosition += bytes.size();
    if (m_replyPosition == m_size) closeReply();
    return bytes;
}

qint64 VodRangeStream::read(char *destination, quint64 maximum)
{
    if (m_cancelled.load()) return -1;
    if (m_position >= m_size || maximum == 0) return 0;
    const auto offset = m_position / BlockSize * BlockSize;
    auto block = std::find_if(m_blocks.begin(), m_blocks.end(), [offset](const Block &entry) { return entry.offset == offset; });
    if (block == m_blocks.end()) {
        QByteArray data;
        QMetaObject::invokeMethod(m_worker, [this, offset, &data]() { data = fetch(offset); }, Qt::BlockingQueuedConnection);
        if (data.isEmpty() || m_cancelled.load()) return -1;
        m_blocks.push_front({offset, std::move(data)});
        if (m_blocks.size() > MaxBlocks) m_blocks.pop_back();
    } else {
        m_blocks.splice(m_blocks.begin(), m_blocks, block);
    }
    const auto &bytes = m_blocks.front().data;
    const auto within = m_position - offset;
    const auto count = static_cast<qint64>(std::min(maximum, static_cast<quint64>(bytes.size() - within)));
    std::memcpy(destination, bytes.constData() + within, static_cast<size_t>(count));
    m_position += count;
    return count;
}
qint64 VodRangeStream::seek(qint64 offset)
{
    if (m_cancelled.load() || offset < 0 || offset > m_size) return -1;
    m_position = offset;
    return offset;
}
}
