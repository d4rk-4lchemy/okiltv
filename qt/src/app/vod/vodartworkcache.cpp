#include "vodartworkcache.h"
#include "core/secretprotection.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QImageReader>
#include <QSaveFile>
#include <algorithm>

namespace OKILTV::Vod {
VodArtworkCache::VodArtworkCache(QString directory) : m_directory(std::move(directory)) {}
QString VodArtworkCache::sourceDirectory(const QUuid &id) const
{ return QDir(m_directory).filePath(id.toString(QUuid::WithoutBraces)); }
std::optional<ArtworkRef> VodArtworkCache::remember(const SourceContext &source, const QUrl &url, const RequestContext &context)
{
    if (!httpUrlAllowed(url) || context.interruption()) return {};
    const auto id = QString::fromLatin1(QCryptographicHash::hash(url.toEncoded(), QCryptographicHash::Sha256).toHex());
    QMutexLocker lock(&m_mutex);
    if (m_removed.contains(source.revision.profileId)) return {};
    const auto directory = sourceDirectory(source.revision.profileId);
    if (!QDir().mkpath(directory)) return {};
    const auto filename = QDir(directory).filePath(id + QStringLiteral(".url"));
    if (!QFile::exists(filename)) {
        try {
            const auto protectedUrl = Core::protectSecret(url.toString(QUrl::FullyEncoded)).toUtf8();
            QSaveFile output(filename);
            if (!output.open(QIODevice::WriteOnly) || output.write(protectedUrl) != protectedUrl.size() || !output.commit()) return {};
        } catch (...) { return {}; }
    }
    return ArtworkRef{id, QStringLiteral("poster"), {}};
}
Result<ArtworkRef> VodArtworkCache::resolve(const QUuid &profile, const ArtworkRef &ref, const RequestContext &context)
{
    if (ref.id.size() != 64 || std::any_of(ref.id.cbegin(), ref.id.cend(), [](QChar c) {
        return !(c >= u'0' && c <= u'9') && !(c >= u'a' && c <= u'f');
    })) return Error{ErrorCode::InvalidResponse, context.operationId};
    const auto base = QDir(sourceDirectory(profile)).filePath(ref.id);
    const auto path = base + QStringLiteral(".jpg");
    QUrl remote;
    {
        QMutexLocker lock(&m_mutex);
        if (m_removed.contains(profile)) return Error{ErrorCode::Cancelled, context.operationId};
        if (QFile::exists(path)) {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly)) file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
            return ArtworkRef{ref.id, ref.role, path};
        }
        QFile file(base + QStringLiteral(".url"));
        if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) return Error{ErrorCode::ContentUnavailable, context.operationId};
        try { remote = QUrl(Core::unprotectSecret(QString::fromUtf8(file.readAll()))); }
        catch (...) { return Error{ErrorCode::SecretUnavailable, context.operationId}; }
    }
    if (!httpUrlAllowed(remote)) return Error{ErrorCode::InvalidResponse, context.operationId};
    auto request = context;
    request.responseByteLimit = qint64(8) * 1024 * 1024;
    request.deadline = QDeadlineTimer(10000);
    const auto response = QtHttpTransport{}.get({remote, {}, RedirectPolicy::SameOrigin}, request);
    if (const auto *error = std::get_if<Error>(&response)) return *error;
    const auto &http = std::get<HttpResponse>(response);
    if (http.status != 200) return Error{ErrorCode::ContentUnavailable, context.operationId};
    QBuffer input;
    input.setData(http.body); input.open(QIODevice::ReadOnly);
    QImageReader reader(&input);
    const auto format = reader.format();
    if (format != "jpeg" && format != "png" && format != "webp")
        return Error{ErrorCode::InvalidResponse, context.operationId};
    const auto dimensions = reader.size();
    if (!dimensions.isValid() || qint64(dimensions.width()) * dimensions.height() > 16000000)
        return Error{ErrorCode::InvalidResponse, context.operationId};
    reader.setScaledSize(dimensions.scaled(480, 720, Qt::KeepAspectRatio));
    const auto image = reader.read();
    if (image.isNull()) return Error{ErrorCode::InvalidResponse, context.operationId};
    QMutexLocker lock(&m_mutex);
    if (m_removed.contains(profile)) return Error{ErrorCode::Cancelled, context.operationId};
    if (const auto error = context.interruption()) return *error;
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "JPG", 85) || !output.commit())
        return Error{ErrorCode::StorageUnavailable, context.operationId};
    trim();
    return ArtworkRef{ref.id, ref.role, path};
}
void VodArtworkCache::trim()
{
    QList<QFileInfo> files;
    qint64 bytes = 0;
    QDirIterator iterator(m_directory, {QStringLiteral("*.jpg")}, QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) { iterator.next(); const auto file = iterator.fileInfo(); files.append(file); bytes += file.size(); }
    std::sort(files.begin(), files.end(), [](const QFileInfo &a, const QFileInfo &b) { return a.lastModified() < b.lastModified(); });
    for (const auto &file : files) {
        if (bytes <= qint64(256) * 1024 * 1024) break;
        if (QFile::remove(file.absoluteFilePath())) bytes -= file.size();
    }
}
void VodArtworkCache::retainSources(const QList<QUuid> &profiles)
{
    const auto directories = QDir(m_directory).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &directory : directories) {
        const QUuid id(directory);
        if (!id.isNull() && !profiles.contains(id)) removeSource(id);
    }
}
void VodArtworkCache::removeSource(const QUuid &profile)
{
    QMutexLocker lock(&m_mutex);
    m_removed.insert(profile);
    QDir(sourceDirectory(profile)).removeRecursively();
}
}
