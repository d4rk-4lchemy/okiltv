#pragma once
#include "core/vod/vodmodels.h"
#include <QObject>
#include <QHash>
#include <deque>
#include <functional>

namespace OKILTV::Vod {
using PublicValue = std::variant<CatalogScope, ArtworkRef, Success, ProviderCapabilities, CategorySnapshot, CatalogPage,
    VodDetails, VodMediaProbe, MovieListState, quint64, std::optional<VodProgress>>;
struct JobReply {
    SourceRevision source;
    PublicValue value;
    // Never emitted as a public facade event.
    std::optional<PlaybackDescriptor> playback;
};
class VodJobRunner final : public QObject {
public:
    using Work = std::function<Result<JobReply>(RequestContext)>;
    using Completion = std::function<void(Result<JobReply>)>;
    explicit VodJobRunner(int concurrency = 4, int perSource = 2, QObject *parent = nullptr);
    ~VodJobRunner() override;
    void submit(const QUuid &profile, RequestContext, Work, Completion);
    void cancel(const QUuid &operation);
    void cancelSource(const QUuid &profile);
    void shutdown();
    bool drain(int deadlineMs);
    [[nodiscard]] int pending() const;
private:
    struct Job { QUuid profile; RequestContext request; Work work; Completion completion; };
    void pump();
    int m_limit;
    int m_perSource;
    bool m_stopped = false;
    std::deque<Job> m_queue;
    QHash<QUuid, Job> m_active;
};
}
