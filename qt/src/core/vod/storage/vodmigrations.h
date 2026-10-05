#pragma once
#include "../vodmodels.h"

namespace OKILTV::Vod {
// SqliteVodStore implements additive SQL and consistent backup/restore.
// Disabled VOD never calls this port; newer schemas and failures fail closed.
class IVodMigrations {
public:
    virtual ~IVodMigrations() = default;
    virtual Outcome prepare(const RequestContext &) = 0;
};
constexpr int schemaVersion = 11;
}
