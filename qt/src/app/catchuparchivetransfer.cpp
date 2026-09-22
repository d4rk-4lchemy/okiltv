#include "catchuparchivetransfer.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>

namespace OKILTV::App {
namespace {
QHash<QString, QString> attributes(const QString &line)
{
    static const QRegularExpression pattern(QStringLiteral(R"re((?:[:,])([A-Z0-9-]+)=("[^"]*"|[^,]*))re"));
    QHash<QString, QString> result;
    auto matches = pattern.globalMatch(line);
    while (matches.hasNext()) {
        const auto match = matches.next();
        auto value = match.captured(2);
        if (value.startsWith(u'"'))
            value = value.mid(1, value.size() - 2);
        result.insert(match.captured(1), value);
    }
    return result;
}
QString replaceUri(QString line, const QString &uri)
{
    static const QRegularExpression pattern(QStringLiteral(R"re(([:,])URI="[^"]*")re"));
    const auto match = pattern.match(line);
    if (!match.hasMatch() || pattern.match(line, match.capturedEnd()).hasMatch())
        return {};
    line.replace(match.capturedStart(), match.capturedLength(), match.captured(1) + QStringLiteral("URI=\"") + uri + u'"');
    return line;
}
} // namespace

CatchupArchiveTransfer::CatchupArchiveTransfer(QObject *parent)
    : QObject(parent), m_http(new CatchupDownloadTransfer(this))
{
    connect(m_http, &CatchupDownloadTransfer::progress, this,
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- HTTP signal argument order.
        [this](quint64 token, qint64 bytes, qint64 total, bool comparing) {
            m_hls |= m_http->hlsResponse();
            if (!m_running || token != m_token)
                return;
            double fraction = -1;
            if (!m_hls) {
                const auto mediaFraction = m_http->mediaFraction();
                fraction = mediaFraction;
                if (total > 0 && !(m_timeline.finishAtMediaEnd && mediaFraction >= 0)) {
                    fraction = static_cast<double>(bytes) / static_cast<double>(total);
                }
            }
            const bool graphReady = m_hls && m_index > 0
                && std::none_of(m_resources.cbegin() + m_index, m_resources.cend(), [](const Resource &resource) {
                    return resource.kind == Kind::Playlist;
                });
            if (graphReady) {
                const double current = total > 0 ? static_cast<double>(bytes) / static_cast<double>(total) : 0;
                fraction = (static_cast<double>(m_index) + current) / static_cast<double>(m_resources.size());
            }
            emit progress(token, m_savedBytes + bytes, m_hls ? -1 : total, comparing, fraction);
        });
    connect(m_http, &CatchupDownloadTransfer::paused, this, [this](quint64 token, const QString &reason) {
        if (token != m_token)
            return;
        m_running = false;
        emit paused(token, reason);
    });
    connect(m_http, &CatchupDownloadTransfer::failed, this, [this](quint64 token, const QString &reason) {
        if (token == m_token)
            fail(reason);
    });
    connect(m_http, &CatchupDownloadTransfer::completed, this, [this](quint64 token) {
        if (m_resourceActive && token == m_token)
            resourceCompleted();
    });
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- Working path and user-agent mirror the HTTP transfer API.
void CatchupArchiveTransfer::start(quint64 token, const QUrl &url, const QString &path, const QString &userAgent,
                                  const DownloadTimeline &timeline)
{
    m_token = token;
    m_agent = userAgent;
    m_timeline = timeline;
    m_running = true;
    if (m_path != path) {
        m_http->close();
        m_path = path;
        m_resources = {{url, path, Kind::Root}};
        m_byUrl = {{url, 0}};
        m_index = 0;
        m_savedBytes = 0;
        m_hls = false;
        m_error.clear();
    }
    next();
}

void CatchupArchiveTransfer::next()
{
    if (!m_running)
        return;
    if (m_index >= m_resources.size()) {
        m_running = false;
        emit completed(m_token, m_hls);
        return;
    }
    const auto &resource = m_resources[m_index];
    m_resourceActive = true;
    m_http->start(m_token, resource.url, resource.path, m_agent,
                  resource.kind == Kind::Root || resource.kind == Kind::Playlist,
                  resource.kind == Kind::Root ? m_timeline : DownloadTimeline {});
}

void CatchupArchiveTransfer::resourceCompleted()
{
    m_resourceActive = false;
    const auto resource = m_resources[m_index]; // Parsing can append to the queue.
    const auto size = QFileInfo(resource.path).size();
    if (resource.kind == Kind::Key && size != 16) {
        fail(QStringLiteral("The HLS AES-128 key is invalid."));
        return;
    }
    const auto suffix = resource.url.path().toLower();
    const bool playlist = resource.kind == Kind::Playlist || (resource.kind == Kind::Root
        && (m_http->hlsResponse() || suffix.endsWith(QStringLiteral(".m3u8")) || suffix.endsWith(QStringLiteral(".m3u"))));
    if (playlist) {
        m_hls = true;
        if (!rewritePlaylist(resource, m_http->responseUrl())) {
            fail(m_error.isEmpty() ? QStringLiteral("Invalid or unsupported finite HLS playlist.") : m_error);
            return;
        }
    }
    m_savedBytes += size;
    ++m_index;
    if (m_index >= m_resources.size()) {
        m_running = false;
        emit completed(m_token, m_hls);
        return;
    }
    // Allow queued pause/cancel requests between resources.
    const auto token = m_token;
    QTimer::singleShot(0, this, [this, token] {
        if (token == m_token)
            next();
    });
}

QString CatchupArchiveTransfer::reference(const QString &uri, const Resource &parent, const QUrl &baseUrl, Kind kind)
{
    const auto url = baseUrl.resolved(QUrl(uri));
    if (uri.isEmpty() || !CatchupDownloadTransfer::supportedUrl(url, true)
        || m_resources.size() >= 100000) {
        m_error = QStringLiteral("The HLS playlist contains an unsupported resource or too many segments.");
        return {};
    }
    int index = m_byUrl.value(url, -1);
    if (index >= 0 && m_resources[index].kind != kind) {
        m_error = QStringLiteral("The HLS playlist contains cyclic or conflicting resources.");
        return {};
    }
    if (index < 0) {
        if (kind == Kind::Playlist && m_resources.size() > 32) {
            m_error = QStringLiteral("The HLS master contains too many renditions.");
            return {};
        }
        index = static_cast<int>(m_resources.size());
        // Provider filenames never become local paths. Ranges share the same
        // complete local resource, preserving implicit byte-range offsets.
        auto suffix = QFileInfo(url.path()).suffix().toLower();
        static const QStringList extensions {QStringLiteral("ts"), QStringLiteral("m4s"), QStringLiteral("mp4"),
            QStringLiteral("aac"), QStringLiteral("mp3"), QStringLiteral("vtt")};
        if (kind == Kind::Playlist)
            suffix = QStringLiteral("m3u8");
        else if (!extensions.contains(suffix))
            suffix = QStringLiteral("bin");
        const auto path = QDir(m_path + QStringLiteral(".hls")).filePath(QStringLiteral("%1.%2").arg(index).arg(suffix));
        m_resources.push_back({url, path, kind});
        m_byUrl.insert(url, index);
    }
    return QFileInfo(parent.path).dir().relativeFilePath(m_resources[index].path);
}

bool CatchupArchiveTransfer::rewritePlaylist(const Resource &resource, const QUrl &baseUrl)
{
    QFile file(resource.path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 8LL * 1024 * 1024)
        return false;
    const auto text = QString::fromUtf8(file.readAll()).trimmed();
    file.close();
    const auto directory = m_path + QStringLiteral(".hls");
    if (!text.startsWith(QStringLiteral("#EXTM3U")) || !QDir().mkpath(directory)
        || !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        return false;
    const auto lines = text.split(u'\n');
    QStringList output {QStringLiteral("#EXTM3U")};
    QString variant;
    QString variantUri;
    qint64 bestBandwidth = -1;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const auto line = lines[i].trimmed();
        if (!line.startsWith(QStringLiteral("#EXT-X-STREAM-INF:")))
            continue;
        const auto attrs = attributes(line);
        bool ok = false;
        const auto bandwidth = attrs.value(QStringLiteral("BANDWIDTH")).toLongLong(&ok);
        if (!ok || bandwidth < 0 || i + 1 >= lines.size() || lines[i + 1].trimmed().startsWith(u'#'))
            return false;
        if (bandwidth > bestBandwidth) {
            bestBandwidth = bandwidth;
            variant = line;
            variantUri = lines[i + 1].trimmed();
        }
    }
    if (!variant.isEmpty()) {
        if (resource.kind != Kind::Root) // RFC 8216 variants point to media playlists.
            return false;
        const auto selected = attributes(variant);
        for (const auto &raw : lines) {
            auto line = raw.trimmed();
            if (!line.startsWith(QStringLiteral("#EXT-X-MEDIA:")))
                continue;
            const auto attrs = attributes(line);
            const auto type = attrs.value(QStringLiteral("TYPE"));
            if ((type != QStringLiteral("AUDIO") && type != QStringLiteral("SUBTITLES") && type != QStringLiteral("VIDEO"))
                || attrs.value(QStringLiteral("GROUP-ID")) != selected.value(type))
                continue;
            if (attrs.contains(QStringLiteral("URI"))) {
                const auto local = reference(attrs.value(QStringLiteral("URI")), resource, baseUrl, Kind::Playlist);
                if (local.isEmpty())
                    return false;
                line = replaceUri(line, local);
                if (line.isEmpty())
                    return false;
            }
            output.push_back(line);
        }
        const auto local = reference(variantUri, resource, baseUrl, Kind::Playlist);
        if (local.isEmpty())
            return false;
        output << variant << local;
    } else {
        bool ended = false;
        bool segment = false;
        bool durationPending = false;
        for (const auto &raw : lines) {
            auto line = raw.trimmed();
            if (line.isEmpty() || line == QStringLiteral("#EXTM3U"))
                continue;
            if (line == QStringLiteral("#EXT-X-ENDLIST"))
                ended = true;
            if (line.startsWith(QStringLiteral("#EXT-X-GAP")) || line.startsWith(QStringLiteral("#EXT-X-DEFINE:")))
                return false;
            if (line.startsWith(QStringLiteral("#EXTINF:")))
                durationPending = true;
            if (line.startsWith(QStringLiteral("#EXT-X-KEY:")) || line.startsWith(QStringLiteral("#EXT-X-MAP:"))) {
                const auto attrs = attributes(line);
                const bool key = line.startsWith(QStringLiteral("#EXT-X-KEY:"));
                if (key && attrs.value(QStringLiteral("METHOD")) == QStringLiteral("NONE")) {
                    output.push_back(QStringLiteral("#EXT-X-KEY:METHOD=NONE"));
                    continue;
                }
                if (key && (attrs.value(QStringLiteral("METHOD")) != QStringLiteral("AES-128")
                    || attrs.value(QStringLiteral("KEYFORMAT"), QStringLiteral("identity")) != QStringLiteral("identity"))) {
                    m_error = QStringLiteral("The HLS archive uses unsupported encryption (only AES-128 identity is supported).");
                    return false;
                }
                const auto local = reference(attrs.value(QStringLiteral("URI")), resource, baseUrl, key ? Kind::Key : Kind::Media);
                if (local.isEmpty())
                    return false;
                line = replaceUri(line, local);
                if (line.isEmpty())
                    return false;
                output.push_back(line);
            } else if (!line.startsWith(u'#')) {
                if (!durationPending)
                    return false;
                durationPending = false;
                const auto local = reference(line, resource, baseUrl, Kind::Media);
                if (local.isEmpty())
                    return false;
                output.push_back(local);
                segment = true;
            } else {
                // Keep only non-network media timing/format tags. Unknown tags,
                // session data and start hints cannot redirect the local remux.
                static const QStringList retained {QStringLiteral("#EXTINF:"), QStringLiteral("#EXT-X-VERSION:"),
                    QStringLiteral("#EXT-X-TARGETDURATION:"), QStringLiteral("#EXT-X-MEDIA-SEQUENCE:"),
                    QStringLiteral("#EXT-X-DISCONTINUITY-SEQUENCE:"), QStringLiteral("#EXT-X-DISCONTINUITY"),
                    QStringLiteral("#EXT-X-BYTERANGE:"), QStringLiteral("#EXT-X-PLAYLIST-TYPE:"),
                    QStringLiteral("#EXT-X-INDEPENDENT-SEGMENTS"), QStringLiteral("#EXT-X-ENDLIST")};
                for (const auto &prefix : retained) {
                    if (line.startsWith(prefix)) {
                        output.push_back(line);
                        break;
                    }
                }
            }
        }
        if (!ended || !segment || durationPending) {
            m_error = QStringLiteral("The HLS archive must be a complete, finite playlist (EXT-X-ENDLIST).");
            return false;
        }
    }
    QSaveFile saved(resource.path);
    const auto bytes = (output.join(u'\n') + u'\n').toUtf8();
    return saved.open(QIODevice::WriteOnly) && saved.write(bytes) == bytes.size() && saved.commit();
}

void CatchupArchiveTransfer::pause(quint64 token)
{
    m_token = token;
    m_running = false;
    if (m_resourceActive)
        m_http->pause(token);
    else
        emit paused(token, {});
}
void CatchupArchiveTransfer::stop(quint64 token)
{
    m_token = token;
    close();
    emit stopped(token);
}
void CatchupArchiveTransfer::close()
{
    m_running = false;
    m_resourceActive = false;
    m_http->close();
}
void CatchupArchiveTransfer::fail(const QString &reason)
{
    close();
    emit failed(m_token, reason);
}
} // namespace OKILTV::App
