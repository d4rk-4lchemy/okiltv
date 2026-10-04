#include "vodepisodesmodel.h"
#include <algorithm>

namespace OKILTV::Vod {
VodEpisodesModel::VodEpisodesModel(VodRuntime *runtime, QObject *parent) : QAbstractListModel(parent), m_runtime(runtime) {
    connect(this, &VodEpisodesModel::changed, this, &VodEpisodesModel::updateRows);
    connect(runtime, &VodRuntime::sourceInvalidated, this, [this](const QUuid &id) { if (id==m_series.profileId) clear(); });
    connect(runtime, &VodRuntime::stateChanged, this, [this]() {
        if (!m_runtime->module()->session()) return;
        const auto &snapshot=m_runtime->module()->session()->snapshot();
        if (parentSeries(snapshot.ref)!=m_series || !snapshot.positionValid) return;
        m_history.episodes.insert(snapshot.ref.key(),m_runtime->module()->progressService()->observedProgress(snapshot));
        emit changed();
    });
}
void VodEpisodesModel::clear() {
    if (m_controller) for (const auto &id : m_pending.keys()) m_controller->cancel(id);
    if (m_controller) for (const auto &id : m_artworkRequests.keys()) m_controller->cancel(id);
    m_artworkRequests.clear(); m_requestedArtwork.clear();
    ++m_generation; m_historyLoaded = false;
    m_pending.clear(); m_series={}; m_selected={}; m_details={}; m_history={}; m_season.clear(); m_error.clear(); emit changed();
}
void VodEpisodesModel::load(const ContentRef &series,const ContentRef &preferred,bool refresh) {
    if (series == m_series && refresh) {
        if (m_controller) for (const auto &id : m_pending.keys()) m_controller->cancel(id);
        m_pending.clear(); ++m_generation;
    } else clear();
    if (!series.valid() || series.kind!=ContentKind::Series || !m_runtime->ready()) return;
    auto *controller=m_runtime->module()->controller();
    if (m_controller!=controller) {
        m_controller=controller;
        connect(controller,&VodController::eventCompleted,this,&VodEpisodesModel::receive);
        connect(m_runtime->module()->progressService(),&VodProgressService::persisted,this,[this](const ContentRef &ref) {
            if (parentSeries(ref)==m_series) refreshHistory();
        });
        connect(m_runtime->module()->progressService(), &VodProgressService::statusWritePendingChanged, this, [this](const ContentRef &ref) {
            if (parentSeries(ref) == m_series) emit changed();
        });
    }
    m_series=series; m_preferred=preferred;
    m_pending.insert(controller->details(series,refresh),true);
    m_pending.insert(controller->seriesProgress(series),false);
    emit changed();
}
QVariantMap VodEpisodesModel::present(const EpisodeSummary &episode) const {
    const auto progress=m_history.episodes.value(episode.ref.key());
    const bool watched=progress.status==WatchStatus::Watched || watchedByPosition(progress.positionMs,progress.durationMs);
    const auto duration=progress.durationMs.value_or(episode.durationMs.value_or(0));
    int seasonNumber=0;
    for (const auto &season : m_details.seasons) if (episode.seasonId==std::optional<QString>(season.id)) seasonNumber=season.number.value_or(0);
    QString poster;
    if (!episode.artwork.isEmpty()) {
        if (const auto cached = episode.artwork.first().cachedPath) poster=QUrl::fromLocalFile(*cached).toString();
    }
    return {{QStringLiteral("episodeKey"),QString::fromLatin1(episode.ref.key().toHex())},
        {QStringLiteral("title"),episode.title},{QStringLiteral("number"),episode.number.value_or(0)},
        {QStringLiteral("label"),QStringLiteral("S%1E%2").arg(seasonNumber,2,10,QChar('0')).arg(episode.number.value_or(0),2,10,QChar('0'))},
        {QStringLiteral("durationMinutes"),duration/60000},{QStringLiteral("description"),episode.description},
        {QStringLiteral("poster"),poster},{QStringLiteral("watched"),watched},
        {QStringLiteral("selected"),episode.ref==m_selected},{QStringLiteral("available"),episode.availability!=Availability::Unavailable},
        {QStringLiteral("progressFraction"),watched ? 1.0 : duration>0 ? std::clamp(double(progress.positionMs)/double(duration),0.0,1.0) : 0.0},
        {QStringLiteral("resumeSeconds"),!watched && progress.positionMs>=60000 ? progress.positionMs/1000 : 0}};
}
QVariantList VodEpisodesModel::seasons() const {
    QVariantList result;
    for (const auto &season : m_details.seasons) result.append(QVariantMap{{QStringLiteral("id"),season.id},{QStringLiteral("name"),season.number==std::optional<int>(0) ? QStringLiteral("Specials") : QStringLiteral("Season %1").arg(season.number.value_or(season.order+1))}});
    if (result.isEmpty() && !m_details.episodes.isEmpty()) result.append(QVariantMap{{QStringLiteral("id"),QString{}},{QStringLiteral("name"),QStringLiteral("Episodes")}});
    return result;
}
QVariantList VodEpisodesModel::episodes() const {
    QVariantList result;
    for (const auto &episode : orderedEpisodes(m_details,true)) if (episode.seasonId.value_or(QString{})==m_season) result.append(present(episode));
    return result;
}
QVariantMap VodEpisodesModel::selectedEpisode() const {
    for (const auto &episode : m_details.episodes) if (episode.ref==m_selected) return present(episode);
    return {};
}
void VodEpisodesModel::choose() {
    if (busy() || !m_error.isEmpty()) return;
    std::optional<EpisodeSummary> target;
    for (const auto &episode : m_details.episodes) if (episode.ref==m_preferred) target=episode;
    if (!target) target=continueEpisode(m_details,m_history);
    if (!target) { const auto ordinary=orderedEpisodes(m_details); if (!ordinary.isEmpty()) target=ordinary.first(); }
    if (target) { m_selected=target->ref; m_season=target->seasonId.value_or(QString{}); }
    emit changed(); emit selectionChanged(); emit loaded();
}
void VodEpisodesModel::receive(const VodEvent &event) {
    const auto artwork=m_artworkRequests.find(event.operationId);
    if (artwork!=m_artworkRequests.end()) {
        m_artworkRequests.erase(artwork);
        if (const auto *value=std::get_if<PublicValue>(&event.result)) {
            if (const auto *image=std::get_if<ArtworkRef>(value); image && image->cachedPath)
                for (auto &episode : m_details.episodes) for (auto &art : episode.artwork) if (art.id==image->id) art.cachedPath=image->cachedPath;
        }
        emit changed();return;
    }
    const auto found=m_pending.find(event.operationId);
    if (found==m_pending.end()) return;
    const bool details=found.value();m_pending.erase(found);
    if (const auto *error=std::get_if<Error>(&event.result)) m_error=error->message();
    else if (details) m_details=std::get<VodDetails>(std::get<PublicValue>(event.result));
    else { m_history=std::get<SeriesProgress>(std::get<PublicValue>(event.result)); m_historyLoaded=true; }
    if (!m_selected.valid()) choose();
    emit changed();
}
void VodEpisodesModel::selectSeason(const QString &id) {
    if (busy() || id == m_season || std::none_of(m_details.seasons.cbegin(),m_details.seasons.cend(),[&](const Season &season) { return season.id==id; })) return;
    m_season=id;
    const auto rows=episodes();
    if (!rows.isEmpty()) selectEpisode(0); else { m_selected={}; emit selectionChanged(); }
    emit changed();
}
void VodEpisodesModel::selectEpisode(int index) {
    if (busy()) return;
    const auto rows=orderedEpisodes(m_details,true);
    int row=0;
    for (const auto &episode : rows) if (episode.seasonId.value_or(QString{})==m_season && row++==index) {
        if (m_selected==episode.ref) return;
        m_selected=episode.ref; emit changed(); emit selectionChanged(); return;
    }
}
void VodEpisodesModel::playEpisode(int row,bool fromBeginning) {
    selectEpisode(row);
    if (!busy() && m_selected.playable() && selectedEpisode().value(QStringLiteral("available")).toBool()) emit playRequested(m_selected,fromBeginning);
}
void VodEpisodesModel::requestArtwork(int index) {
    if (!m_controller || busy() || m_artworkRequests.size()>=4 || index<0) return;
    int row=0;
    for (const auto &episode : orderedEpisodes(m_details,true)) if (episode.seasonId.value_or(QString{})==m_season && row++==index) {
        if (episode.artwork.isEmpty()) return;
        const auto &art=episode.artwork.first();
        if (art.cachedPath || m_requestedArtwork.contains(art.id)) return;
        m_requestedArtwork.insert(art.id);
        m_artworkRequests.insert(m_controller->artwork(m_series.profileId,art),art.id);return;
    }
}
bool VodEpisodesModel::busy() const {
    return std::any_of(m_pending.cbegin(), m_pending.cend(), [](bool details) { return details; })
        || (!m_selected.valid() && !m_pending.isEmpty());
}
bool VodEpisodesModel::statusBusy() const {
    return m_runtime->ready() && m_runtime->module()->progressService()->statusWritePending(m_series);
}
bool VodEpisodesModel::allWatched() const {
    return seriesWatched(m_details, m_history);
}
void VodEpisodesModel::refreshHistory() {
    if (!m_controller || !m_series.valid()) return;
    for (const auto &id : m_pending.keys()) if (!m_pending.value(id)) { m_controller->cancel(id); m_pending.remove(id); }
    m_pending.insert(m_controller->seriesProgress(m_series), false);
    emit changed();
}
int VodEpisodesModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : static_cast<int>(m_rows.size()); }
QVariant VodEpisodesModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || role != Qt::UserRole || index.row() < 0 || index.row() >= rowCount()) return {};
    return m_rows.at(index.row());
}
QHash<int, QByteArray> VodEpisodesModel::roleNames() const { return {{Qt::UserRole, QByteArrayLiteral("modelData")}}; }
QVariantMap VodEpisodesModel::get(int row) const { return row >= 0 && row < rowCount() ? m_rows.at(row) : QVariantMap{}; }
int VodEpisodesModel::currentIndex() const {
    for (int row = 0; row < rowCount(); ++row) if (m_rows.at(row).value(QStringLiteral("selected")).toBool()) return row;
    return -1;
}
void VodEpisodesModel::updateRows() {
    QList<QVariantMap> next;
    for (const auto &item : episodes()) next.append(item.toMap());
    const auto key = [](const QVariantMap &row) { return row.value(QStringLiteral("episodeKey")).toString(); };
    bool structural = next.size() != m_rows.size();
    if (!structural) for (qsizetype row = 0; row < next.size(); ++row) if (key(next.at(row)) != key(m_rows.at(row))) { structural = true; break; }
    if (structural) emit rowsChanging();
    for (int row = 0; row < static_cast<int>(next.size()); ++row) {
        if (row >= rowCount() || key(m_rows.at(row)) != key(next.at(row))) {
            int existing = row + 1;
            while (existing < rowCount() && key(m_rows.at(existing)) != key(next.at(row))) ++existing;
            if (existing < rowCount()) {
                beginMoveRows({}, existing, existing, {}, row); m_rows.move(existing, row); endMoveRows();
            } else { beginInsertRows({}, row, row); m_rows.insert(row, next.at(row)); endInsertRows(); }
        }
        if (m_rows.at(row) != next.at(row)) {
            m_rows[row] = next.at(row); emit dataChanged(index(row), index(row), {Qt::UserRole});
        }
    }
    if (rowCount() > static_cast<int>(next.size())) {
        beginRemoveRows({}, static_cast<int>(next.size()), rowCount() - 1);
        m_rows.erase(m_rows.begin() + next.size(), m_rows.end()); endRemoveRows();
    }
    if (structural) emit rowsChanged();
}
void VodEpisodesModel::toggleEpisodeWatched(const QString &episodeKey) {
    if (!statusReady() || statusBusy()) return;
    for (const auto &episode : m_details.episodes) {
        if (QString::fromLatin1(episode.ref.key().toHex()) != episodeKey) continue;
        const auto ref = episode.ref;
        const auto generation = m_generation;
        const QPointer<VodEpisodesModel> self(this);
        m_error.clear();
        m_runtime->module()->progressService()->setWatched(ref, !present(episode).value(QStringLiteral("watched")).toBool(), [self, ref, generation](Result<VodProgress> result) {
            if (!self || generation != self->m_generation || parentSeries(ref) != self->m_series) return;
            if (const auto *error = std::get_if<Error>(&result)) self->m_error = error->message();
            else self->m_history.episodes.insert(ref.key(), std::get<VodProgress>(result));
            emit self->changed();
        });
        return;
    }
}
void VodEpisodesModel::toggleWatched() {
    if (!statusReady() || statusBusy()) return;
    const auto generation = m_generation;
    const QPointer<VodEpisodesModel> self(this);
    m_error.clear();
    m_runtime->module()->progressService()->setSeriesWatched(m_details, !allWatched(), [self, generation](Outcome result) {
        if (!self || generation != self->m_generation) return;
        if (const auto *error = std::get_if<Error>(&result)) self->m_error = error->message();
        emit self->changed();
    });
}
}
