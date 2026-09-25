#include "epggridmodel.h"
#include "../core/debuglogger.h"

#include <QtConcurrent>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace OKILTV::App {

using namespace Core;

namespace {

constexpr int kViewportMinDurationMinutes = 60;
constexpr int kViewportTimePrefetchMinutes = 120;
constexpr int kViewportRowPrefetch = 12;
constexpr int kViewportRangeBucketMinutes = 30;
constexpr int kOffscreenWarmupDelayMs = 100;
constexpr qint64 kSecondsPerMinute = 60;
constexpr qint64 kSecondsPerHour = 60 * kSecondsPerMinute;

qint64 minutesToSeconds(const int minutes)
{
    return static_cast<qint64>(minutes) * kSecondsPerMinute;
}

double secondsToMinutes(const qint64 seconds)
{
    return static_cast<double>(seconds) / static_cast<double>(kSecondsPerMinute);
}

QDateTime roundToHour(const QDateTime &value)
{
    auto rounded = value.toLocalTime();
    rounded.setTime(QTime(rounded.time().hour(), 0, 0, 0));
    return rounded.toUTC();
}

QString toIsoUtc(const QDateTime &value)
{
    return value.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime fromIso(const QString &value)
{
    auto parsed = QDateTime::fromString(value.trimmed(), Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(value.trimmed(), Qt::ISODate);
    }
    return parsed.toUTC();
}

bool matchesProgramStart(const EpgEntry &entry, const QString &selectedStart)
{
    if (selectedStart.isEmpty()) {
        return false;
    }

    return toIsoUtc(entry.start) == selectedStart
        || entry.start.toUTC().toString(Qt::ISODate) == selectedStart;
}

bool entryIsNowAt(const EpgEntry &entry, const QDateTime &nowUtc)
{
    return entry.start.isValid() && entry.stop.isValid() && nowUtc >= entry.start && nowUtc < entry.stop;
}

double entryProgressPercentAt(const EpgEntry &entry, const QDateTime &nowUtc)
{
    if (!entry.start.isValid() || !entry.stop.isValid() || nowUtc < entry.start || nowUtc >= entry.stop) {
        return 0.0;
    }

    const auto total = entry.start.msecsTo(entry.stop);
    if (total <= 0) {
        return 0.0;
    }

    return static_cast<double>(entry.start.msecsTo(nowUtc)) / static_cast<double>(total) * 100.0;
}

QVariantMap buildProgramMap(
    const Channel &channel,
    const EpgEntry &program,
    const QDateTime &windowStart,
    const QDateTime &windowEnd,
    const std::optional<EpgEntry> &nextProgram,
    const QDateTime &nowUtc,
    const bool isSelected,
    const DateTimeFormatOptions format)
{
    const auto visibleStart = std::max(program.start, windowStart);
    const auto visibleStop = std::min(program.stop, windowEnd);
    const auto durationMinutes = std::max(10.0, secondsToMinutes(visibleStart.secsTo(visibleStop)));
    auto displayDurationMinutes = durationMinutes;
    if (nextProgram.has_value()) {
        const auto nextVisibleStart = std::max(nextProgram->start, windowStart);
        const auto availableMinutes = secondsToMinutes(visibleStart.secsTo(nextVisibleStart));
        if (availableMinutes > 0.0) {
            displayDurationMinutes = std::min(durationMinutes, availableMinutes);
        }
    }

    return QVariantMap {
        { QStringLiteral("channelId"), program.channelId },
        { QStringLiteral("channelIdInt"), channel.id },
        { QStringLiteral("channelName"), channel.name },
        { QStringLiteral("profileId"), guidToString(channel.profileId) },
        { QStringLiteral("streamUrl"), channel.streamUrl },
        { QStringLiteral("tvgId"), channel.tvgId },
        { QStringLiteral("title"), program.title },
        { QStringLiteral("subTitle"), program.subTitle },
        { QStringLiteral("description"), program.description },
        { QStringLiteral("episodeNum"), program.episodeNum },
        { QStringLiteral("start"), program.start.toUTC().toString(Qt::ISODateWithMs) },
        { QStringLiteral("stop"), program.stop.toUTC().toString(Qt::ISODateWithMs) },
        { QStringLiteral("isNow"), entryIsNowAt(program, nowUtc) },
        { QStringLiteral("progressPercent"), entryProgressPercentAt(program, nowUtc) },
        { QStringLiteral("timeRange"), epgEntryTimeRange(program, format) },
        { QStringLiteral("startTimeLabel"), epgEntryStartTimeLabel(program, format) },
        { QStringLiteral("offsetMinutes"), secondsToMinutes(windowStart.secsTo(visibleStart)) },
        { QStringLiteral("durationMinutes"), durationMinutes },
        { QStringLiteral("displayDurationMinutes"), displayDurationMinutes },
        { QStringLiteral("startsBeforeWindow"), program.start < windowStart },
        { QStringLiteral("endsAfterWindow"), program.stop > windowEnd },
        { QStringLiteral("isSelected"), isSelected }
    };
}

} // namespace

EpgGridModel::EpgGridModel(EpgService *epg, QObject *parent)
    : QAbstractListModel(parent)
    , m_epg(epg)
    , m_windowStart(defaultWindowStart(m_guidePastHours))
    , m_timeSlots(computeTimeSlots())
    , m_visibleTimeSlots(computeVisibleTimeSlots())
{
}

void EpgGridModel::setDateTimeFormat(const Core::DateTimeFormatOptions options)
{
    if (options == m_dateTimeFormat) {
        return;
    }
    m_dateTimeFormat = options;
    invalidateProgramTilesCache();
    m_timeSlots = computeTimeSlots();
    m_visibleTimeSlots = computeVisibleTimeSlots();
    emit timeSlotsChanged();
    emit visibleTimeSlotsChanged();
    emit windowChanged();
    emitProgramsChangedForVisibleRows();
    emit selectedProgramChanged();
}

EpgGridModel::~EpgGridModel()
{
    m_tilesCancelled->store(true);
    ++m_rebuildGeneration;
    m_backgroundTasks.waitForFinished();
}

int EpgGridModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant EpgGridModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }

    const auto &row = m_rows.at(index.row());
    switch (role) {
    case ChannelIdRole:
        return row.channel.id;
    case ChannelNameRole:
        return row.channel.name;
    case ChannelIconPathRole:
        return row.channel.cachedIconPath;
    case ChannelTvgIdRole:
        return row.channel.tvgId;
    case ChannelProfileIdRole:
        return guidToString(row.channel.profileId);
    case ChannelStreamUrlRole:
        return row.channel.streamUrl;
    case ProgramsRole:
        return buildPrograms(index.row(), row);
    case ChannelCatchupSupportedRole:
        return row.channel.catchupSupported;
    default:
        return {};
    }
}

