#pragma once
#include "core/vod/ivodprovider.h"
#include "core/vod/ivodcatalogrepository.h"
#include "core/vod/ivodprogressrepository.h"
#include "core/vod/storage/vodmigrations.h"
#include "vodjobrunner.h"

namespace OKILTV::Vod {
// Shared ownership lets cancelled workers drain without touching destroyed GUI
// objects. Production adapters must implement deadlines and publication barriers.
struct VodDependencies {
    std::shared_ptr<IVodProvider> provider;
    std::shared_ptr<IVodSourceAccess> sources;
    std::shared_ptr<IVodCatalogRepository> catalog;
    std::shared_ptr<IVodProgressRepository> progress;
    std::shared_ptr<IVodMigrations> migrations;
    std::function<Result<ArtworkRef>(const QUuid &, const ArtworkRef &, const RequestContext &)> artwork;
    std::function<Result<VodMediaProbe>(const PlaybackDescriptor &, const RequestContext &)> mediaProbe;
    [[nodiscard]] bool complete() const;
};
class VodCatalogService {
public:
    static Result<quint64> refresh(const VodDependencies &, const SourceContext &, const CatalogScope &, const RequestContext &);
};
class VodPlaybackResolver {
public:
    static Result<PlaybackDescriptor> resolve(const VodDependencies &, const SourceContext &, const ContentRef &,
        const PlaybackPreferences &, const RequestContext &);
};
}
