#pragma once

#include "core/vod/vodmodels.h"

namespace OKILTV::Vod {

Result<VodMediaProbe> parseVodMediaProbe(const QByteArray &payload, const RequestContext &context = {});
Result<VodMediaProbe> probeVodMedia(const PlaybackDescriptor &, const RequestContext &);

}