QHash<int, QByteArray> EpgGridModel::roleNames() const
{
    return {
        { ChannelIdRole, "channelId" },
        { ChannelNameRole, "channelName" },
        { ChannelIconPathRole, "channelIconPath" },
        { ChannelTvgIdRole, "channelTvgId" },
        { ChannelProfileIdRole, "channelProfileId" },
        { ChannelStreamUrlRole, "channelStreamUrl" },
        { ProgramsRole, "programs" },
        { ChannelCatchupSupportedRole, "channelCatchupSupported" }
    };
}

QVariantList EpgGridModel::timeSlots() const
{
    return m_timeSlots;
}

QVariantList EpgGridModel::visibleTimeSlots() const
{
    return m_visibleTimeSlots;
}

QString EpgGridModel::windowStartLabel() const
{
    return formatDisplayTime(m_windowStart, m_dateTimeFormat);
}

QString EpgGridModel::windowEndLabel() const
{
    return formatDisplayTime(windowEnd(), m_dateTimeFormat);
}

int EpgGridModel::windowSpanMinutes() const
{
    return m_windowSpanMinutes;
}

int EpgGridModel::selectedChannelId() const
{
    return m_selectedChannelId;
}

void EpgGridModel::setSelectedChannelId(const int value)
{
    if (m_selectedChannelId == value) {
        return;
    }

    const auto previousRow = rowIndexForChannelId(m_selectedChannelId);
    m_selectedChannelId = value;
    const auto nextRow = rowIndexForChannelId(m_selectedChannelId);
    if (previousRow >= 0) {
        m_programTilesCacheByRow.remove(previousRow);
    }
    if (nextRow >= 0) {
        m_programTilesCacheByRow.remove(nextRow);
    }

    emit selectedChannelIdChanged();
    emit selectedProgramChanged();
    emitProgramsChangedForRow(previousRow);
    emitProgramsChangedForRow(nextRow);
}

QString EpgGridModel::selectedProgramStart() const
{
    return m_selectedProgramStart;
}

void EpgGridModel::setSelectedProgramStart(const QString &value)
{
    if (m_selectedProgramStart == value) {
        return;
    }

    m_selectedProgramStart = value;
    const auto selectedRow = rowIndexForChannelId(m_selectedChannelId);
    if (selectedRow >= 0) {
        m_programTilesCacheByRow.remove(selectedRow);
    }
    emit selectedProgramStartChanged();
    emit selectedProgramChanged();
    emitProgramsChangedForRow(selectedRow);
}

QVariantMap EpgGridModel::selectedProgram() const
{
    return findSelectedProgram();
}

double EpgGridModel::currentTimeOffsetMinutes() const
{
    return secondsToMinutes(m_windowStart.secsTo(QDateTime::currentDateTimeUtc()));
}

int EpgGridModel::guidePastHours() const
{
    return m_guidePastHours;
}

int EpgGridModel::lookAheadHours() const
{
    return m_lookAheadHours;
}

void EpgGridModel::invalidateRebuild()
{
    cancelProgramRequest();
    m_resolvedProgram.clear();
    ++m_rebuildGeneration;
    if (!m_rebuildPending) {
        m_rebuildPending = true;
        emit rebuildPendingChanged();
    }
}

void EpgGridModel::rebuild(
    const QList<Channel> &channels,
    const int guidePastHours,
    const int lookAheadHours)
{
    if (m_epg->snapshot()->store) { rebuildAsync(channels, guidePastHours, lookAheadHours); return; }
    invalidateRebuild();
    const auto normalizedGuidePastHours = normalizeGuideHours(guidePastHours);
    const auto normalizedLookAheadHours = normalizeGuideHours(lookAheadHours);
    const auto windowStart = defaultWindowStart(normalizedGuidePastHours);
    QDateTime resolvedWindowEnd;
    applyRows(
        channels,
        buildRows(channels, normalizedGuidePastHours, normalizedLookAheadHours, windowStart, &resolvedWindowEnd),
        normalizedGuidePastHours,
        normalizedLookAheadHours,
        windowStart,
        resolvedWindowEnd);
}

