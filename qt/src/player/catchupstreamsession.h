#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPair>
#include <QPointer>
#include <QUrl>
#include <QWaitCondition>

#include <atomic>
#include <deque>
#include <memory>

namespace OKILTV::Player {

class CatchupStreamSession final : public std::enable_shared_from_this<CatchupStreamSession>
{
public:
    using Ptr = std::shared_ptr<CatchupStreamSession>;
    using HeaderList = QList<QPair<QByteArray, QByteArray>>;
    struct BufferingPolicy
    {
        qsizetype queueHighWaterBytes;
        qsizetype queueLowWaterBytes;
        qint64 replyReadBufferBytes;
        QString roleLabel;
    };

    explicit CatchupStreamSession(
        QString sourceUrl,
        HeaderList requestHeaders = {},
        BufferingPolicy bufferingPolicy = {});
    ~CatchupStreamSession();

    static Ptr create(
        const QString &sourceUrl,
        HeaderList requestHeaders = {},
        BufferingPolicy bufferingPolicy = {});
    static Ptr find(const QString &virtualUrl);
    static bool unregisterSession(const QString &virtualUrl);

    QString sourceUrl() const;
    QString virtualUrl() const;
    bool start();
    void closeProviderConnection(const QString &reason);
    bool closeRequestedByApp() const;
    QString closeRequestReason() const;
    bool providerConnectionClosed() const;
    bool hasNetworkError() const;
    QString errorString() const;
    qsizetype bufferedBytes() const;
    qsizetype peakBufferedBytes() const;

    qint64 read(char *buffer, quint64 maxBytes);
    void cancelRead();

private:
    void appendNetworkData();
    void finishNetwork();
    void failNetwork(const QString &message);
    void scheduleDrainOnReplyThread();
    void wakeReaders();

    static QMutex s_registryMutex;
    static QHash<QString, std::weak_ptr<CatchupStreamSession>> s_registry;
    static std::atomic_uint s_nextId;

    const QString m_id;
    const QString m_sourceUrl;
    const QString m_virtualUrl;
    const HeaderList m_requestHeaders;
    const qsizetype m_queueHighWaterBytes;
    const qsizetype m_queueLowWaterBytes;
    const qint64 m_replyReadBufferBytes;
    const QString m_roleLabel;
    mutable QMutex m_mutex;
    QWaitCondition m_dataAvailable;
    std::deque<QByteArray> m_chunks;
    qsizetype m_bufferedBytes { 0 };
    qsizetype m_peakBufferedBytes { 0 };
    qsizetype m_frontOffset { 0 };
    bool m_started { false };
    bool m_cancelled { false };
    bool m_providerClosed { false };
    bool m_replyFinished { false };
    bool m_networkFinished { false };
    bool m_networkError { false };
    bool m_abortExpected { false };
    bool m_closeRequestedByApp { false };
    bool m_backpressureActive { false };
    bool m_drainScheduled { false };
    QElapsedTimer m_closeRequestedTimer;
    QString m_closeRequestReason;
    QString m_errorString;
    QNetworkAccessManager m_networkAccess;
    QPointer<QNetworkReply> m_reply;
};

} // namespace OKILTV::Player
