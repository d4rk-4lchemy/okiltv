#pragma once
#include "../ivodcatalogrepository.h"
#include "../ivodprogressrepository.h"
#include "../ivodmovielistsrepository.h"
#include "../ivodprovider.h"
#include "vodmigrations.h"
#include <QMutex>
#include <QHash>
#include <functional>

namespace OKILTV::Vod {
// All SQL connections are invocation-local. SourceLoader reads a protected
// SourceStore snapshot on the caller's worker, never SettingsManager::current().
class SqliteVodStore final : public IVodMigrations, public IVodSourceAccess,
    public IVodCatalogRepository, public IVodProgressRepository, public IVodMovieListsRepository {
public:
    using SourceLoader = std::function<Result<SourceContext>(const QUuid &)>;
    SqliteVodStore(QString databasePath, SourceLoader loader);
    Outcome prepare(const RequestContext &) override;
    Result<SourceContext> snapshot(const QUuid &) override;
    bool isCurrent(const SourceRevision &) override;
    // Immediate in-process barrier; durable removal/edit follows on a worker.
    void invalidate(const QUuid &);
    Outcome advanceCredentialRevision(const QUuid &, quint64 revision);
    // Reconcile an interrupted edit against protected, persisted source details.
    Outcome finishCredentialChange(const QUuid &);
    Outcome prepareRemoval(const QUuid &) override;
    Outcome finishRemoval(const QUuid &) override;
    Result<QList<QUuid>> pendingRemovals();
    Result<QList<QUuid>> reconcileRemovedSources(const QList<QUuid> &existingProfiles);
    Result<std::optional<CategorySnapshot>> readCategories(const CatalogScope &, const RequestContext &) override;
    Outcome beginCategoryRefresh(const CatalogScope &, const RequestContext &) override;
    Outcome storeCategories(const CategorySnapshot &, const RequestContext &) override;
    Result<CatalogPage> query(const CatalogQuery &, const RequestContext &) override;
    Result<std::optional<VodDetails>> readDetails(const ContentRef &, const RequestContext &) override;
    Result<std::optional<VodMediaProbe>> readSeasonMediaMetadata(const ContentRef &, const QString &, const RequestContext &) override;
    Outcome storeDetails(const VodDetails &, const RequestContext &) override;
    Result<ImportToken> beginRefresh(const CatalogScope &, const RequestContext &) override;
    Outcome stageBatch(const ImportToken &, const CatalogBatch &) override;
    Result<quint64> publishIfCurrent(const ImportToken &, bool complete) override;
    Outcome abandonRefresh(const ImportToken &) noexcept override;
    Outcome evictCache(const CatalogScope &, const RequestContext &) override;
    Outcome removeSourceState(const QUuid &) override;
    Result<std::optional<VodProgress>> read(const ContentRef &, const RequestContext &) override;
    Result<SeriesProgress> readSeriesProgress(const ContentRef &, const RequestContext &) override;
    Outcome setSeriesWatched(const SeriesWatchedChange &, const RequestContext &) override;
    Outcome beginSession(const ContentRef &, const QUuid &, const RequestContext &) override;
    Outcome checkpoint(const ContentRef &, const VodProgress &, const RequestContext &, bool completed = false) override;
    Result<MovieListState> readMovieLists(const ContentRef &, const RequestContext &) override;
    Result<MovieListState> setMovieList(const ContentRef &, MovieList, bool, const RequestContext &) override;
    [[nodiscard]] QString backupPath() const;
private:
    QString m_path;
    SourceLoader m_loader;
    QMutex m_migrationMutex;
    bool m_prepared = false;
    mutable QMutex m_stateMutex;
    QHash<QUuid, SourceRevision> m_current;
    QHash<QUuid, quint64> m_epochs;
    QString m_backupPath;
};
}
