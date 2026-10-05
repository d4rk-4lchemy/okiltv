#pragma once
#include "vodmodels.h"
#include <QJsonArray>

namespace OKILTV::Vod {
// Payload contains only local cache identifiers/names and a requested selection.
// It is independent of watch progress and survives catalogue eviction.
class IVodSubtitleRepository {
public:
    virtual ~IVodSubtitleRepository() = default;
    virtual Result<QJsonObject> readSubtitles(const ContentRef &, const RequestContext &) = 0;
    virtual Result<QJsonArray> subtitleInventory(const RequestContext &) = 0;
    virtual Outcome writeSubtitles(const ContentRef &, const QJsonObject &, const RequestContext &) = 0;
};
}
