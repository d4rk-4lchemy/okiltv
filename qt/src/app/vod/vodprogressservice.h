#pragma once
#include "vodservices.h"
#include "app/playback/vodplaybacksession.h"
#include <QTimer>

namespace OKILTV::Vod {
class VodProgressService final : public QObject {
    Q_OBJECT
public:
    explicit VodProgressService(VodDependencies, QObject *parent = nullptr);
    ~VodProgressService() override;
    void observe(const SessionSnapshot &, bool checkpoint);
    void flush();
    VodProgress observedProgress(const SessionSnapshot &) const;
    bool movieListWritePending(const ContentRef &ref) const { return m_listWrites.contains(ref.key()); }
    void setMovieList(const ContentRef &, MovieList, bool, std::function<void(Result<MovieListState>)>);
    void setWatched(const ContentRef &, bool, std::function<void(Result<VodProgress>)>);
    void setSeriesWatched(const VodDetails &, bool, std::function<void(Outcome)>);
    bool statusWritePending(const ContentRef &ref) const { return m_statusWrites.contains(parentSeries(ref).valid() ? parentSeries(ref).key() : ref.key()); }
signals:
    void persisted(const OKILTV::Vod::ContentRef &ref);
    void movieListWritePendingChanged(const OKILTV::Vod::ContentRef &ref);
    void movieListsChanged(const OKILTV::Vod::ContentRef &ref);
    void statusWritePendingChanged(const OKILTV::Vod::ContentRef &ref);
public:
    void flushSource(const QUuid &, std::function<void(Outcome)>);
    void shutdown();
    std::function<void(const Error &)> failed;
private:
    VodDependencies m_deps;
    VodJobRunner m_jobs{1, 1};
    QTimer m_timer;
    std::optional<SessionSnapshot> m_snapshot;
    QUuid m_session;
    // Accessed only by jobs on the serial storage lane.
    std::shared_ptr<QUuid> m_initializedSession = std::make_shared<QUuid>();
    quint64 m_sequence = 0;
    bool m_dirty = false;
    bool m_stopped = false;
    bool m_completionQueued = false;
    QSet<QByteArray> m_listWrites;
    QSet<QByteArray> m_statusWrites;
    std::optional<bool> m_manualStatus;
    ContentRef m_currentRef;
    QHash<QUuid, Error> m_writeErrors;
};
}
