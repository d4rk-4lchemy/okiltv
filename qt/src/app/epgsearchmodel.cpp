#include "epgsearchmodel.h"
#include <QUrl>
#include <algorithm>
namespace OKILTV::App {
int EpgSearchModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}
const Core::EpgSearchRow* EpgSearchModel::row(int index) const
{
    return index >= 0 && index < m_rows.size() ? &m_rows.at(index) : nullptr;
}
QString EpgSearchModel::timeLabel(const Core::EpgSearchRow& r) const
{
    return Core::formatDisplayDateTime(r.program.start, m_format.timelineDatePattern())
        + QStringLiteral(" · ") + Core::formatDisplayTimeRange(r.program.start, r.program.stop, m_format);
}
QVariant EpgSearchModel::data(const QModelIndex& index, int role) const
{
    const auto* r = row(index.row());
    if (!index.isValid() || !r)
        return { };
    switch (role) {
    case ProfileIdRole:
        return Core::guidToString(r->channel.profileId);
    case PlaybackChannelIdRole:
        return r->channel.id;
    case EpgChannelKeyRole:
        return r->program.channelId;
    case StartUtcMsRole:
        return r->program.start.toMSecsSinceEpoch();
    case StopUtcMsRole:
        return r->program.stop.toMSecsSinceEpoch();
    case ChannelRole:
        return Core::toVariantMap(r->channel);
    case ProgramRole: {
        auto program = Core::toVariantMap(r->program, m_format);
        program.insert(QStringLiteral("detailsPending"), m_summaries);
        return program;
    }
    case ResultKeyRole:
        return r->resultKey;
    case TitleRole:
        return r->program.title;
    case SubTitleRole:
        return r->program.subTitle;
    case EpisodeNumRole:
        return r->program.episodeNum;
    case ChannelNameRole:
        return r->channel.name;
    case ChannelLogoRole:
        return r->channel.cachedIconPath.isEmpty() ? r->channel.iconUrl
                                                 : QUrl::fromLocalFile(r->channel.cachedIconPath).toString();
    case SectionKeyRole:
        return r->sectionKey;
    case TimeLabelRole:
        return timeLabel(*r);
    case BroadcastStateRole:
    case StatusLabelRole: {
        const bool state = role == BroadcastStateRole;
        if (m_nowUtc < r->program.start)
            return state ? QStringLiteral("upcoming") : tr("Upcoming");
        if (m_nowUtc >= r->program.stop)
            return state ? QStringLiteral("past") : tr("Past broadcast");
        return state ? QStringLiteral("now")
                     : tr("On now · %1 min remaining").arg(std::max<qint64>(1, m_nowUtc.secsTo(r->program.stop) / 60));
    }
    case TitleHighlightsRole:
        return r->titleHighlights;
    case SubTitleHighlightsRole:
        return r->subTitleHighlights;
    default:
        return { };
    }
}
QHash<int, QByteArray> EpgSearchModel::roleNames() const
{
    return { { ResultKeyRole, "resultKey" }, { TitleRole, "title" }, { SubTitleRole, "subTitle" },
        { EpisodeNumRole, "episodeNum" }, { ChannelNameRole, "channelName" },
        { ChannelLogoRole, "channelLogo" }, { SectionKeyRole, "sectionKey" }, { TimeLabelRole, "timeLabel" },
        { StatusLabelRole, "statusLabel" }, { TitleHighlightsRole, "titleHighlights" },
        { SubTitleHighlightsRole, "subTitleHighlights" }, { ProfileIdRole, "profileId" },
        { PlaybackChannelIdRole, "playbackChannelId" }, { EpgChannelKeyRole, "epgChannelKey" },
        { StartUtcMsRole, "startUtcMs" }, { StopUtcMsRole, "stopUtcMs" }, { ChannelRole, "channel" },
        { ProgramRole, "program" }, { BroadcastStateRole, "broadcastState" } };
}
void EpgSearchModel::replace(QList<Core::EpgSearchRow> rows)
{
    beginResetModel();
    m_nowUtc = QDateTime::currentDateTimeUtc();
    m_rows = std::move(rows);
    endResetModel();
}
void EpgSearchModel::append(const QList<Core::EpgSearchRow>& rows)
{
    QList<Core::EpgSearchRow> added;
    for (const auto& r : rows) {
        const bool duplicate = std::any_of(added.cbegin(), added.cend(),
            [&](const auto& existing) { return existing.resultKey == r.resultKey; });
        if (!duplicate && indexOfKey(r.resultKey) < 0)
            added.append(r);
    }
    if (added.isEmpty())
        return;
    const auto first = static_cast<int>(m_rows.size());
    beginInsertRows({ }, first, first + static_cast<int>(added.size()) - 1);
    m_rows.append(added);
    endInsertRows();
}
int EpgSearchModel::indexOfKey(const QString& key) const
{
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows.at(i).resultKey == key)
            return i;
    return -1;
}
void EpgSearchModel::refreshLabels(QDateTime nowUtc)
{
    m_nowUtc = std::move(nowUtc);
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(rowCount() - 1), { TimeLabelRole, StatusLabelRole, BroadcastStateRole });
}
void EpgSearchModel::setSummaries(bool summaries)
{
    if (m_summaries == summaries)
        return;
    m_summaries = summaries;
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(rowCount() - 1), { ProgramRole });
}
void EpgSearchModel::setDateTimeFormat(Core::DateTimeFormatOptions options)
{
    m_format = options;
    refreshLabels();
}
} // namespace OKILTV::App