void EpgGridModel::rebuildAsync(
    const QList<Channel> &channels,
    const int guidePastHours,
    const int lookAheadHours)
{
    invalidateRebuild();
    const auto generation = m_rebuildGeneration;
    const auto channelsCopy = std::make_shared<QList<Channel>>(channels);
    const auto normalizedGuidePastHours = normalizeGuideHours(guidePastHours);
    const auto normalizedLookAheadHours = normalizeGuideHours(lookAheadHours);
    const auto windowStart = defaultWindowStart(normalizedGuidePastHours);

    m_backgroundTasks.addFuture(QtConcurrent::run(EpgService::readPool(), [this, generation, channelsCopy, normalizedGuidePastHours, normalizedLookAheadHours, windowStart, snapshot = m_epg->snapshot()]() {
        auto resolvedWindowEnd = std::make_shared<QDateTime>();
        EpgService reader; reader.applySnapshot(snapshot);
        const auto to = windowStart.addSecs(minutesToSeconds((normalizedGuidePastHours + normalizedLookAheadHours) * 60));
        QStringList ids;
        for (const auto &channel : *channelsCopy) ids.push_back(channel.tvgId);
        const auto available = reader.programsForChannels(ids, windowStart, to, 1, true);
        auto rows = std::make_shared<QList<Row>>();
        for (const auto &channel : *channelsCopy)
            if (!channel.tvgId.trimmed().isEmpty() && !available.value(channel.tvgId.trimmed().toLower()).isEmpty())
                rows->push_back(Row {channel});
        *resolvedWindowEnd = to;
        QMetaObject::invokeMethod(
            this,
            [this, generation, channelsCopy, rows, resolvedWindowEnd, normalizedGuidePastHours, normalizedLookAheadHours, windowStart]() {
                if (generation != m_rebuildGeneration) {
                    return;
                }

                applyRows(
                    *channelsCopy,
                    *rows,
                    normalizedGuidePastHours,
                    normalizedLookAheadHours,
                    windowStart,
                    *resolvedWindowEnd);
            },
            Qt::QueuedConnection);
    }));
}

int EpgGridModel::channelIdAt(const int row) const
{
    if (row < 0 || row >= m_rows.size()) {
        return -1;
    }
    return m_rows.at(row).channel.id;
}

