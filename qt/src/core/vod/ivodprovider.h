#pragma once
#include "vodmodels.h"

namespace OKILTV::Vod {
// Called only inside workers. Implementations create thread-local HTTP objects.
class IVodProvider {
public:
    virtual ~IVodProvider() = default;
    virtual Result<ProviderCapabilities> capabilities(const SourceContext &, const RequestContext &) = 0;
    virtual Result<QList<VodCategory>> listCategories(const SourceContext &, const CatalogScope &, const RequestContext &) = 0;
    virtual Result<CatalogBatch> fetchCatalog(const SourceContext &, const CatalogScope &,
        const std::optional<ProviderCursor> &, const RequestContext &) = 0;
    virtual Result<VodDetails> fetchDetails(const SourceContext &, const ContentRef &, const RequestContext &) = 0;
    virtual Result<PlaybackDescriptor> resolvePlayback(const SourceContext &, const ContentRef &,
        const PlaybackPreferences &, const RequestContext &) = 0;
};
class IVodSourceAccess {
public:
    virtual ~IVodSourceAccess() = default;
    virtual Result<SourceContext> snapshot(const QUuid &profileId) = 0;
    virtual bool isCurrent(const SourceRevision &) = 0;
    // Persistent deletion intent/barrier precedes cancellation and any removal.
    // Must serialize with repository publication/checkpoints; no resurrection.
    virtual Outcome prepareRemoval(const QUuid &profileId) = 0;
    virtual Outcome finishRemoval(const QUuid &profileId) = 0;
};
}
