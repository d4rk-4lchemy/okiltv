#pragma once

#include <QString>
#include <QUuid>
#include <optional>
#include <variant>

namespace OKILTV::Vod {
enum class ErrorCode {
    Cancelled, Timeout, Unauthorized, Forbidden, RateLimited, ProviderUnavailable,
    UnsupportedCapability, InvalidResponse, ResponseTooLarge, ContentUnavailable,
    StorageUnavailable, SecretUnavailable, PlaybackConflict, PlaybackFailed, StorageBusy
};
struct Error {
    ErrorCode code;
    QUuid operationId;
    bool retryable = false;
    std::optional<qint64> retryAfterMs;
    // Messages are generated locally from codes, never from a provider payload.
    [[nodiscard]] QString message() const;
};
template<class T> using Result = std::variant<T, Error>;
using Success = std::monostate;
using Outcome = Result<Success>;
}
