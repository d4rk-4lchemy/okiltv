#pragma once

#include <QHash>
#include <QElapsedTimer>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QObject>
#include <QThread>
#include <QUrl>
#include <atomic>
#include <functional>
#include <list>
#include <memory>

namespace OKILTV::Player {
// One finite HTTP resource, with a bounded byte cache shared by its audio/video
// reads. Network objects live on a Qt-owned thread; mpv's IO thread may block,
// but cancellation never waits for the GUI event loop.
class VodRangeStream final : public QObject {
public:
    using Ptr = std::shared_ptr<VodRangeStream>;
    static Ptr create(const QUrl &url, const QByteArray &userAgent);
    static Ptr find(const QString &url);
    ~VodRangeStream() override;
    void prepare(std::function<void(bool)> completion);
    QString virtualUrl() const { return m_virtualUrl; }
    qint64 read(char *destination, quint64 maximum);
    qint64 seek(qint64 offset);
    qint64 size() const { return m_size; }
    bool nativeFallbackAllowed() const { return m_nativeFallback; }
    bool failed() const { return m_failed.load(); }
    void cancel();
    bool claimReader() { return !m_readerClaimed.exchange(true); }

private:
    VodRangeStream(QUrl url, QByteArray userAgent);
    QByteArray fetch(qint64 offset); // Network thread only.
    void closeReply(); // Network thread only; before opening another response.
    bool validateReply(qint64 offset);
    static constexpr qint64 BlockSize = qint64(256) * 1024;
    static constexpr size_t MaxBlocks = 64; // 16 MiB; no disk/media persistence.
    struct Block { qint64 offset; QByteArray data; };
    QUrl m_effectiveUrl;
    const QByteArray m_userAgent;
    const QString m_virtualUrl;
    const bool m_trace = qEnvironmentVariableIntValue("OKILTV_TRACE_VOD_RANGE") == 1;
    QElapsedTimer m_elapsed;
    QThread m_thread;
    QObject *m_worker = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_reply = nullptr;
    qint64 m_replyPosition = -1;
    bool m_replyValidated = false;
    bool m_fetching = false;
    std::atomic_bool m_cancelled{false};
    std::atomic_bool m_readerClaimed{false};
    std::atomic_bool m_failed{false};
    qint64 m_size = -1;
    bool m_nativeFallback = false;
    qint64 m_position = 0;
    QByteArray m_validator;
    QByteArray m_validatorHeader;
    std::list<Block> m_blocks;
    static QMutex s_mutex;
    static QHash<QString, std::weak_ptr<VodRangeStream>> s_streams;
};
}
