#pragma once
#include "vodmodels.h"

namespace OKILTV::Vod {
// Thread-safe port; SQL implementations create connections on the calling worker.
class IVodCatalogRepository {
public:
    virtual ~IVodCatalogRepository() = default;
    virtual Result<std::optional<CategorySnapshot>> readCategories(const CatalogScope &, const RequestContext &) = 0;
    // Begin fences earlier requests without erasing the last complete snapshot.
    // Store atomically publishes only for the matching request and source.
    virtual Outcome beginCategoryRefresh(const CatalogScope &, const RequestContext &) = 0;
    virtual Outcome storeCategories(const CategorySnapshot &, const RequestContext &) = 0;
    virtual Result<CatalogPage> query(const CatalogQuery &, const RequestContext &) = 0;
    virtual Result<std::optional<VodDetails>> readDetails(const ContentRef &, const RequestContext &) = 0;
    virtual Outcome storeDetails(const VodDetails &, const RequestContext &) = 0;
    virtual Result<ImportToken> beginRefresh(const CatalogScope &, const RequestContext &) = 0;
    virtual Outcome stageBatch(const ImportToken &, const CatalogBatch &) = 0;
    // Atomic barrier checks cancellation, deadline, latest import AND source
    // revision/removal inside the publication transaction, not just at delivery.
    virtual Result<quint64> publishIfCurrent(const ImportToken &, bool complete) = 0;
    virtual Outcome abandonRefresh(const ImportToken &) noexcept = 0;
    // Eviction MUST NOT erase progress. Source removal is explicitly separate.
    virtual Outcome evictCache(const CatalogScope &, const RequestContext &) = 0;
    virtual Outcome removeSourceState(const QUuid &) = 0;
};
}
