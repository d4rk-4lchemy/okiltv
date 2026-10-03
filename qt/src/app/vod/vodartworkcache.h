#pragma once
#include "core/vod/providers/httptransport.h"
#include <QMutex>
#include <QSet>
#include <QWaitCondition>

namespace OKILTV::Vod {
// Trusted disk registry. Only opaque references and decoded local images leave it.
class VodArtworkCache final {
public:
    explicit VodArtworkCache(QString directory);
    std::optional<ArtworkRef> remember(const SourceContext &, const QUrl &, const RequestContext &);
    // Worker-only, disk-only lookup; never starts a download.
    Result<std::optional<ArtworkRef>> cached(const QUuid &, const ArtworkRef &, const RequestContext &);
    Result<ArtworkRef> resolve(const QUuid &, const ArtworkRef &, const RequestContext &);
    void removeSource(const QUuid &);
    void retainSources(const QList<QUuid> &);
private:
    QString sourceDirectory(const QUuid &) const;
    std::optional<ArtworkRef> cachedLocked(const QUuid &, const ArtworkRef &);
    void trim();
    QString m_directory;
    QMutex m_mutex;
    QSet<QUuid> m_removed;
    QSet<QString> m_inFlight;
    QWaitCondition m_downloadFinished;
};
}
