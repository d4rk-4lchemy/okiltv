#pragma once

#include "mpegtstimestampnormalizer.h"
#include "catchuptsjoiner.h"
#include "../core/models.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPair>
#include <QPointer>
#include <QUrl>
#include <QTimer>
#include <QWaitCondition>

#include <atomic>
#include <deque>
#include <memory>

namespace OKILTV::Player {

class CatchupStreamSession final : public QObject, public std::enable_shared_from_this<CatchupStreamSession>
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
        bool normalizeMpegTsTimestamps;
        int transferTimeoutMs;
    };
    struct ContinuousPolicy
    {
        QString canonicalUrl;
        QDateTime programStartUtc;
        QDateTime programStopUtc;
        double streamBaseSeconds { 0.0 };
        int safetySeconds { 180 };
        double initialBufferSeconds { 30.0 };
        bool allowContinuation { true };
        bool endless { false };
        std::optional<Core::Channel> templateChannel;
    };

    explicit CatchupStreamSession(
        QString sourceUrl,
        HeaderList requestHeaders = {},
        const BufferingPolicy &bufferingPolicy = {});
    ~CatchupStreamSession() override;

    static Ptr create(
        const QString &sourceUrl,
        HeaderList requestHeaders = {},
        const BufferingPolicy &bufferingPolicy = {});
    static Ptr find(const QString &virtualUrl);
    static bool unregisterSession(const QString &virtualUrl);
    static HeaderList requestHeadersFromOptions(const QString &userAgent, const QMap<QString, QString> &options);

    QString sourceUrl() const;
    QString virtualUrl() const;
    bool start();
    void configureContinuous(ContinuousPolicy policy);
    void configureMediaPeriods(double streamBaseSeconds = 0.0);
    bool continuous() const;
    bool failedMediaTransport() const;
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
    quint64 readGeneration() const;
    qint64 read(quint64 generation, char *buffer, quint64 maxBytes);
    void cancelRead(quint64 generation);
    std::optional<double> nextPeriodBaseSeconds() const;
    bool advancePeriod();
    void allowRetriedForwardGap(std::optional<CatchupTsJoiner::ForwardGap> gap);
    std::optional<CatchupTsJoiner::ForwardGap> failedForwardGap() const;

private:
    void appendNetworkData();
    bool enqueueNetworkData(QByteArray data);
    void queueOutputLocked(const QByteArray &data = {});
    void finishNetwork();
    void failNetwork(const QString &message);
    void scheduleDrainOnReplyThread();
    void wakeReaders();
    bool openUrl(const QString &url);
    void finishContinuousReply();
    void openContinuation();

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
    const int m_transferTimeoutMs;
    std::optional<MpegTsTimestampNormalizer> m_timestampNormalizer;
    std::optional<ContinuousPolicy> m_continuousPolicy;
    CatchupTsJoiner m_joiner;
    bool m_mediaPeriods { false };
    double m_mediaPeriodBaseSeconds { 0.0 };
    QTimer m_continuationTimer;
    QElapsedTimer m_overlapTimer;
    double m_previousResponseDuration { -1.0 };
    int m_noProgressAttempts { 0 };
    bool m_minuteContinuation { false };
    QString m_lastContinuationUrl;
    QElapsedTimer m_noProgressTimer;
    bool m_readReady { true };
    quint64 m_readGeneration { 0 };
    QByteArray m_nextPeriodBytes;
    std::optional<double> m_nextPeriodBaseSeconds;
    std::optional<CatchupTsJoiner::ForwardGap> m_failedForwardGap;
    mutable QMutex m_mutex;
    QWaitCondition m_dataAvailable;
    std::deque<QByteArray> m_chunks;
    QByteArray m_pendingOutput;
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
    QTimer m_networkIdleTimer;
    QPointer<QNetworkReply> m_reply;
};

} // namespace OKILTV::Player
