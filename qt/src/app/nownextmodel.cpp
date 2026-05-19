#include "nownextmodel.h"

#include <QtConcurrent>

namespace OKILTV::App {

using namespace Core;

namespace {

bool sameEntry(const EpgEntry &left, const EpgEntry &right)
{
    return left.channelId == right.channelId
        && left.start == right.start
        && left.stop == right.stop
        && left.title == right.title;
}

} // namespace

NowNextModel::NowNextModel(EpgService *epg, QObject *parent)
    : QObject(parent)
    , m_epg(epg)
{
}

NowNextModel::~NowNextModel()
{
    ++m_refreshGeneration;
    m_backgroundTasks.waitForFinished();
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
    return m_currentProgramVariant;
}

QVariantMap NowNextModel::nextProgram() const
{
    return m_nextProgramVariant;
}

QVariantList NowNextModel::upcomingPrograms() const
{
    return m_upcomingProgramsVariant;
}

void NowNextModel::setChannel(const std::optional<Channel> &channel)
{
    m_channel = channel;
    emit channelChanged();

    if (channel.has_value() && !channel->tvgId.trimmed().isEmpty()) {
        const auto cacheKey = channel->tvgId.trimmed().toLower();
        const auto it = m_resultCache.constFind(cacheKey);
        if (it != m_resultCache.constEnd()) {
            m_loading = false;
            m_currentProgramVariant = it->currentProgramVariant;
            m_nextProgramVariant = it->nextProgramVariant;
            m_upcomingProgramsVariant = it->upcomingProgramsVariant;
            m_skipNextLoadingState = true;
            emit dataChanged();
        }
    }

    refresh();
}

void NowNextModel::refresh()
{
    const auto generation = ++m_refreshGeneration;
    const auto channel = m_channel;

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
        return;
    }

    startRefreshJob(generation, channel.value());
}

void NowNextModel::startRefreshJob(const quint64 generation, const Channel &channel)
{
    m_refreshInFlight = true;
    m_backgroundTasks.addFuture(QtConcurrent::run([this, generation, channel]() {
        const auto now = QDateTime::currentDateTimeUtc();
        const auto entries = m_epg->programsInRange(channel.tvgId, now, now.addSecs(static_cast<qint64>(24) * 3600));

        RefreshResult result;
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

        if (result.currentProgram.has_value()) {
            result.currentProgramVariant = toVariantMap(result.currentProgram.value());
        }
        if (result.nextProgram.has_value()) {
            result.nextProgramVariant = toVariantMap(result.nextProgram.value());
        }
        result.upcomingProgramsVariant = toVariantList(result.upcomingPrograms);

        QMetaObject::invokeMethod(
            this,
            [this, generation, tvgId = channel.tvgId.trimmed().toLower(), result = std::move(result)]() mutable {
                if (!tvgId.isEmpty()) {
                    m_resultCache[tvgId] = CachedResult {
                        result.currentProgramVariant,
                        result.nextProgramVariant,
                        result.upcomingProgramsVariant
                    };
                }
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
    m_loading = false;
    m_currentProgram = std::nullopt;
    m_nextProgram = std::nullopt;
    m_upcomingPrograms.clear();
    m_currentProgramVariant.clear();
    m_nextProgramVariant.clear();
    m_upcomingProgramsVariant.clear();
    emit dataChanged();
}

void NowNextModel::applyRefreshResult(const quint64 generation, RefreshResult result)
{
    m_refreshInFlight = false;
    if (m_refreshQueued && m_queuedChannel.has_value()) {
        const auto queuedGeneration = m_queuedGeneration;
        const auto queuedChannel = m_queuedChannel.value();
        m_refreshQueued = false;
        m_queuedGeneration = 0;
        m_queuedChannel = std::nullopt;
        startRefreshJob(queuedGeneration, queuedChannel);
        return;
    }

    m_refreshQueued = false;
    m_queuedGeneration = 0;
    m_queuedChannel = std::nullopt;
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
    emit dataChanged();
}

} // namespace OKILTV::App
