#include "guidestatemodel.h"

#include <QtConcurrent>

#include <algorithm>

namespace OKILTV::App {

using namespace Core;

GuideStateModel::GuideStateModel(EpgService *epg, SettingsManager *settings, QObject *parent)
    : QObject(parent)
    , m_epg(epg)
    , m_settings(settings)
    , m_previewEnabled(settings->current().guidePreviewEnabled)
{
}

GuideStateModel::~GuideStateModel()
{
    ++m_updateGeneration;
    m_backgroundTasks.waitForFinished();
}

QString GuideStateModel::selectedGroupId() const
{
    return m_selectedGroupId;
}

int GuideStateModel::selectedChannelId() const
{
    return m_selectedChannel.has_value() ? m_selectedChannel->id : -1;
}

QVariantMap GuideStateModel::selectedChannel() const
{
    return m_selectedChannel.has_value() ? toVariantMap(m_selectedChannel.value()) : QVariantMap {};
}

QVariantMap GuideStateModel::selectedProgram() const
{
    return m_selectedProgramVariant;
}

QVariantList GuideStateModel::channelPrograms() const
{
    return m_channelProgramsVariant;
}

bool GuideStateModel::previewEnabled() const
{
    return m_previewEnabled;
}

bool GuideStateModel::detailsExpanded() const
{
    return m_detailsExpanded;
}

void GuideStateModel::setChannels(const QList<Channel> &channels)
{
    const auto previousSelectedChannel = m_selectedChannel;
    m_channels = channels;

    if (!previousSelectedChannel.has_value()) {
        return;
    }

    const auto it = std::find_if(m_channels.cbegin(), m_channels.cend(), [&previousSelectedChannel](const Channel &channel) {
        return channel.id == previousSelectedChannel->id;
    });
    if (it == m_channels.cend()) {
        clear();
        return;
    }

    m_selectedChannel = *it;
    emit selectedChannelChanged();

    if (previousSelectedChannel->tvgId.trimmed() != it->tvgId.trimmed()) {
        m_preferredProgramStart.clear();
        updatePrograms();
    }
}

void GuideStateModel::selectChannel(const int channelId)
{
    const auto it = std::find_if(m_channels.cbegin(), m_channels.cend(), [channelId](const Channel &channel) {
        return channel.id == channelId;
    });
    if (it == m_channels.cend()) {
        clear();
        return;
    }

    const auto previousId = selectedChannelId();
    m_selectedChannel = *it;
    if (previousId == channelId) {
        emit selectedChannelChanged();
        return;
    }

    m_preferredProgramStart.clear();
    emit selectedChannelIdChanged();
    emit selectedChannelChanged();
    updatePrograms();
}

void GuideStateModel::selectProgram(const QVariantMap &program)
{
    ++m_updateGeneration;
    m_preferredProgramStart = program.value(QStringLiteral("start")).toString().trimmed();
    EpgEntry entry;
    entry.channelId = program.value(QStringLiteral("channelId")).toString();
    entry.title = program.value(QStringLiteral("title")).toString();
    entry.subTitle = program.value(QStringLiteral("subTitle")).toString();
    entry.description = program.value(QStringLiteral("description")).toString();
    entry.episodeNum = program.value(QStringLiteral("episodeNum")).toString();
    entry.start = QDateTime::fromString(program.value(QStringLiteral("start")).toString(), Qt::ISODateWithMs);
    if (!entry.start.isValid()) {
        entry.start = QDateTime::fromString(program.value(QStringLiteral("start")).toString(), Qt::ISODate);
    }
    entry.stop = QDateTime::fromString(program.value(QStringLiteral("stop")).toString(), Qt::ISODateWithMs);
    if (!entry.stop.isValid()) {
        entry.stop = QDateTime::fromString(program.value(QStringLiteral("stop")).toString(), Qt::ISODate);
    }
    m_selectedProgram = entry;
    m_selectedProgramVariant = toVariantMap(entry);
    emit selectedProgramChanged();
}

void GuideStateModel::selectProgramByStart(const QString &startIso)
{
    m_preferredProgramStart = startIso.trimmed();
    for (const auto &program : m_channelPrograms) {
        if (program.start.toUTC().toString(Qt::ISODateWithMs) == m_preferredProgramStart
            || program.start.toUTC().toString(Qt::ISODate) == m_preferredProgramStart) {
            ++m_updateGeneration;
            m_selectedProgram = program;
            m_selectedProgramVariant = toVariantMap(program);
            emit selectedProgramChanged();
            return;
        }
    }
}

void GuideStateModel::setSelectedGroupId(const QString &value)
{
    if (m_selectedGroupId == value) {
        return;
    }

    m_selectedGroupId = value;
    emit selectedGroupIdChanged();
}

void GuideStateModel::setPreviewEnabled(const bool value)
{
    if (m_previewEnabled == value) {
        return;
    }

    m_previewEnabled = value;
    emit previewEnabledChanged();
}

void GuideStateModel::setDetailsExpanded(const bool value)
{
    if (m_detailsExpanded == value) {
        return;
    }

    m_detailsExpanded = value;
    emit detailsExpandedChanged();
}

void GuideStateModel::refresh()
{
    updatePrograms();
}

void GuideStateModel::clear()
{
    ++m_updateGeneration;
    m_updateQueued = false;
    m_queuedGeneration = 0;
    m_queuedChannel = std::nullopt;
    const auto previousId = selectedChannelId();
    m_selectedChannel = std::nullopt;
    m_selectedProgram = std::nullopt;
    m_channelPrograms.clear();
    m_selectedProgramVariant.clear();
    m_channelProgramsVariant.clear();
    m_preferredProgramStart.clear();
    if (previousId != -1) {
        emit selectedChannelIdChanged();
    }
    emit selectedChannelChanged();
    emit selectedProgramChanged();
    emit channelProgramsChanged();
}

void GuideStateModel::startProgramsUpdateJob(
    const quint64 generation,
    const Channel &selectedChannel,
    const int lookAheadHours)
{
    m_updateInFlight = true;
    m_backgroundTasks.addFuture(QtConcurrent::run([this, generation, selectedChannel, lookAheadHours]() {
        ProgramsUpdate result;
        const auto from = QDateTime::currentDateTimeUtc().addSecs(-1800);
        const auto to = QDateTime::currentDateTimeUtc().addSecs(static_cast<qint64>(lookAheadHours) * 3600);
        result.channelPrograms = m_epg->programsInRange(selectedChannel.tvgId, from, to);
        for (const auto &program : result.channelPrograms) {
            if (epgEntryIsNow(program)) {
                result.selectedProgram = program;
                break;
            }
        }
        if (!result.selectedProgram.has_value() && !result.channelPrograms.isEmpty()) {
            result.selectedProgram = result.channelPrograms.first();
        }
        if (result.selectedProgram.has_value()) {
            result.selectedProgramVariant = toVariantMap(result.selectedProgram.value());
        }
        result.channelProgramsVariant = toVariantList(result.channelPrograms);

        QMetaObject::invokeMethod(
            this,
            [this, generation, result = std::move(result)]() mutable {
                applyProgramsUpdate(generation, std::move(result));
            },
            Qt::QueuedConnection);
    }));
}

void GuideStateModel::updatePrograms()
{
    const auto generation = ++m_updateGeneration;
    const auto selectedChannel = m_selectedChannel;
    const auto lookAheadHours = m_settings->current().epgLookAheadHours;

    if (!selectedChannel.has_value() || selectedChannel->tvgId.trimmed().isEmpty()) {
        m_updateQueued = false;
        m_queuedGeneration = 0;
        m_queuedChannel = std::nullopt;
        m_selectedProgram = std::nullopt;
        m_channelPrograms.clear();
        m_selectedProgramVariant.clear();
        m_channelProgramsVariant.clear();
        emit selectedProgramChanged();
        emit channelProgramsChanged();
        return;
    }

    if (m_updateInFlight) {
        m_updateQueued = true;
        m_queuedGeneration = generation;
        m_queuedChannel = selectedChannel;
        m_queuedLookAheadHours = lookAheadHours;
        return;
    }

    startProgramsUpdateJob(generation, selectedChannel.value(), lookAheadHours);
}

void GuideStateModel::applyProgramsUpdate(const quint64 generation, ProgramsUpdate result)
{
    m_updateInFlight = false;
    if (m_updateQueued && m_queuedChannel.has_value()) {
        const auto queuedGeneration = m_queuedGeneration;
        const auto queuedChannel = m_queuedChannel.value();
        const auto queuedLookAheadHours = m_queuedLookAheadHours;
        m_updateQueued = false;
        m_queuedGeneration = 0;
        m_queuedChannel = std::nullopt;
        startProgramsUpdateJob(queuedGeneration, queuedChannel, queuedLookAheadHours);
        return;
    }

    m_updateQueued = false;
    m_queuedGeneration = 0;
    m_queuedChannel = std::nullopt;
    if (generation != m_updateGeneration) {
        return;
    }

    if (!m_preferredProgramStart.isEmpty()) {
        for (const auto &program : result.channelPrograms) {
            const auto startIsoWithMs = program.start.toUTC().toString(Qt::ISODateWithMs);
            const auto startIso = program.start.toUTC().toString(Qt::ISODate);
            if (startIsoWithMs != m_preferredProgramStart && startIso != m_preferredProgramStart) {
                continue;
            }

            result.selectedProgram = program;
            result.selectedProgramVariant = toVariantMap(program);
            break;
        }
    }

    m_selectedProgram = std::move(result.selectedProgram);
    m_channelPrograms = std::move(result.channelPrograms);
    m_selectedProgramVariant = std::move(result.selectedProgramVariant);
    m_channelProgramsVariant = std::move(result.channelProgramsVariant);
    emit selectedProgramChanged();
    emit channelProgramsChanged();
}

} // namespace OKILTV::App
