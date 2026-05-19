#include "models.h"

#include <algorithm>
#include <cmath>

namespace OKILTV::Core {

namespace {

QString normalizedGuid(const QUuid &value)
{
    return value.toString(QUuid::WithoutBraces).toLower();
}

double roundToSingleDecimal(const double value)
{
    return std::round(value * 10.0) / 10.0;
}

} // namespace

QString guidToString(const QUuid &value)
{
    return normalizedGuid(value);
}

QUuid parseGuid(QStringView value)
{
    const auto trimmed = value.toString().trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    if (trimmed.startsWith(u'{')) {
        return QUuid(trimmed);
    }

    return QUuid(QStringLiteral("{%1}").arg(trimmed));
}

QString profileTypeLabel(const ProfileType type)
{
    switch (type) {
    case ProfileType::Xtream:
        return QStringLiteral("Xtream");
    case ProfileType::M3UUrl:
        return QStringLiteral("M3U URL");
    case ProfileType::M3UFile:
        return QStringLiteral("M3U File");
    }

    return QStringLiteral("Unknown");
}

QString channelSourceToString(const ChannelSource source)
{
    switch (source) {
    case ChannelSource::Xtream:
        return QStringLiteral("Xtream");
    case ChannelSource::M3U:
        return QStringLiteral("M3U");
    }

    return QStringLiteral("M3U");
}

ChannelSource channelSourceFromString(const QString &value)
{
    return value.compare(QStringLiteral("Xtream"), Qt::CaseInsensitive) == 0
        ? ChannelSource::Xtream
        : ChannelSource::M3U;
}

QString ungroupedCategoryId()
{
    return QStringLiteral("__ungrouped__");
}

QString normalizeChannelCategoryId(const QString &value)
{
    const auto trimmed = value.trimmed();
    return trimmed.isEmpty() ? ungroupedCategoryId() : trimmed;
}

QString displayNameForCategoryId(const QString &value)
{
    auto normalized = normalizeChannelCategoryId(value);
    if (normalized == ungroupedCategoryId()) {
        return QStringLiteral("Ungrouped");
    }
    return normalized;
}

int normalizeAutoRefreshIntervalHours(const int value)
{
    return std::max(0, value);
}

int normalizeGuideHours(const int value)
{
    return std::clamp(value, 1, 48);
}

double normalizePlayerWaitForStreamSeconds(const double value)
{
    if (!std::isfinite(value)) {
        return 5.0;
    }

    return std::clamp(roundToSingleDecimal(value), 0.1, 120.0);
}

double normalizePlayerBufferSeconds(const double value)
{
    if (!std::isfinite(value)) {
        return 3.0;
    }

    return std::clamp(roundToSingleDecimal(value), 0.1, 60.0);
}

int normalizeTimeshiftWindowMinutes(const int value)
{
    return std::clamp(value, 15, 360);
}

int normalizeTimeshiftSegmentSeconds(const int value)
{
    return std::clamp(value, 2, 60);
}

int normalizeTimeshiftMaxDiskGb(const int value)
{
    return std::clamp(value, 1, 128);
}

int normalizeMultiviewMaxTiles(const int value)
{
    if (value <= 2) {
        return 2;
    }
    if (value < 4) {
        return 2;
    }
    if (value < 6) {
        return 4;
    }
    if (value < 9) {
        return 6;
    }
    if (value < 12) {
        return 9;
    }
    return 12;
}

bool epgEntryIsNow(const EpgEntry &entry)
{
    const auto now = QDateTime::currentDateTimeUtc();
    return entry.start.isValid() && entry.stop.isValid() && now >= entry.start && now < entry.stop;
}

double epgEntryProgressPercent(const EpgEntry &entry)
{
    const auto now = QDateTime::currentDateTimeUtc();
    if (!entry.start.isValid() || !entry.stop.isValid() || now < entry.start || now >= entry.stop) {
        return 0.0;
    }

    const auto total = entry.start.msecsTo(entry.stop);
    if (total <= 0) {
        return 0.0;
    }

    return static_cast<double>(entry.start.msecsTo(now)) / static_cast<double>(total) * 100.0;
}

QString epgEntryTimeRange(const EpgEntry &entry)
{
    return QStringLiteral("%1 - %2")
        .arg(entry.start.toLocalTime().toString(QStringLiteral("HH:mm")))
        .arg(entry.stop.toLocalTime().toString(QStringLiteral("HH:mm")));
}

QString epgEntryStartTimeLabel(const EpgEntry &entry)
{
    return entry.start.toLocalTime().toString(QStringLiteral("HH:mm"));
}

} // namespace OKILTV::Core
