#include "vodsubtitlecache.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QMutexLocker>

namespace OKILTV::Vod {
namespace {
QString uuid(const QUuid &id) { return id.toString(QUuid::WithoutBraces); }
QString digest(const QByteArray &key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex());
}
QString importDirectoryName(const QString &profile, const QByteArray &key, const QString &id)
{
    // Hash the entire logical source/content/import path, using portable separators.
    return digest((profile + u'/' + digest(key) + u'/' + id).toUtf8());
}
QStringList legacyContentNames(const QString &profile, const QByteArray &key)
{
    return {profile + u'/' + digest(key), profile + u'/' + QString::fromLatin1(key.toHex())};
}
bool safeName(const QString &value)
{
    return !value.isEmpty() && value != QLatin1String(".") && value != QLatin1String("..")
        && !value.contains(u'/') && !value.contains(u'\\') && !value.contains(u':');
}
void migrateImports(const QString &root, const QString &profile, const QByteArray &key, const QJsonObject &state)
{
    for (const auto &value : state.value(QStringLiteral("files")).toArray()) {
        const auto id = value.toObject().value(QStringLiteral("id")).toString();
        if (!safeName(id)) continue;
        const auto destination = QDir(root).filePath(importDirectoryName(profile, key, id));
        if (QFileInfo::exists(destination)) continue;
        for (const auto &content : legacyContentNames(profile, key)) {
            // Rename the whole import atomically, keeping IDX/SUB pairs together.
            // Failed moves retain the old path for playback and a later retry.
            if (QDir().rename(QDir(root).filePath(content + u'/' + id), destination)) break;
        }
    }
}
}
VodSubtitleCache::VodSubtitleCache(QString root, std::shared_ptr<IVodSubtitleRepository> repository)
    : m_root(std::move(root)), m_repository(std::move(repository)) {}
