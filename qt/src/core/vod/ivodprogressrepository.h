#pragma once
#include "vodmodels.h"

namespace OKILTV::Vod {
class IVodProgressRepository {
public:
    virtual ~IVodProgressRepository() = default;
    virtual Result<std::optional<VodProgress>> read(const ContentRef &, const RequestContext &) = 0;
    // Establishes ordering across sessions before any checkpoint. An old session
    // can never become current merely by presenting a new sequence number.
    virtual Outcome beginSession(const ContentRef &, const QUuid &session, const RequestContext &) = 0;
    virtual Outcome checkpoint(const ContentRef &, const VodProgress &, const RequestContext &) = 0;
    virtual Outcome removeSourceState(const QUuid &) = 0;
};
}