int EpgGridModel::rowIndexForChannelId(const int channelId) const
{
    return m_rowIndexByChannelId.value(channelId, -1);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int EpgGridModel::adjacentChannelId(const int channelId, const int delta) const
{
    if (m_rows.isEmpty()) {
        return -1;
    }

    auto rowIndex = rowIndexForChannelId(channelId);
    if (rowIndex < 0) {
        return -1;
    }

    rowIndex = std::clamp(rowIndex + delta, 0, static_cast<int>(m_rows.size()) - 1);

    return m_rows.at(rowIndex).channel.id;
}

QVariantMap EpgGridModel::adjacentProgram(const int channelId, const QString &currentStartIso, const int delta) const
{
    const auto programs = channelProgramsInWindow(channelId);
    if (programs.isEmpty()) {
        return {};
    }

    int programIndex = 0;
    auto foundCurrent = false;
    for (qsizetype index = 0; index < programs.size(); ++index) {
        if (matchesProgramStart(programs.at(index), currentStartIso)) {
            programIndex = static_cast<int>(index);
            foundCurrent = true;
            break;
        }
    }

    if (foundCurrent) {
        programIndex = std::clamp(programIndex + delta, 0, static_cast<int>(programs.size()) - 1);
    } else if (delta < 0) {
        programIndex = static_cast<int>(programs.size()) - 1;
    }

    const auto rowIndex = rowIndexForChannelId(channelId);
    if (rowIndex < 0) {
        return {};
    }

    const auto &channel = m_rows.at(rowIndex).channel;
    const auto &program = programs.at(programIndex);
    const std::optional<EpgEntry> nextProgram = programIndex + 1 < programs.size()
        ? std::optional<EpgEntry>(programs.at(programIndex + 1))
        : std::nullopt;
    return buildProgramMap(
        channel,
        program,
        m_windowStart,
        windowEnd(),
        nextProgram,
        QDateTime::currentDateTimeUtc(),
        false, m_dateTimeFormat);
}

QVariantMap EpgGridModel::programForChannelAtTimestamp(const int channelId, const QString &timestampIso) const
{
    const auto programs = channelProgramsInWindow(channelId);
    if (programs.isEmpty()) {
        return {};
    }

    const auto timestamp = fromIso(timestampIso);
    qsizetype targetIndex = 0;
    if (!timestamp.isValid()) {
        targetIndex = 0;
    } else {
        qint64 nearestDistance = std::numeric_limits<qint64>::max();
        for (qsizetype index = 0; index < programs.size(); ++index) {
            const auto &program = programs.at(index);
            if (program.start.isValid() && program.stop.isValid() && timestamp >= program.start && timestamp < program.stop) {
                targetIndex = index;
                break;
            }

            const auto distance = program.start.isValid()
                ? qAbs(program.start.secsTo(timestamp))
                : std::numeric_limits<qint64>::max();
            if (distance < nearestDistance) {
                nearestDistance = distance;
                targetIndex = index;
            }
        }
    }

    const auto rowIndex = rowIndexForChannelId(channelId);
    if (rowIndex < 0) {
        return {};
    }

    const auto &channel = m_rows.at(rowIndex).channel;
    const auto &program = programs.at(targetIndex);
    const std::optional<EpgEntry> nextProgram = targetIndex + 1 < programs.size()
        ? std::optional<EpgEntry>(programs.at(targetIndex + 1))
        : std::nullopt;
    return buildProgramMap(
        channel,
        program,
        m_windowStart,
        windowEnd(),
        nextProgram,
        QDateTime::currentDateTimeUtc(),
        false, m_dateTimeFormat);
}

void EpgGridModel::setRenderViewport(const double startMinutes, const double durationMinutes)
{
    if (m_windowSpanMinutes <= 0) {
        return;
    }

    const auto normalizedDuration = std::max(
        kViewportMinDurationMinutes,
        static_cast<int>(std::ceil(std::max(0.0, durationMinutes))));
    const auto maxStart = std::max(0, m_windowSpanMinutes - 1);
    const auto normalizedStart = std::clamp(
        static_cast<int>(std::floor(std::max(0.0, startMinutes))),
        0,
        maxStart);

    const auto previousViewportStart = m_renderViewportStartMinutes;
    const auto previousViewportDuration = m_renderViewportDurationMinutes;
    if (previousViewportStart == normalizedStart
        && previousViewportDuration == normalizedDuration) {
        return;
    }

    m_renderViewportStartMinutes = normalizedStart;
    m_renderViewportDurationMinutes = normalizedDuration;

    const auto nextVisibleTimeSlots = computeVisibleTimeSlots();
    if (nextVisibleTimeSlots != m_visibleTimeSlots) {
        m_visibleTimeSlots = nextVisibleTimeSlots;
        emit visibleTimeSlotsChanged();
    }

    auto normalizeRangeStart = [](const int value) {
        return std::max(0, value / kViewportRangeBucketMinutes * kViewportRangeBucketMinutes);
    };
    auto normalizeRangeEnd = [](const int value) {
        return std::max(
            kViewportRangeBucketMinutes,
            ((std::max(0, value) + kViewportRangeBucketMinutes - 1) / kViewportRangeBucketMinutes) * kViewportRangeBucketMinutes);
    };

    auto rangeStart = normalizeRangeStart(normalizedStart - kViewportTimePrefetchMinutes);
    auto rangeEnd = normalizeRangeEnd(normalizedStart + normalizedDuration + kViewportTimePrefetchMinutes);
    rangeStart = std::clamp(rangeStart, 0, m_windowSpanMinutes);
    rangeEnd = std::clamp(rangeEnd, rangeStart, m_windowSpanMinutes);
    if (rangeEnd <= rangeStart) {
        rangeEnd = std::min(m_windowSpanMinutes, rangeStart + std::max(kViewportMinDurationMinutes, normalizedDuration));
    }

    auto previousRangeStart = m_renderProgramsRangeStartMinutes;
    auto previousRangeEnd = m_renderProgramsRangeEndMinutes;
    if (previousRangeStart < 0 || previousRangeEnd <= previousRangeStart) {
        previousRangeStart = normalizeRangeStart(previousViewportStart - kViewportTimePrefetchMinutes);
        previousRangeEnd = normalizeRangeEnd(previousViewportStart + previousViewportDuration + kViewportTimePrefetchMinutes);
        previousRangeStart = std::clamp(previousRangeStart, 0, m_windowSpanMinutes);
        previousRangeEnd = std::clamp(previousRangeEnd, previousRangeStart, m_windowSpanMinutes);
        if (previousRangeEnd <= previousRangeStart) {
            previousRangeEnd = std::min(
                m_windowSpanMinutes,
                previousRangeStart + std::max(kViewportMinDurationMinutes, previousViewportDuration));
        }
    }

    if (previousRangeStart == rangeStart && previousRangeEnd == rangeEnd) {
        return;
    }

    m_renderProgramsRangeStartMinutes = rangeStart;
    m_renderProgramsRangeEndMinutes = rangeEnd;
    invalidateProgramTilesCache();
    emitProgramsChangedForVisibleRows();
    scheduleOffscreenRowWarmup();
}

void EpgGridModel::setVisibleRowRange(int firstRow, int lastRow)
{
    if (m_rows.isEmpty()) {
        if (m_visibleRowStart != -1 || m_visibleRowEnd != -1) {
            m_visibleRowStart = -1;
            m_visibleRowEnd = -1;
            invalidateProgramTilesCache();
        }
        return;
    }

    if (firstRow > lastRow) {
        std::swap(firstRow, lastRow);
    }

    firstRow = std::clamp(firstRow, 0, static_cast<int>(m_rows.size()) - 1);
    lastRow = std::clamp(lastRow, 0, static_cast<int>(m_rows.size()) - 1);

    if (m_visibleRowStart == firstRow && m_visibleRowEnd == lastRow) {
        return;
    }

    const auto previousFirst = m_visibleRowStart;
    const auto previousLast = m_visibleRowEnd;
    m_visibleRowStart = firstRow;
    m_visibleRowEnd = lastRow;

    const auto keepFrom = std::max(0, m_visibleRowStart - kViewportRowPrefetch);
    const auto keepTo = std::max(keepFrom, m_visibleRowEnd + kViewportRowPrefetch);
    for (auto it = m_programTilesCacheByRow.begin(); it != m_programTilesCacheByRow.end();) {
        if (it.key() < keepFrom || it.key() > keepTo) {
            m_loadedProgramsByRow.remove(it.key());
            it = m_programTilesCacheByRow.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = m_loadedProgramsByRow.begin(); it != m_loadedProgramsByRow.end();) {
        if (it.key() < keepFrom || it.key() > keepTo)
            it = m_loadedProgramsByRow.erase(it);
        else
            ++it;
    }

    if (previousFirst < 0 || previousLast < previousFirst) {
        emitProgramsChangedForRange(m_visibleRowStart, m_visibleRowEnd);
        scheduleOffscreenRowWarmup();
        return;
    }

    if (m_visibleRowStart < previousFirst) {
        emitProgramsChangedForRange(m_visibleRowStart, previousFirst - 1);
    }
    if (m_visibleRowEnd > previousLast) {
        emitProgramsChangedForRange(previousLast + 1, m_visibleRowEnd);
    }

    scheduleOffscreenRowWarmup();
}

void EpgGridModel::emitProgramsChangedForRow(const int row)
{
    if (row < 0 || row >= rowCount()) {
        return;
    }
    emitProgramsChangedForRange(row, row);
}

void EpgGridModel::emitProgramsChangedForRange(int firstRow, int lastRow)
{
    if (rowCount() <= 0) {
        return;
    }

    firstRow = std::clamp(firstRow, 0, rowCount() - 1);
    lastRow = std::clamp(lastRow, 0, rowCount() - 1);
    if (lastRow < firstRow) {
        return;
    }

    emit dataChanged(index(firstRow, 0), index(lastRow, 0), { ProgramsRole });
}

void EpgGridModel::emitProgramsChangedForVisibleRows()
{
    if (rowCount() <= 0) {
        return;
    }

    if (m_visibleRowStart >= 0 && m_visibleRowEnd >= m_visibleRowStart) {
        emitProgramsChangedForRange(m_visibleRowStart, m_visibleRowEnd);
        return;
    }

    emitProgramsChangedForRange(0, std::min(rowCount() - 1, 24));
}

void EpgGridModel::invalidateProgramTilesCache()
{
    ++m_rowWarmupGeneration;
    m_tilesCancelled->store(true);
    m_tilesCancelled = std::make_shared<std::atomic_bool>(false);
    m_pendingRows.clear();
    m_loadedProgramsByRow.clear();
    m_programTilesCacheByRow.clear();
}

void EpgGridModel::scheduleOffscreenRowWarmup()
{
    if (m_rows.isEmpty() || m_visibleRowStart < 0 || m_visibleRowEnd < m_visibleRowStart) {
        return;
    }

    const auto generation = ++m_rowWarmupGeneration;
    QTimer::singleShot(kOffscreenWarmupDelayMs, this, [this, generation]() {
        if (generation != m_rowWarmupGeneration) {
            return;
        }

        if (m_rows.isEmpty() || m_visibleRowStart < 0 || m_visibleRowEnd < m_visibleRowStart) {
            return;
        }

        const auto from = std::max(0, m_visibleRowStart - kViewportRowPrefetch);
        const auto to = std::min(static_cast<int>(m_rows.size()) - 1, m_visibleRowEnd + kViewportRowPrefetch);
        warmProgramTilesForRows(from, to, generation);
    });
}

void EpgGridModel::warmProgramTilesForRows(int firstRow, int lastRow, const quint64 generation)
{
    if (generation != m_rowWarmupGeneration || m_rows.isEmpty()) {
        return;
    }

    firstRow = std::clamp(firstRow, 0, static_cast<int>(m_rows.size()) - 1);
    lastRow = std::clamp(lastRow, 0, static_cast<int>(m_rows.size()) - 1);
    if (lastRow < firstRow) {
        return;
    }

    int warmedRows = 0;
    for (auto rowIndex = firstRow; rowIndex <= lastRow; ++rowIndex) {
        if (m_programTilesCacheByRow.contains(rowIndex)) {
            continue;
        }
        const auto &row = m_rows.at(rowIndex);
        buildPrograms(rowIndex, row);
        if (++warmedRows == 2 && rowIndex < lastRow) {
            QTimer::singleShot(16, this, [this, rowIndex, lastRow, generation]() {
                warmProgramTilesForRows(rowIndex + 1, lastRow, generation);
            });
            return;
        }
    }
}

void EpgGridModel::applyRows(
    const QList<Channel> &channels,
    QList<Row> rows,
    const int guidePastHours,
    const int lookAheadHours,
    // Ordered time-window bounds shared by synchronous and asynchronous rebuilds.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const QDateTime &windowStart,
    const QDateTime &resolvedWindowEnd)
{
    m_channels = channels;
    m_guidePastHours = guidePastHours;
    m_lookAheadHours = lookAheadHours;
    m_windowStart = windowStart;

    const auto minimumWindowSpanMinutes = (std::max(1, guidePastHours) + std::max(1, lookAheadHours)) * 60;
    auto computedWindowSpanMinutes = minimumWindowSpanMinutes;
    if (resolvedWindowEnd.isValid()) {
        const auto spanSeconds = std::max<qint64>(0, m_windowStart.secsTo(resolvedWindowEnd));
        const auto spanMinutes = static_cast<int>((spanSeconds + 59) / 60);
        const auto roundedSpanMinutes = ((spanMinutes + 59) / 60) * 60;
        computedWindowSpanMinutes = std::max(minimumWindowSpanMinutes, roundedSpanMinutes);
    }
    m_windowSpanMinutes = computedWindowSpanMinutes;

    // A periodic EPG refresh changes programmes, not the channel list. Resetting
    // an unchanged list makes ListView discard its scroll position and delegates.
    const auto sameRows = m_rows.size() == rows.size()
        && std::equal(m_rows.cbegin(), m_rows.cend(), rows.cbegin(), [](const Row &oldRow, const Row &newRow) {
            return oldRow.channel.id == newRow.channel.id
                && oldRow.channel.profileId == newRow.channel.profileId;
        });
    if (!sameRows) {
        beginResetModel();
    }
    m_rows = std::move(rows);
    m_rowIndexByChannelId.clear();
    m_rowIndexByChannelId.reserve(m_rows.size());
    for (auto row = 0; row < m_rows.size(); ++row) {
        m_rowIndexByChannelId.insert(m_rows.at(row).channel.id, row);
    }

    if (m_rows.isEmpty()) {
        m_visibleRowStart = -1;
        m_visibleRowEnd = -1;
    } else {
        if (m_visibleRowStart < 0 || m_visibleRowEnd < m_visibleRowStart) {
            m_visibleRowStart = 0;
            m_visibleRowEnd = std::min(static_cast<int>(m_rows.size()) - 1, 24);
        } else {
            m_visibleRowStart = std::clamp(m_visibleRowStart, 0, static_cast<int>(m_rows.size()) - 1);
            m_visibleRowEnd = std::clamp(m_visibleRowEnd, m_visibleRowStart, static_cast<int>(m_rows.size()) - 1);
        }
    }

    invalidateProgramTilesCache();
    m_renderProgramsRangeStartMinutes = -1;
    m_renderProgramsRangeEndMinutes = -1;
    m_timeSlots = computeTimeSlots();
    m_visibleTimeSlots = computeVisibleTimeSlots();
    if (!sameRows) {
        endResetModel();
    } else if (!m_rows.isEmpty()) {
        emit dataChanged(index(0), index(static_cast<int>(m_rows.size()) - 1));
    }

    emit timeSlotsChanged();
    emit visibleTimeSlotsChanged();
    emit windowChanged();
    emit selectedProgramChanged();
    scheduleOffscreenRowWarmup();
    m_rebuildPending = false;
    emit rebuildPendingChanged();
}

QList<EpgGridModel::Row> EpgGridModel::buildRows(
    const QList<Channel> &channels,
    const int guidePastHours,
    const int lookAheadHours,
    const QDateTime &windowStart,
    QDateTime *resolvedWindowEnd) const
{
    QList<Row> rows;
    const auto &from = windowStart;
    const auto to = from.addSecs(minutesToSeconds((std::max(1, guidePastHours) + std::max(1, lookAheadHours)) * 60));

    for (const auto &channel : channels) {
        if (channel.tvgId.trimmed().isEmpty()) {
            continue;
        }

        if (m_epg->programsInRange(channel.tvgId, from, to, 1).isEmpty()) {
            continue;
        }

        rows.push_back(Row { channel });
    }

    if (resolvedWindowEnd != nullptr) {
        *resolvedWindowEnd = to;
    }

    return rows;
}

QVariantList EpgGridModel::computeTimeSlots() const
{
    QVariantList timeSlots;
    auto current = m_windowStart.toLocalTime();
    const auto slotCount = std::max(1, windowSpanMinutes() / 60);
    for (auto index = 0; index <= slotCount; ++index) {
        timeSlots.push_back(QVariantMap {
            { QStringLiteral("label"), formatDisplayTime(current, m_dateTimeFormat) },
            { QStringLiteral("isNow"), qAbs(index * 60.0 - currentTimeOffsetMinutes()) < 30.0 },
            { QStringLiteral("isHour"), true },
            { QStringLiteral("offsetMinutes"), index * 60 }
        });
        current = current.addSecs(kSecondsPerHour);
    }

    return timeSlots;
}

QVariantList EpgGridModel::computeVisibleTimeSlots() const
{
    QVariantList visibleSlots;
    if (m_windowSpanMinutes <= 0) {
        return visibleSlots;
    }

    auto startMinute = std::max(0, m_renderViewportStartMinutes - kViewportTimePrefetchMinutes);
    auto endMinute = std::min(
        m_windowSpanMinutes,
        m_renderViewportStartMinutes + m_renderViewportDurationMinutes + kViewportTimePrefetchMinutes);
    if (endMinute <= startMinute) {
        endMinute = std::min(m_windowSpanMinutes, startMinute + std::max(kViewportMinDurationMinutes, m_renderViewportDurationMinutes));
    }

    const auto startSlot = std::max(0, startMinute / 60);
    const auto endSlot = std::max(startSlot, (endMinute + 59) / 60);
    for (auto slot = startSlot; slot <= endSlot; ++slot) {
        auto value = m_windowStart.toLocalTime();
        value = value.addSecs(static_cast<qint64>(slot) * kSecondsPerHour);
        visibleSlots.push_back(QVariantMap {
            { QStringLiteral("label"), formatDisplayTime(value, m_dateTimeFormat) },
            { QStringLiteral("isNow"), qAbs(slot * 60.0 - currentTimeOffsetMinutes()) < 30.0 },
            { QStringLiteral("isHour"), true },
            { QStringLiteral("offsetMinutes"), slot * 60 }
        });
    }

    return visibleSlots;
}

QVariantList EpgGridModel::buildPrograms(const int rowIndex, const Row &row) const
{
    if (m_visibleRowStart >= 0 && m_visibleRowEnd >= m_visibleRowStart) {
        const auto boundedStart = m_visibleRowStart - kViewportRowPrefetch;
        const auto boundedEnd = m_visibleRowEnd + kViewportRowPrefetch;
        if (rowIndex < boundedStart || rowIndex > boundedEnd) {
            return {};
        }
    }

    const auto cached = m_programTilesCacheByRow.constFind(rowIndex);
    if (cached != m_programTilesCacheByRow.cend()) {
        return cached.value();
    }

    if (row.channel.tvgId.trimmed().isEmpty()) {
        m_programTilesCacheByRow.insert(rowIndex, {});
        return {};
    }

    auto renderStart = m_renderProgramsRangeStartMinutes >= 0
        ? m_renderProgramsRangeStartMinutes
        : std::max(0, m_renderViewportStartMinutes - kViewportTimePrefetchMinutes);
    auto renderEnd = m_renderProgramsRangeEndMinutes > renderStart
        ? m_renderProgramsRangeEndMinutes
        : std::min(
            m_windowSpanMinutes,
            m_renderViewportStartMinutes + m_renderViewportDurationMinutes + kViewportTimePrefetchMinutes);
    if (renderEnd <= renderStart) {
        renderEnd = std::min(m_windowSpanMinutes, renderStart + std::max(kViewportMinDurationMinutes, m_renderViewportDurationMinutes));
    }

    const auto from = m_windowStart.addSecs(minutesToSeconds(renderStart));
    const auto to = m_windowStart.addSecs(minutesToSeconds(renderEnd));
    QList<EpgEntry> programs;
    if (m_epg->snapshot()->store) {
        const auto loaded = m_loadedProgramsByRow.constFind(rowIndex);
        if (loaded == m_loadedProgramsByRow.cend()) {
            const_cast<EpgGridModel *>(this)->loadRowPrograms(rowIndex, from, to);
            return {};
        }
        programs = loaded.value();
    } else programs = m_epg->programsInRange(row.channel.tvgId, from, to);

    QVariantList tiles;
    tiles.reserve(programs.size());
    const auto nowUtc = QDateTime::currentDateTimeUtc();
    const auto fullWindowEnd = windowEnd();
    const auto selectedStartIso = m_selectedProgramStart.trimmed();
    for (auto index = 0; index < programs.size(); ++index) {
        const auto &program = programs.at(index);
        const std::optional<EpgEntry> nextProgram = index + 1 < programs.size()
            ? std::optional<EpgEntry>(programs.at(index + 1))
            : std::nullopt;
        const auto isSelected = row.channel.id == m_selectedChannelId && matchesProgramStart(program, selectedStartIso);
        auto tile = buildProgramMap(
            row.channel,
            program,
            m_windowStart,
            fullWindowEnd,
            nextProgram,
            nowUtc,
            isSelected, m_dateTimeFormat);
        tile.insert(QStringLiteral("detailsPending"), static_cast<bool>(m_epg->snapshot()->store));
        tiles.push_back(tile);
    }

    m_programTilesCacheByRow.insert(rowIndex, tiles);
    return tiles;
}

void EpgGridModel::loadRowPrograms(int rowIndex, const QDateTime &from, const QDateTime &to)
{
    if (m_pendingRows.contains(rowIndex)) return;
    m_pendingRows.insert(rowIndex);
    const auto channel = m_rows.at(rowIndex).channel;
    const auto token = m_tilesCancelled;
    m_backgroundTasks.addFuture(QtConcurrent::run(EpgService::readPool(),
        [this, rowIndex, channel, from, to, token, snapshot = m_epg->snapshot()] {
            if (token->load()) return;
            QList<EpgEntry> entries;
            try { entries = snapshot->store->range(channel.tvgId, from, to, -1, true); }
            catch (const std::exception &error) { DebugLogger::instance().log(QStringLiteral("epg.read"), QString::fromUtf8(error.what())); }
            QMetaObject::invokeMethod(this, [this, rowIndex, token, entries = std::move(entries)] {
                if (token->load()) return;
                m_pendingRows.remove(rowIndex);
                if (rowIndex < m_visibleRowStart - kViewportRowPrefetch || rowIndex > m_visibleRowEnd + kViewportRowPrefetch) return;
                m_loadedProgramsByRow.insert(rowIndex, entries);
                m_programTilesCacheByRow.remove(rowIndex);
                emitProgramsChangedForRow(rowIndex);
                emit selectedProgramChanged();
            }, Qt::QueuedConnection);
        }));
}

void EpgGridModel::cancelProgramRequest()
{
    ++m_navigationGeneration;
    m_navigationPending = false;
    m_navigationDelta = 0;
}

void EpgGridModel::requestProgram(int channelId, const QString &timestampIso, int delta, bool adjacent)
{
    const auto row = rowIndexForChannelId(channelId);
    if (row < 0) return;
    if (!m_epg->snapshot()->store) {
        emit programResolved(channelId, adjacent ? adjacentProgram(channelId, timestampIso, delta)
            : programForChannelAtTimestamp(channelId, timestampIso), adjacent);
        return;
    }
    if (adjacent && m_navigationPending && m_navigationChannel == channelId && m_navigationStart == timestampIso)
        m_navigationDelta += delta;
    else m_navigationDelta = delta;
    m_navigationPending = true; m_navigationChannel = channelId; m_navigationStart = timestampIso;
    const auto generation = ++m_navigationGeneration;
    const auto channel = m_rows.at(row).channel;
    const auto from = m_windowStart, to = windowEnd();
    const auto format = m_dateTimeFormat;
    delta = m_navigationDelta;
    m_backgroundTasks.addFuture(QtConcurrent::run(EpgService::readPool(),
        [this, generation, channel, from, to, format, timestampIso, delta, adjacent, snapshot = m_epg->snapshot()] {
            QVariantMap result;
            try {
                const auto programs = snapshot->store->range(channel.tvgId, from, to, -1, true);
                if (!programs.isEmpty()) {
                    qsizetype target = 0;
                    if (adjacent) {
                        bool found = false;
                        for (qsizetype i = 0; i < programs.size(); ++i) if (matchesProgramStart(programs.at(i), timestampIso)) {
                            target = std::clamp<qsizetype>(i + delta, 0, programs.size() - 1); found = true; break;
                        }
                        if (!found && delta < 0) target = programs.size() - 1;
                    } else {
                        const auto timestamp = fromIso(timestampIso);
                        auto nearest = std::numeric_limits<qint64>::max();
                        for (qsizetype i = 0; timestamp.isValid() && i < programs.size(); ++i) {
                            const auto &p = programs.at(i);
                            if (p.start <= timestamp && timestamp < p.stop) { target = i; break; }
                            const auto distance = qAbs(p.start.msecsTo(timestamp));
                            if (distance < nearest) { nearest = distance; target = i; }
                        }
                    }
                    const auto next = target + 1 < programs.size() ? std::optional<EpgEntry>(programs.at(target + 1)) : std::nullopt;
                    auto selected = programs.at(target);
                    for (const auto &full : snapshot->store->range(channel.tvgId, selected.start, selected.start.addMSecs(1)))
                        if (full.start == selected.start) { selected = full; break; }
                    result = buildProgramMap(channel, selected, from, to, next, QDateTime::currentDateTimeUtc(), true, format);
                }
            } catch (const std::exception &error) { DebugLogger::instance().log(QStringLiteral("epg.read"), QString::fromUtf8(error.what())); }
            QMetaObject::invokeMethod(this, [this, generation, channel, result, adjacent, snapshot] {
                if (generation != m_navigationGeneration || snapshot != m_epg->snapshot()) return;
                m_navigationPending = false; m_navigationDelta = 0;
                m_resolvedProgram = result;
                emit programResolved(channel.id, result, adjacent);
                emit selectedProgramChanged();
            }, Qt::QueuedConnection);
        }));
}

QList<EpgEntry> EpgGridModel::channelProgramsInRange(const int channelId, const QDateTime &from, const QDateTime &to) const
{
    const auto rowIndex = rowIndexForChannelId(channelId);
    if (rowIndex < 0) {
        return {};
    }

    const auto &channel = m_rows.at(rowIndex).channel;
    if (channel.tvgId.trimmed().isEmpty()) {
        return {};
    }

    if (m_epg->snapshot()->store) return {};
    return m_epg->programsInRange(channel.tvgId, from, to);
}

QList<EpgEntry> EpgGridModel::channelProgramsInWindow(const int channelId) const
{
    return channelProgramsInRange(channelId, m_windowStart, windowEnd());
}

QVariantMap EpgGridModel::findSelectedProgram() const
{
    if (m_epg->snapshot()->store) {
        if (m_resolvedProgram.value(QStringLiteral("channelIdInt")).toInt() == m_selectedChannelId
            && m_resolvedProgram.value(QStringLiteral("start")).toString() == m_selectedProgramStart) return m_resolvedProgram;
        for (const auto &tile : m_programTilesCacheByRow.value(rowIndexForChannelId(m_selectedChannelId)))
            if (tile.toMap().value(QStringLiteral("start")).toString() == m_selectedProgramStart) return tile.toMap();
        return {};
    }
    const auto selectedStart = m_selectedProgramStart.trimmed();
    if (selectedStart.isEmpty()) {
        return {};
    }
    const auto nowUtc = QDateTime::currentDateTimeUtc();
    const auto fullWindowEnd = windowEnd();

    if (!selectedStart.isEmpty() && m_selectedChannelId >= 0) {
        const auto rowIndex = rowIndexForChannelId(m_selectedChannelId);
        if (rowIndex < 0 || rowIndex >= m_rows.size()) {
            return {};
        }

        const auto &channel = m_rows.at(rowIndex).channel;
        const auto programs = channelProgramsInWindow(m_selectedChannelId);
        for (auto index = 0; index < programs.size(); ++index) {
            const auto &program = programs.at(index);
            if (!matchesProgramStart(program, selectedStart)) {
                continue;
            }

            const std::optional<EpgEntry> nextProgram = index + 1 < programs.size()
                ? std::optional<EpgEntry>(programs.at(index + 1))
                : std::nullopt;
            return buildProgramMap(
                channel,
                program,
                m_windowStart,
                fullWindowEnd,
                nextProgram,
                nowUtc,
                true, m_dateTimeFormat);
        }
        return {};
    }

    for (const auto &row : m_rows) {
        const auto programs = channelProgramsInWindow(row.channel.id);
        for (auto index = 0; index < programs.size(); ++index) {
            const auto &program = programs.at(index);
            if (!matchesProgramStart(program, selectedStart)) {
                continue;
            }

            const std::optional<EpgEntry> nextProgram = index + 1 < programs.size()
                ? std::optional<EpgEntry>(programs.at(index + 1))
                : std::nullopt;
            return buildProgramMap(
                row.channel,
                program,
                m_windowStart,
                fullWindowEnd,
                nextProgram,
                nowUtc,
                row.channel.id == m_selectedChannelId, m_dateTimeFormat);
        }
    }

    return {};
}

QDateTime EpgGridModel::defaultWindowStart(const int guidePastHours) const
{
    return roundToHour(QDateTime::currentDateTimeUtc().addSecs(-minutesToSeconds(std::max(1, guidePastHours) * 60)));
}

QDateTime EpgGridModel::windowEnd() const
{
    return m_windowStart.addSecs(minutesToSeconds(windowSpanMinutes()));
}

} // namespace OKILTV::App
