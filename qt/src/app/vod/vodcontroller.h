#pragma once
#include "vodservices.h"
#include "app/playback/playbackcoordinator.h"
#include <QSet>
#include <QTimer>

namespace OKILTV::Vod {
struct VodEvent {
    QUuid operationId;
    QUuid profileId;
    Result<PublicValue> result;
};
class VodController final : public QObject {
    Q_OBJECT
public:
    VodController(VodDependencies, PlaybackCoordinator &, QObject *parent = nullptr);
    QUuid scope(const QUuid &profile);
    QUuid artwork(const QUuid &profile, const ArtworkRef &);
    QUuid capabilities(const QUuid &profile);
    QUuid categories(const CatalogScope &, bool refresh = false);
    QUuid query(const CatalogQuery &);
    QUuid details(const ContentRef &);
    QUuid probe(const ContentRef &);
    QUuid cachePlaybackMetadata(const ContentRef &, const VodMediaProbe &, std::optional<qint64> durationMs);
    QUuid refresh(const CatalogScope &);
    QUuid play(const ContentRef &, const PlaybackPreferences & = {});
    QUuid progress(const ContentRef &);
    QUuid movieLists(const ContentRef &);
    QUuid removeSource(const QUuid &profile);
    void cancel(const QUuid &operation);
    void cancelPendingPlayback();
    void sourceChanged(const QUuid &profile);
    void blockSource(const QUuid &profile);
    void unblockSource(const QUuid &profile);
    void shutdown();
    void resolveForRecovery(const ContentRef &, VodPlaybackSession::RecoveryCompletion);
    std::function<void(const VodEvent &)> completed;
signals:
    void eventCompleted(const OKILTV::Vod::VodEvent &event);
private:
    using Operation = std::function<Result<JobReply>(const VodDependencies &, const SourceContext &, RequestContext)>;
    enum class OperationKind { Read, Play, Probe };
    QUuid submit(const QUuid &, Operation, OperationKind = OperationKind::Read);
    void deliver(const QUuid &, const QUuid &, Result<JobReply>, bool play);
    VodDependencies m_deps;
    PlaybackCoordinator &m_coordinator;
    VodJobRunner m_jobs;
    QUuid m_playRequest;
    QSet<QUuid> m_probes;
    struct DeferredPlay { QUuid id; QUuid profile; std::function<void()> start; };
    std::optional<DeferredPlay> m_deferredPlay;
    QTimer m_probeCooldown;
    bool m_probeHandoff = false;
    bool m_stopped = false;
    QSet<QUuid> m_blockedSources;
};
}
