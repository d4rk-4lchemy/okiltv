#pragma once
#include "core/vod/ivodsubtitlerepository.h"
#include <QMutex>

namespace OKILTV::Vod {
// Called on workers. One lock serializes local files and selection publication.
class VodSubtitleCache final {
public:
    VodSubtitleCache(QString root, std::shared_ptr<IVodSubtitleRepository> repository);
    Result<QJsonObject> read(const ContentRef &, const RequestContext &);
    Result<QJsonObject> importFile(const ContentRef &, const QUrl &, const RequestContext &);
    Result<QJsonObject> remove(const ContentRef &, const QString &id, const RequestContext &);
    Result<QJsonObject> select(const ContentRef &, const QJsonObject &, const RequestContext &);
    QVariantList playbackFiles(const ContentRef &, const QJsonObject &) const;
    void reconcile(const QList<QUuid> &profiles);
    void removeSource(const QUuid &);
private:
    QString directory(const ContentRef &, const QString &id) const;
    QStringList legacyDirectories(const ContentRef &) const;
    void clean(const ContentRef &, const QJsonObject &);
    void reconcileInventory(const QJsonArray &);
    QString m_root;
    std::shared_ptr<IVodSubtitleRepository> m_repository;
    QMutex m_mutex;
};
}