QString VodSubtitleCache::directory(const ContentRef &ref, const QString &id) const
{
    return QDir(m_root).filePath(importDirectoryName(uuid(ref.profileId), ref.key(), id));
}
QStringList VodSubtitleCache::legacyDirectories(const ContentRef &ref) const
{
    QStringList paths;
    for (const auto &name : legacyContentNames(uuid(ref.profileId), ref.key())) paths.append(QDir(m_root).filePath(name));
    return paths;
}
void VodSubtitleCache::clean(const ContentRef &ref, const QJsonObject &state)
{
    migrateImports(m_root, uuid(ref.profileId), ref.key(), state);
    QSet<QString> retained;
    for (const auto &value : state.value(QStringLiteral("files")).toArray()) retained.insert(value.toObject().value(QStringLiteral("id")).toString());
    for (const auto &path : legacyDirectories(ref)) {
        const QDir dir(path);
        for (const auto &name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (!retained.contains(name)) QDir(dir.filePath(name)).removeRecursively();
        QDir().rmdir(path);
    }
    QDir().rmdir(QDir(m_root).filePath(uuid(ref.profileId)));
}
Result<QJsonObject> VodSubtitleCache::read(const ContentRef &ref, const RequestContext &context)
{
    QMutexLocker lock(&m_mutex);
    auto result = m_repository->readSubtitles(ref, context);
    if (const auto *state = std::get_if<QJsonObject>(&result)) clean(ref, *state);
    return result;
}
QVariantList VodSubtitleCache::playbackFiles(const ContentRef &ref, const QJsonObject &state) const
{
    QVariantList result;
    for (const auto &value : state.value(QStringLiteral("files")).toArray()) {
        const auto file = value.toObject();
        const auto id = file.value(QStringLiteral("id")).toString();
        const auto entry = file.value(QStringLiteral("entry")).toString();
        if (!safeName(id) || !safeName(entry)) continue;
        auto row = file.toVariantMap();
        auto path = QDir(directory(ref, id)).filePath(entry);
        if (!QFileInfo::exists(path)) {
            for (const auto &content : legacyDirectories(ref)) {
                const auto legacy = QDir(content).filePath(id + u'/' + entry);
                if (QFileInfo::exists(legacy)) { path = legacy; break; }
            }
        }
        row.insert(QStringLiteral("path"), path);
        result.append(row);
    }
    return result;
}
Result<QJsonObject> VodSubtitleCache::importFile(const ContentRef &ref, const QUrl &url, const RequestContext &context)
{
    QMutexLocker lock(&m_mutex);
    if (!ref.playable() || !url.isLocalFile()) return Error{ErrorCode::ContentUnavailable, context.operationId};
    auto current = m_repository->readSubtitles(ref, context);
    if (const auto *error = std::get_if<Error>(&current)) return *error;
    auto state = std::get<QJsonObject>(current);
    clean(ref, state);
    QFileInfo input(url.toLocalFile());
    if (!input.isFile() || !input.isReadable() || input.size() == 0) return Error{ErrorCode::ContentUnavailable, context.operationId};
    QStringList paths{input.absoluteFilePath()};
    // VobSub needs both files; a text .sub remains an ordinary single file.
    const auto suffix = input.suffix().toLower();
    if (suffix == QLatin1String("idx") || suffix == QLatin1String("sub")) {
        QString idx, sub;
        for (const auto &candidate : input.dir().entryInfoList(QDir::Files)) {
            if (candidate.completeBaseName().compare(input.completeBaseName(), Qt::CaseInsensitive) != 0) continue;
            if (candidate.suffix().compare(QStringLiteral("idx"), Qt::CaseInsensitive) == 0) idx = candidate.absoluteFilePath();
            if (candidate.suffix().compare(QStringLiteral("sub"), Qt::CaseInsensitive) == 0) sub = candidate.absoluteFilePath();
        }
        QFile sample(input.absoluteFilePath());
        const bool binarySub = suffix == QLatin1String("sub") && sample.open(QIODevice::ReadOnly)
            && sample.read(4) == QByteArray::fromHex("000001ba");
        if (suffix == QLatin1String("idx") || binarySub || !idx.isEmpty()) {
            if (idx.isEmpty() || sub.isEmpty()) return Error{ErrorCode::ContentUnavailable, context.operationId};
            paths = {idx, sub}; input.setFile(idx);
        }
    }
    const auto id = uuid(QUuid::createUuid());
    const auto folder = directory(ref, id);
    if (!QDir().mkpath(folder)) return Error{ErrorCode::StorageUnavailable, context.operationId};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QString entry;
    for (const auto &path : paths) {
        const auto extension = QFileInfo(path).suffix().toLower();
        const auto name = QStringLiteral("subtitle") + (extension.isEmpty() ? QString{} : u'.' + extension);
        if (entry.isEmpty()) entry = name;
        QFile source(path); QSaveFile target(QDir(folder).filePath(name));
        if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly)) {
            QDir(folder).removeRecursively(); return Error{ErrorCode::StorageUnavailable, context.operationId};
        }
        hash.addData(QByteArray::number(source.size()) + ':');
        while (!source.atEnd()) {
            if (const auto error = context.interruption()) { target.cancelWriting(); QDir(folder).removeRecursively(); return *error; }
            const auto bytes = source.read(qint64(256) * 1024);
            if (bytes.isEmpty() || target.write(bytes) != bytes.size()) {
                target.cancelWriting(); QDir(folder).removeRecursively(); return Error{ErrorCode::StorageUnavailable, context.operationId};
            }
            hash.addData(bytes);
        }
        source.close();
        if (!target.commit()) { QDir(folder).removeRecursively(); return Error{ErrorCode::StorageUnavailable, context.operationId}; }
    }
    const auto digest = QString::fromLatin1(hash.result().toHex());
    auto files = state.value(QStringLiteral("files")).toArray();
    QString selected = id;
    for (const auto &value : files) {
        if (value.toObject().value(QStringLiteral("hash")).toString() == digest) {
            selected = value.toObject().value(QStringLiteral("id")).toString(); break;
        }
    }
    if (selected == id) files.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("name"), input.fileName()},
        {QStringLiteral("entry"), entry}, {QStringLiteral("hash"), digest}});
    else QDir(folder).removeRecursively();
    state.insert(QStringLiteral("files"), files);
    state.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("external")}, {QStringLiteral("externalId"), selected}, {QStringLiteral("ordinal"), 0}});
    const auto saved = m_repository->writeSubtitles(ref, state, context);
    if (const auto *error = std::get_if<Error>(&saved)) { QDir(folder).removeRecursively(); return *error; }
    return state;
}
Result<QJsonObject> VodSubtitleCache::select(const ContentRef &ref, const QJsonObject &selection, const RequestContext &context)
{
    QMutexLocker lock(&m_mutex);
    auto current = m_repository->readSubtitles(ref, context);
    if (const auto *error = std::get_if<Error>(&current)) return *error;
    auto state = std::get<QJsonObject>(current);
    state.insert(QStringLiteral("selection"), selection);
    const auto saved = m_repository->writeSubtitles(ref, state, context);
    if (const auto *error = std::get_if<Error>(&saved)) return *error;
    return state;
}
Result<QJsonObject> VodSubtitleCache::remove(const ContentRef &ref, const QString &id, const RequestContext &context)
{
    QMutexLocker lock(&m_mutex);
    auto current = m_repository->readSubtitles(ref, context);
    if (const auto *error = std::get_if<Error>(&current)) return *error;
    auto state = std::get<QJsonObject>(current);
    QJsonArray files;
    for (const auto &value : state.value(QStringLiteral("files")).toArray())
        if (value.toObject().value(QStringLiteral("id")).toString() != id) files.append(value);
    state.insert(QStringLiteral("files"), files);
    if (state.value(QStringLiteral("selection")).toObject().value(QStringLiteral("externalId")).toString() == id)
        state.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("off")}});
    const auto saved = m_repository->writeSubtitles(ref, state, context);
    if (const auto *error = std::get_if<Error>(&saved)) return *error;
    QDir(directory(ref, id)).removeRecursively();
    clean(ref, state);
    return state;
}
void VodSubtitleCache::reconcile(const QList<QUuid> &profiles)
{
    QMutexLocker lock(&m_mutex);
    const auto inventory = m_repository->subtitleInventory({});
    if (!std::holds_alternative<QJsonArray>(inventory)) return;
    QJsonArray retained;
    for (const auto &value : std::get<QJsonArray>(inventory)) {
        if (profiles.contains(QUuid(value.toObject().value(QStringLiteral("profile")).toString()))) retained.append(value);
    }
    reconcileInventory(retained);
}
void VodSubtitleCache::reconcileInventory(const QJsonArray &inventory)
{
    QSet<QString> retained;
    for (const auto &value : inventory) {
        const auto row = value.toObject();
        const auto profile = row.value(QStringLiteral("profile")).toString();
        const auto key = QByteArray::fromHex(row.value(QStringLiteral("key")).toString().toLatin1());
        migrateImports(m_root, profile, key, row.value(QStringLiteral("state")).toObject());
        for (const auto &file : row.value(QStringLiteral("state")).toObject().value(QStringLiteral("files")).toArray())
        {
            const auto id = file.toObject().value(QStringLiteral("id")).toString();
            retained.insert(importDirectoryName(profile, key, id));
            for (const auto &content : legacyContentNames(profile, key)) retained.insert(content + u'/' + id);
        }
    }
    const QDir root(m_root);
    for (const auto &name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (retained.contains(name)) continue; // Referenced flat import directory.
        const QDir source(root.filePath(name));
        if (QUuid(name).isNull()) { QDir(source).removeRecursively(); continue; }
        for (const auto &key : source.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QDir content(source.filePath(key));
            for (const auto &id : content.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
                if (!retained.contains(name + u'/' + key + u'/' + id)) QDir(content.filePath(id)).removeRecursively();
            QDir().rmdir(content.absolutePath());
        }
        QDir().rmdir(source.absolutePath());
    }
}
void VodSubtitleCache::removeSource(const QUuid &profile)
{
    QMutexLocker lock(&m_mutex);
    // Source rows have already been deleted. Retain other sources through the SQL
    // inventory; ownership is no longer encoded in a top-level source directory.
    const auto inventory = m_repository->subtitleInventory({});
    if (!std::holds_alternative<QJsonArray>(inventory)) return;
    QJsonArray retained;
    for (const auto &value : std::get<QJsonArray>(inventory))
        if (QUuid(value.toObject().value(QStringLiteral("profile")).toString()) != profile) retained.append(value);
    reconcileInventory(retained);
}
}
