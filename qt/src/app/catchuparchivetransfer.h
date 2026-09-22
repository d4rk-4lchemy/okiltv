#pragma once

#include "catchupdownloadtransfer.h"
#include <QHash>

namespace OKILTV::App {

// Downloads direct media or a finite HLS resource graph serially on the download
// thread. Rewritten playlists refer only to generated local names; ffmpeg never
// receives provider URLs. Pausing preserves completed resources and the active
// HTTP transfer's verified byte-resume state.
class CatchupArchiveTransfer final : public QObject
{
    Q_OBJECT
public:
    explicit CatchupArchiveTransfer(QObject *parent = nullptr);
    void start(quint64 token, const QUrl &url, const QString &path, const QString &userAgent,
               const DownloadTimeline &timeline = {});
    void pause(quint64 token);
    void stop(quint64 token);
    void close();
signals:
    void progress(quint64 token, qint64 bytes, qint64 total, bool comparing, double fraction);
    void paused(quint64 token, const QString &reason);
    void failed(quint64 token, const QString &reason);
    void completed(quint64 token, bool hls);
    void stopped(quint64 token);
private:
    enum class Kind { Root, Playlist, Media, Key };
    struct Resource { QUrl url; QString path; Kind kind; };
    void next();
    void resourceCompleted();
    bool rewritePlaylist(const Resource &resource, const QUrl &baseUrl);
    QString reference(const QString &uri, const Resource &parent, const QUrl &baseUrl, Kind kind);
    void fail(const QString &reason);
    CatchupDownloadTransfer *m_http;
    QList<Resource> m_resources;
    QHash<QUrl, int> m_byUrl;
    QString m_path;
    QString m_agent;
    QString m_error;
    quint64 m_token { 0 };
    int m_index { 0 };
    qint64 m_savedBytes { 0 };
    bool m_hls { false };
    bool m_running { false };
    bool m_resourceActive { false };
    DownloadTimeline m_timeline;
};

} // namespace OKILTV::App
