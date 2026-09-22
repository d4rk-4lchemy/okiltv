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
    int safetySeconds { 180 };
    QString reasonIfUnavailable;
};

struct CatchupDownloadTarget
{
    QString url;
    qint64 trimStartSeconds { 0 };
    qint64 durationSeconds { 0 };
};

class CatchupUrlResolver
{
public:
    explicit CatchupUrlResolver(std::optional<ServerProfile> profile = std::nullopt);

    static QDateTime availableEdge(const QDateTime &stop, int safetySeconds = 180,
                                  const QDateTime &now = QDateTime::currentDateTimeUtc());
    // Rebuild from the canonical provider URL, never a redirect/session URL.
    static QString xtreamWindowUrl(const QString &canonicalUrl, qint64 offsetSeconds,
                                   qint64 availableSeconds, bool withSeconds = false);

    std::optional<CatchupPlaybackTarget> resolve(
        const Channel &channel,
        const EpgEntry &program,
        QString *failureReason = nullptr) const;

    // Finite archive export; unlike resolveWindow(), this covers the complete
    // programme and reports the provider's minute-rounded leading material.
    std::optional<CatchupDownloadTarget> resolveDownload(
        const Channel &channel, const EpgEntry &program, QString *failureReason = nullptr) const;

    // Transport ranges are independent of EPG programme boundaries. The caller
    // supplies the published edge; returned bounds include provider rounding.
    std::optional<CatchupPlaybackTarget> resolveWindow(
        const Channel &channel, const QDateTime &startUtc, const QDateTime &endUtc,
        QString *failureReason = nullptr) const;

private:
    std::optional<ServerProfile> m_profile;
};

} // namespace OKILTV::Core
