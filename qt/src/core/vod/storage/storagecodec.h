#pragma once
#include "../vodmodels.h"
namespace OKILTV::Vod::Storage {
QByteArray encodeDetails(const VodDetails &);
std::optional<VodDetails> decodeDetails(const QByteArray &);
}
