#pragma once

#include <QFile>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>

#include "../player/catchuptsjoiner.h"

namespace OKILTV::App {

struct DownloadTimeline {
    qint64 durationSeconds { 0 };
    qint64 trimStartSeconds { 0 };
    // Receives a stream-relative offset; the caller owns provider formatting.
    std::function<QUrl(qint64)> resumeUrl;
    // Finite export may stop after enough observed media even if the provider
    // keeps sending archive data beyond the requested programme.
    bool finishAtMediaEnd { false };
};

// Read-only duration observation. Broadcast clock/configuration resets begin a
// new period; strict playback continuity policy stays in CatchupTsJoiner.
class CatchupDownloadClock
{
public:
    void push(const QByteArray &data);
    double durationSeconds() const { return m_elapsed + m_clock.durationSeconds(); }
    bool known() const { return m_known; }
private:
    Player::CatchupTsJoiner m_clock;
    double m_elapsed { 0 };
    bool m_known { false };
};

// Lives entirely on the download thread. The file contains the unmodified HTTP
// representation; it is never an MKV until the controller remuxes it locally.
class CatchupDownloadTransfer final : public QObject
{
    Q_OBJECT
public:
    explicit CatchupDownloadTransfer(QObject *parent = nullptr);
    void start(quint64 token, const QUrl &url, const QString &path, const QString &userAgent,
               bool allowHls = false, const DownloadTimeline &timeline = {});
    void pause(quint64 token);
    void stop(quint64 token);
    void close();
    static bool supportedUrl(const QUrl &url, bool allowHls = false);
    QUrl responseUrl() const { return m_responseUrl; }
    bool hlsResponse() const { return m_hlsResponse; }
    double mediaFraction() const;
signals:
    void progress(quint64 token, qint64 bytes, qint64 total, bool comparing);
    void paused(quint64 token, const QString &reason);
    void failed(quint64 token, const QString &reason);
    void completed(quint64 token);
    void stopped(quint64 token);
private:
    bool inspectResponse();
    void drain();
    void finishReply();
    bool mediaEndReached() const;
    void completeTransfer();
    void fail(const QString &reason, bool retryable = false);
    void failResponse(const QString &reason, bool retryable = false);
    bool flush();
    void reportProgress(bool force = false);
    bool prepareTimeResume(QUrl &url);
    QElapsedTimer m_progressClock;
    QNetworkAccessManager *m_network;
    QTimer *m_timeout;
    QTimer *m_progressTimer;
    QTimer *m_resumeRetryTimer;
    QPointer<QNetworkReply> m_reply;
    QFile m_file;
    QString m_path;
    QString m_userAgent;
    QUrl m_url;
    QUrl m_responseUrl;
    bool m_complete { false };
    bool m_allowHls { false };
    bool m_hlsResponse { false };
    QByteArray m_etag;
    QByteArray m_prefix;
    qint64 m_bytes { 0 };
    qint64 m_total { -1 };
    qint64 m_cursor { 0 };
    qint64 m_savedBytes { 0 };
    quint64 m_token { 0 };
    bool m_range { false };
    bool m_inspected { false };
    DownloadTimeline m_timeline;
    CatchupDownloadClock m_clock;
    CatchupDownloadClock m_overlapClock;
    QByteArray m_signature;
    QByteArray m_search;
    qint64 m_rangeStart { 0 };
    qint64 m_received { 0 };
    qint64 m_responseLength { -1 };
    bool m_timeResume { false };
    bool m_joined { false };
    int m_resumeRetryCount { 0 };
};

} // namespace OKILTV::App
