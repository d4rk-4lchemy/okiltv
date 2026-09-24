#include "nownextmodel.h"

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

bool sameEntry(const EpgEntry &left, const EpgEntry &right)
{
    return left.channelId == right.channelId
        && left.start == right.start
        && left.stop == right.stop
        && left.title == right.title;
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

        RefreshResult result;
        for (const auto &entry : entries) {
            if (entry.stop <= now && entry.start >= historyStart) {
                result.pastProgramsVariant.push_back(toVariantMap(entry));
            }
        }
        for (const auto &entry : entries) {
            if (entry.start <= now && now < entry.stop) {
                result.currentProgram = entry;
                break;
            }
        }

        const auto nextFrom = result.currentProgram.has_value() ? result.currentProgram->stop : now;
        for (const auto &entry : entries) {
            if (entry.start >= nextFrom) {
                result.nextProgram = entry;
                break;
            }
        }

        for (const auto &entry : entries) {
            if (entry.start < now) {
                continue;
            }

            const auto isCurrent = result.currentProgram.has_value() && sameEntry(entry, result.currentProgram.value());
            const auto isNext = result.nextProgram.has_value() && sameEntry(entry, result.nextProgram.value());
            if (!isCurrent && !isNext) {
                result.upcomingPrograms.push_back(entry);
            }
        }

        for (auto *selected : {&result.currentProgram, &result.nextProgram}) {
            if (!selected->has_value()) continue;
            const auto start = selected->value().start;
            for (const auto &entry : reader.programsInRange(channel.tvgId, start, start.addMSecs(1)))
                if (entry.start == start) { *selected = entry; break; }
        }
        if (result.currentProgram.has_value()) {
            result.currentProgramVariant = toVariantMap(result.currentProgram.value());
        }
        if (result.nextProgram.has_value()) {
            result.nextProgramVariant = toVariantMap(result.nextProgram.value());
        }
        result.upcomingProgramsVariant = toVariantList(result.upcomingPrograms);
        if (snapshot->store) {
            for (auto *list : {&result.pastProgramsVariant, &result.upcomingProgramsVariant})
                for (auto &value : *list) { auto map = value.toMap(); map.insert(QStringLiteral("detailsPending"), true); value = map; }
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
    m_currentProgram = std::nullopt;
    m_nextProgram = std::nullopt;
    m_upcomingPrograms.clear();
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
    m_currentProgram = std::move(result.currentProgram);
    m_nextProgram = std::move(result.nextProgram);
    m_upcomingPrograms = std::move(result.upcomingPrograms);
    m_currentProgramVariant = std::move(result.currentProgramVariant);
    m_nextProgramVariant = std::move(result.nextProgramVariant);
    m_upcomingProgramsVariant = std::move(result.upcomingProgramsVariant);
    m_pastProgramsVariant = std::move(result.pastProgramsVariant);
    emit dataChanged();
}

} // namespace OKILTV::App
