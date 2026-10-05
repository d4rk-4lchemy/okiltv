#pragma once
#include "../ivodprovider.h"
#include "httptransport.h"
#include <QJsonDocument>
#include <functional>

namespace OKILTV::Vod {
class XtreamVodProvider final : public IVodProvider {
public:
    using ArtworkRegistrar = std::function<std::optional<ArtworkRef>(const SourceContext &, const QUrl &, const RequestContext &)>;
    ArtworkRegistrar registerArtwork;
    explicit XtreamVodProvider(std::shared_ptr<IHttpTransport> transport = std::make_shared<QtHttpTransport>(), bool seriesEnabled = false);
    Result<ProviderCapabilities> capabilities(const SourceContext &, const RequestContext &) override;
    Result<QList<VodCategory>> listCategories(const SourceContext &, const CatalogScope &, const RequestContext &) override;
    Result<CatalogBatch> fetchCatalog(const SourceContext &, const CatalogScope &,
        const std::optional<ProviderCursor> &, const RequestContext &) override;
    Result<VodDetails> fetchDetails(const SourceContext &, const ContentRef &, const RequestContext &) override;
    Result<PlaybackDescriptor> resolvePlayback(const SourceContext &, const ContentRef &,
        const PlaybackPreferences &, const RequestContext &) override;
private:
    Result<QJsonDocument> api(const SourceContext &, const QString &action,
        const QMap<QString, QString> &parameters, const RequestContext &) const;
    std::shared_ptr<IHttpTransport> m_transport;
    bool m_seriesEnabled = false;
};
}
