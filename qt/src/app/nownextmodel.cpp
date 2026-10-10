#include "nownextmodel.h"
#include "epgpresentationtime.h"

#include <QtConcurrent>
#include <QSet>

namespace OKILTV::App {

using namespace Core;

namespace {

QString cacheKeyFor(const Channel &channel, int lookAheadHours)
{
    return QStringLiteral("%1:%2:%3:%4:%5:%6")
        .arg(guidToString(channel.profileId)).arg(channel.id)
        .arg(channel.tvgId.trimmed().toLower()).arg(lookAheadHours)
        .arg(channel.catchupSupported).arg(channel.catchupWindowHours);
}

} // namespace

NowNextModel::NowNextModel(EpgService *epg, SettingsManager *settings, QObject *parent)
    : QObject(parent)
    , m_epg(epg)
    , m_settings(settings)
{
}

NowNextModel::~NowNextModel()
{
    ++m_refreshGeneration;
    m_backgroundTasks.waitForFinished();
}

QVariantMap NowNextModel::channel() const
{
    return m_channel.has_value() ? toVariantMap(m_channel.value()) : QVariantMap {};
}

QVariantList NowNextModel::pastPrograms() const
{
    return formatProgramTimes(m_pastProgramsVariant, resolveDateTimeFormat(
        m_settings->current().dateOrder, m_settings->current().timeFormat));
}

QString NowNextModel::channelName() const
{
    return m_channel.has_value() ? m_channel->name : QString {};
}

bool NowNextModel::loading() const
{
    return m_loading;
}

QVariantMap NowNextModel::currentProgram() const
{
    return formatProgramTimes(m_currentProgramVariant, resolveDateTimeFormat(
        m_settings->current().dateOrder, m_settings->current().timeFormat));
}

QVariantMap NowNextModel::nextProgram() const
{
    return formatProgramTimes(m_nextProgramVariant, resolveDateTimeFormat(
        m_settings->current().dateOrder, m_settings->current().timeFormat));
}

QVariantList NowNextModel::upcomingPrograms() const
{
    return formatProgramTimes(m_upcomingProgramsVariant, resolveDateTimeFormat(
        m_settings->current().dateOrder, m_settings->current().timeFormat));
}

void NowNextModel::setChannel(const std::optional<Channel> &channel)
{
    const auto changed = !m_channel.has_value() || !channel.has_value()
        || cacheKeyFor(m_channel.value(), 0) != cacheKeyFor(channel.value(), 0);
    if (changed) {
        clear(); // Never expose the previous channel's programmes under a new identity.
    }
    m_channel = channel;
    emit channelChanged();


    refresh();
}

void NowNextModel::refresh()
{
    const auto generation = ++m_refreshGeneration;
    const auto channel = m_channel;
    const auto lookAheadHours = normalizeGuideHours(m_settings->current().epgLookAheadHours);

    if (!channel.has_value() || channel->tvgId.trimmed().isEmpty()) {
        clear();
        return;
    }

    if (!m_loading && !m_skipNextLoadingState) {
        m_loading = true;
        emit dataChanged();
    }
    m_skipNextLoadingState = false;

    if (m_refreshInFlight) {
        m_refreshQueued = true;
        m_queuedGeneration = generation;
        m_queuedChannel = channel;
        m_queuedLookAheadHours = lookAheadHours;
        return;
    }

    startRefreshJob(generation, channel.value(), lookAheadHours);
}

void NowNextModel::refreshForClock()
{
    // Retain readable cached programmes while the periodic query runs.
    m_skipNextLoadingState = !m_cachedPrograms.isEmpty();
    refresh();
}

void NowNextModel::refreshTime(const QDateTime &nowUtc)
{
    if (!nowUtc.isValid()) return;
    updateEpgProgramTimes(m_cachedPrograms, nowUtc);
    m_currentProgramVariant.clear();
    m_nextProgramVariant.clear();
    m_pastProgramsVariant.clear();
    m_upcomingProgramsVariant.clear();
    const bool hasHistory = m_channel.has_value() && m_channel->catchupSupported && m_channel->catchupWindowHours > 0;
    const auto historyStart = hasHistory
        ? nowUtc.addSecs(-static_cast<qint64>(m_channel->catchupWindowHours) * 3600) : nowUtc;
    for (const auto &value : m_cachedPrograms) {
        const auto program = value.toMap();
        const auto start = QDateTime::fromString(program.value(QStringLiteral("start")).toString(), Qt::ISODateWithMs);
        const auto stop = QDateTime::fromString(program.value(QStringLiteral("stop")).toString(), Qt::ISODateWithMs);
        if (hasHistory && stop <= nowUtc && start >= historyStart) m_pastProgramsVariant.push_back(program);
        if (m_currentProgramVariant.isEmpty() && program.value(QStringLiteral("isNow")).toBool())
            m_currentProgramVariant = program;
    }
    const auto nextFrom = m_currentProgramVariant.isEmpty() ? nowUtc
        : QDateTime::fromString(m_currentProgramVariant.value(QStringLiteral("stop")).toString(), Qt::ISODateWithMs);
    for (const auto &value : m_cachedPrograms) {
        const auto program = value.toMap();
        const auto start = QDateTime::fromString(program.value(QStringLiteral("start")).toString(), Qt::ISODateWithMs);
        if (start < nowUtc) continue;
        if (m_nextProgramVariant.isEmpty() && start >= nextFrom) {
            m_nextProgramVariant = program;
        } else if (program.value(QStringLiteral("start")) != m_currentProgramVariant.value(QStringLiteral("start"))) {
            m_upcomingProgramsVariant.push_back(program);
        }
    }
    emit dataChanged();
}

void NowNextModel::startRefreshJob(const quint64 generation, const Channel &channel, const int lookAheadHours)
{
    m_refreshInFlight = true;
    m_backgroundTasks.addFuture(QtConcurrent::run(EpgService::readPool(), [this, generation, channel, lookAheadHours, snapshot = m_epg->snapshot()]() {
        EpgService reader; reader.applySnapshot(snapshot);
        const auto now = QDateTime::currentDateTimeUtc();
        const auto historyStart = channel.catchupSupported && channel.catchupWindowHours > 0
            ? now.addSecs(-static_cast<qint64>(channel.catchupWindowHours) * 3600) : now;
        const auto candidates = reader.programsForChannels({channel.tvgId}, historyStart,
            now.addSecs(static_cast<qint64>(lookAheadHours) * 3600), -1, true).value(channel.tvgId.trimmed().toLower());

        // XMLTV can contain duplicate entries. Programme identity does not depend on its title.
        QList<EpgEntry> entries;
        QSet<qint64> starts;
        for (const auto &entry : candidates) {
            const auto startMs = entry.start.toMSecsSinceEpoch();
            if (entry.stop <= entry.start || starts.contains(startMs)) {
                continue;
            }
            starts.insert(startMs);
            entries.push_back(entry);
        }

        // Hydrate current/next in the worker. Clock ticks can project all retained
        // summaries immediately, including an upcoming programme becoming current.
        std::optional<EpgEntry> current;
        for (const auto &entry : entries) {
            if (entry.start <= now && now < entry.stop) { current = entry; break; }
        }
        const auto nextFrom = current.has_value() ? current->stop : now;
        std::optional<EpgEntry> next;
        for (const auto &entry : entries) {
            if (entry.start >= nextFrom) { next = entry; break; }
        }
        RefreshResult result;
        result.programs.reserve(entries.size());
        for (const auto &entry : entries) {
            auto program = toVariantMap(entry);
            if (snapshot->store) program.insert(QStringLiteral("detailsPending"), true);
            if ((current.has_value() && entry.start == current->start)
                || (next.has_value() && entry.start == next->start)) {
                for (const auto &full : reader.programsInRange(channel.tvgId, entry.start, entry.start.addMSecs(1))) {
                    if (full.start == entry.start) { program = toVariantMap(full); break; }
                }
            }
            result.programs.push_back(program);
        }

        QMetaObject::invokeMethod(
            this,
            [this, generation, result = std::move(result)]() mutable {
                applyRefreshResult(generation, std::move(result));
            },
            Qt::QueuedConnection);
    }));
}

void NowNextModel::clear()
{
    ++m_refreshGeneration;
    m_refreshQueued = false;
    m_queuedGeneration = 0;
    m_queuedChannel = std::nullopt;
    m_queuedLookAheadHours = 24;
    m_loading = false;
    m_cachedPrograms.clear();
    m_currentProgramVariant.clear();
    m_nextProgramVariant.clear();
    m_upcomingProgramsVariant.clear();
    m_pastProgramsVariant.clear();
    m_skipNextLoadingState = false;
    emit dataChanged();
}

void NowNextModel::applyRefreshResult(const quint64 generation, RefreshResult result)
{
    m_refreshInFlight = false;
    if (m_refreshQueued && m_queuedChannel.has_value()) {
        const auto queuedGeneration = m_queuedGeneration;
        const auto queuedChannel = m_queuedChannel.value();
        const auto queuedLookAheadHours = m_queuedLookAheadHours;
        m_refreshQueued = false;
        m_queuedGeneration = 0;
        m_queuedChannel = std::nullopt;
        m_queuedLookAheadHours = 24;
        startRefreshJob(queuedGeneration, queuedChannel, queuedLookAheadHours);
        return;
    }

    m_refreshQueued = false;
    m_queuedGeneration = 0;
    m_queuedChannel = std::nullopt;
    m_queuedLookAheadHours = 24;
    if (generation != m_refreshGeneration) {
        if (m_loading) {
            m_loading = false;
            emit dataChanged();
        }
        return;
    }

    m_loading = false;
    m_cachedPrograms = std::move(result.programs);
    // Publication may occur after a minute boundary or a wall-clock correction.
    // Never publish the worker's earlier classification/progress sample.
    refreshTime(QDateTime::currentDateTimeUtc());
}

} // namespace OKILTV::App
