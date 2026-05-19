#pragma once

#include "models.h"

#include <optional>

namespace OKILTV::Core {

struct CatchupPlaybackTarget
{
    QString url;
    QDateTime programStartUtc;
    QDateTime programStopUtc;
    qint64 durationSeconds { 0 };
    QString reasonIfUnavailable;
};

class CatchupUrlResolver
{
public:
    explicit CatchupUrlResolver(std::optional<ServerProfile> profile = std::nullopt);

    std::optional<CatchupPlaybackTarget> resolve(
        const Channel &channel,
        const EpgEntry &program,
        QString *failureReason = nullptr) const;

private:
    std::optional<ServerProfile> m_profile;
};

} // namespace OKILTV::Core
