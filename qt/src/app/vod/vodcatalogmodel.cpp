#include "vodcatalogmodel.h"
#include "vodepisodesmodel.h"
#include "core/settingsmanager.h"
#include "core/sourcestore.h"
#include "core/trackpreferences.h"
#include <algorithm>

namespace OKILTV::Vod {
namespace {
double progressFraction(const VodProgress &progress)
{
    if (!progress.durationMs || *progress.durationMs <= 0 || progress.positionMs <= 0) return 0;
    return std::clamp(static_cast<double>(progress.positionMs) / static_cast<double>(*progress.durationMs), 0.0, 1.0);
}
QVariantMap trackVariant(const VodMediaTrack &track)
{
    return {{QStringLiteral("type"), track.type}, {QStringLiteral("id"), track.ordinal + 1},
        {QStringLiteral("title"), track.title}, {QStringLiteral("lang"), track.language},
        {QStringLiteral("codec"), track.codec}};
}
QString trackLabel(const VodMediaTrack &track)
{
    QStringList parts;
    parts.append(QStringLiteral("Track %1").arg(track.ordinal + 1));
    if (!track.language.isEmpty()) parts.append(track.language.toUpper());
    if (!track.title.isEmpty()) parts.append(track.title);
    if (!track.codec.isEmpty()) parts.append(track.codec.toUpper());
    if (track.forced) parts.append(QStringLiteral("Forced"));
    else if (track.isDefault) parts.append(QStringLiteral("Default"));
    return parts.join(QStringLiteral(" · "));
}
QJsonObject probedPreference(const VodMediaTrack &track)
{
    return {{QStringLiteral("mode"), QStringLiteral("track")},
        {QStringLiteral("ordinal"), track.ordinal},
        {QStringLiteral("title"), Core::normalizedTrackLabel(track.title)},
        {QStringLiteral("lang"), Core::normalizedTrackLabel(track.language)},
        {QStringLiteral("codec"), Core::normalizedTrackLabel(track.codec)}};
}
int selectedProbeTrack(const QList<VodMediaTrack> &tracks, const QString &type, const QJsonObject &preference)
{
    QVariantList variants;
    for (const auto &track : tracks) variants.append(trackVariant(track));
    const auto id = Core::matchTrackPreference(variants, type, preference);
    return id > 0 ? id - 1 : -1;
}
}
VodCatalogModel::VodCatalogModel(VodRuntime *runtime, Core::SettingsManager *settings, QObject *parent, Purpose purpose, CatalogKind kind)
    : QAbstractListModel(parent), m_kind(kind), m_runtime(runtime), m_settings(settings), m_purpose(purpose)
{
    m_episodes = std::make_unique<VodEpisodesModel>(runtime);
    connect(m_episodes.get(), &VodEpisodesModel::selectionChanged, this, [this]() {
        if (!series() || !m_selected.valid()) return;
        cancelKind(Operation::EpisodeDetails); cancelKind(Operation::SeasonMetadata); cancelKind(Operation::Probe); cancelKind(Operation::Progress);
        const auto season = m_episodes->seasonId();
        if (season != m_trackMetadataSeason) m_seasonTrackMetadata.reset();
        m_trackMetadataSeason = season;
        m_mediaProbe.reset(); m_playbackTrackPreferences={}; m_trackOptionsEdited=false;
        m_movie.insert(QStringLiteral("progressLoaded"),false);
        m_movie.insert(QStringLiteral("mediaProbeReady"),false);
        m_movie.insert(QStringLiteral("mediaProbeLoading"),false);
        m_movie.remove(QStringLiteral("mediaProbeError"));
        updateMediaPresentation();
        const auto ref=m_episodes->selectedRef();
        if (ref.playable()) {
            track(m_controller->progress(ref),Operation::Progress,ref);
            track(m_controller->details(ref),Operation::EpisodeDetails,ref);
            track(m_controller->cachedSeasonMetadata(m_selected, season), Operation::SeasonMetadata, ref);
        }
        emit changed();
    });
    connect(m_episodes.get(), &VodEpisodesModel::playRequested, this, [this](const ContentRef &,bool fromBeginning) { play(fromBeginning); });
    connect(m_episodes.get(), &VodEpisodesModel::changed, this, [this]() {
        if (series()) m_movie.insert(QStringLiteral("watched"), m_episodes->allWatched());
        emit changed();
    });
    connect(m_runtime, &VodRuntime::subtitlesChanged, this, [this](const ContentRef &ref) {
        if (ref != (series() ? m_episodes->selectedRef() : m_selected)) return;
        updateSubtitlePresentation(); emit changed();
    });
    m_probePlayDelay.setSingleShot(true);
    m_probePlayDelay.setInterval(5000);
    m_probePlayDelay.setTimerType(Qt::PreciseTimer);
    connect(&m_probePlayDelay, &QTimer::timeout, this, &VodCatalogModel::changed);
    m_storageWaitTimeout.setSingleShot(true);
    m_storageWaitTimeout.setInterval(15000);
    m_storageWaitTimeout.setTimerType(Qt::PreciseTimer);
    m_storageRetry.setInterval(250);
    connect(&m_storageRetry, &QTimer::timeout, this, [this]() { m_runtime->retryInitialization(); });
    connect(&m_storageWaitTimeout, &QTimer::timeout, this, [this]() {
        m_storageRetry.stop();
        m_error = QStringLiteral("VOD storage unavailable after 15 seconds. Try again.");
        emit changed();
    });
    connect(runtime, &VodRuntime::stateChanged, this, [this]() {
        if (m_runtime->active() && pending(Operation::Probe)) {
            cancelKind(Operation::Probe);
            m_movie.insert(QStringLiteral("mediaProbeLoading"), false);
        }
        if (m_purpose == Purpose::PlaybackSidebar) syncPlayback();
        if (!m_runtime->module()->session()) return;
        const auto &snapshot = m_runtime->module()->session()->snapshot();
        if (!snapshot.ref.playable() || !snapshot.positionValid) return;
        applyProgress(snapshot.ref, m_runtime->module()->progressService()->observedProgress(snapshot));
    });
    m_searchTimer.setSingleShot(true); m_searchTimer.setInterval(250);
    connect(&m_searchTimer, &QTimer::timeout, this, [this]() { query(); });
    connect(runtime, &VodRuntime::errorOccurred, this, [this](const QString &message) {
        if (m_open && !m_runtime->ready() && !m_storageWaitTimeout.isActive() && m_error.isEmpty()) { m_error = message; emit changed(); }
    });
    connect(runtime, &VodRuntime::playbackMetadataChanged, this, [this](const ContentRef &ref) {
        if (ref.profileId != m_profile) return;
        const auto metadata = m_runtime->playbackMetadata(ref);
        if (!metadata) return;
        applyResolution(ref, metadata->videoWidth, metadata->videoHeight);
        if (ref == m_selected || (series() && ref==m_episodes->selectedRef())) {
            cancelKind(Operation::Probe);
            m_mediaProbe = metadata;
            updateMediaPresentation();
        }
        if (ref == m_playingRef) {
            m_playingMovie.insert(QStringLiteral("durationMinutes"), m_runtime->module()->session()->snapshot().durationMs.value_or(0) / 60000);
        }
        emit changed();
    });
    connect(runtime, &VodRuntime::sourcesReconciled, this, [this]() { stopWaitingForStorage(); if (m_open) { attach(); loadScope(); if (m_purpose == Purpose::PlaybackSidebar) { m_playingRef = {}; syncPlayback(); } } });
    connect(runtime, &VodRuntime::sourceInvalidated, this, [this](const QUuid &id) {
        if (id != m_profile) return;
        cancelAll(); m_resolutions.clear(); m_requestedResolutions.clear(); resetRows(); back(); m_scope = {}; m_categories.clear(); m_continueRows.clear(); m_continueMoviesLoaded = false; emit changed();
    });
    connect(runtime, &VodRuntime::sourceSyncChanged, this, [this](const QUuid &id) { if (id == m_profile) emit changed(); });
    connect(runtime, &VodRuntime::sourceSyncFinished, this, [this](const QUuid &id) {
        // Startup has its own retry window; a failed queued refresh must not
        // publish a transient storage error or start queries before readiness.
        if (id != m_profile || !m_open || !m_controller || !m_runtime->ready()) return;
        m_error.clear();
        const auto saved = m_settings->profileById(id);
        if (!saved || !saved->vodEnabled) { emit changed(); return; }
        if (!m_scope.catalogNamespace.isNull()) {
            track(m_controller->categories(m_scope), Operation::Categories);
            query(); queryContinue();
        } else loadScope();
        if (m_refreshSeriesDetails.valid()) {
            const auto ref = m_refreshSeriesDetails; m_refreshSeriesDetails = {};
            if (ref == m_selected) {
                track(m_controller->details(ref, true), Operation::Details, ref);
                m_episodes->load(ref, m_episodes->selectedRef(), true);
            }
        }
        emit changed();
    });
    connect(runtime, &VodRuntime::sourceUpdated, this, [this](const QUuid &id) {
        reloadSources();
        if (m_open && id == m_profile) {
            loadScope();
            if (m_purpose == Purpose::PlaybackSidebar) { m_playingRef = {}; syncPlayback(); }
        }
    });
}
int VodCatalogModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : count(); }
QVariant VodCatalogModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) return {};
    const auto &item = m_rows[index.row()];
    switch (role) {
    case KeyRole: return QString::fromLatin1(item.ref.key().toHex());
    case TitleRole: return item.title;
    case YearRole: return series() && !m_episodeLabels.value(item.ref.key()).isEmpty() ? m_episodeLabels.value(item.ref.key()) : item.year ? QString::number(*item.year) : QString{};
    case PosterRole: return item.artwork.isEmpty() ? QString{} : m_posters.value(item.artwork.first().id);
    case ResolutionRole: return m_resolutions.value(item.ref.key());
    case ProgressRole: return m_progress.value(item.ref.key(), 0.0);
    case ToWatchRole: return m_movieLists.value(item.ref.key()).toWatch;
    case FavouriteRole: return m_movieLists.value(item.ref.key()).favourite;
    case ListsBusyRole: return listsBusy(item.ref);
    case AvailableRole: return item.availability != Availability::Unavailable;
    default: return {};
    }
}
QHash<int, QByteArray> VodCatalogModel::roleNames() const
{ return {{KeyRole, "movieKey"}, {TitleRole, "title"}, {YearRole, "year"}, {PosterRole, "poster"}, {AvailableRole, "available"}, {ProgressRole, "progressFraction"}, {ResolutionRole, "resolutionLabel"}, {ToWatchRole, "toWatch"}, {FavouriteRole, "favourite"}, {ListsBusyRole, "listsBusy"}}; }
bool VodCatalogModel::pending(Operation kind) const
{ return std::any_of(m_pending.cbegin(), m_pending.cend(), [kind](const Pending &p) { return p.kind == kind; }); }
bool VodCatalogModel::busy() const
{ return (m_open && m_runtime->syncing(m_profile)) || m_marking || m_storageWaitTimeout.isActive() || std::any_of(m_pending.cbegin(), m_pending.cend(), [](const Pending &p) { return p.kind != Operation::MovieLists && p.kind != Operation::Probe && p.kind != Operation::Artwork && p.kind != Operation::Progress && p.kind != Operation::CardProgress && p.kind != Operation::CardResolution && p.kind != Operation::ContinueQuery && p.kind != Operation::PlayingDetails && p.kind != Operation::PlayingArtwork && p.kind != Operation::EpisodeDetails && p.kind != Operation::SeasonMetadata; }); }
bool VodCatalogModel::startingPlayback() const { return pending(Operation::Play); }
QString VodCatalogModel::errorText() const
{
    if (!m_error.isEmpty() || m_storageWaitTimeout.isActive()) return m_error;
    return m_runtime->syncError(m_profile, m_kind);
}
void VodCatalogModel::track(const QUuid &id, Operation kind, const ContentRef &ref, bool append)
{
    m_pending.insert(id, {kind, m_queryGeneration, ref, append, m_listsRevision});
    if (kind == Operation::Probe) m_probePlayDelay.start();
    emit changed();
}
void VodCatalogModel::attach()
{
    auto *controller = m_runtime->module()->controller();
    if (!controller || controller == m_controller) return;
    m_controller = controller;
    connect(controller, &VodController::eventCompleted, this, &VodCatalogModel::receive);
    auto *progressService = m_runtime->module()->progressService();
    connect(progressService, &VodProgressService::movieListWritePendingChanged, this, [this](const ContentRef &ref) {
        if (ref.profileId != m_profile) return;
        if (ref == m_selected) m_movie.insert(QStringLiteral("listsBusy"), listsBusy(ref));
        for (int row = 0; row < count(); ++row)
            if (m_rows[row].ref == ref) emit dataChanged(index(row), index(row), {ListsBusyRole});
        emit changed();
    });
    connect(progressService, &VodProgressService::movieListsChanged, this, [this](const ContentRef &ref) {
        if (ref.profileId != m_profile || ref.catalogNamespace != m_scope.catalogNamespace) return;
        ++m_listsRevision;
        track(m_controller->movieLists(ref), Operation::MovieLists, ref);
        if (m_open && (m_category == QStringLiteral("__to_watch__") || m_category == QStringLiteral("__favourites__"))) query();
    });
    connect(progressService, &VodProgressService::persisted, this, [this](const ContentRef &ref) {
        if (!m_open || ref.profileId != m_profile) return;
        queryContinue();
        const bool membershipChanged = series() || m_continueChanged.remove(ref.key());
        if (membershipChanged && m_category == QStringLiteral("__continue_watching__")) query();
    });
}
void VodCatalogModel::reloadSources()
{
    m_sources.clear();
    for (const auto &source : m_settings->sourceSummaries()) {
        const auto detail = m_settings->profileById(source.id);
        if (source.type == Core::ProfileType::Xtream && detail && detail->vodEnabled)
            m_sources.append(QVariantMap{{QStringLiteral("id"), source.id.toString(QUuid::WithoutBraces)}, {QStringLiteral("name"), source.name}});
    }
    const auto exists = std::any_of(m_sources.cbegin(), m_sources.cend(), [this](const QVariant &v) { return QUuid(v.toMap().value(QStringLiteral("id")).toString()) == m_profile; });
    if (!exists && !m_profile.isNull()) {
        stopWaitingForStorage();
        cancelAll(); m_profile = QUuid{}; m_scope = {}; m_categories.clear(); m_continueRows.clear(); m_continueMoviesLoaded = false; resetRows(); back();
    }
    emit changed();
}
void VodCatalogModel::open()
{
    m_open = true; m_requestedProgress.clear(); reloadSources();
    if (m_profile.isNull() && !m_sources.isEmpty()) {
        auto chosen = m_sources.first().toMap().value(QStringLiteral("id")).toString();
        const auto active = m_settings->current().activeProfileId;
        for (const auto &value : m_sources) {
            const auto id = value.toMap().value(QStringLiteral("id")).toString();
            if (active && QUuid(id) == *active) chosen = id;
        }
        selectSource(chosen);
    } else if (!m_profile.isNull()) {
        m_runtime->ensureForSource(m_profile); attach();
        if (!m_runtime->ready()) waitForStorage();
        if (m_runtime->ready() && m_scope.catalogNamespace.isNull()) loadScope();
        else if (m_runtime->ready() && (m_rows.isEmpty() || m_category == QStringLiteral("__continue_watching__") || m_category == QStringLiteral("__to_watch__") || m_category == QStringLiteral("__favourites__"))) query();
        if (m_runtime->ready() && m_selected.valid()) {
            const auto playable = series() ? m_episodes->selectedRef() : m_selected;
            if (playable.playable()) {
                track(m_controller->progress(playable), Operation::Progress, playable);
                track(m_controller->details(playable), series() ? Operation::EpisodeDetails : Operation::Details, playable);
            }
            track(m_controller->movieLists(m_selected), Operation::MovieLists, m_selected);
        }
    }
    queryContinue();
    emit changed();
}
void VodCatalogModel::close()
{
    stopWaitingForStorage();
    m_open = false; m_searchTimer.stop(); cancelAll(); m_requestedResolutions.clear();
    // Cancelled images may be requested again when the grid is reopened.
    m_requestedPosters.clear(); emit changed();
}
void VodCatalogModel::cancelAll()
{
    m_probePlayDelay.stop();
    const auto operations = m_pending.keys(); m_pending.clear();
    if (m_controller) for (const auto &id : operations) m_controller->cancel(id);
}
void VodCatalogModel::cancelKind(Operation kind)
{
    if (kind == Operation::Probe) m_probePlayDelay.stop();
    const auto operations = m_pending.keys();
    for (const auto &id : operations) if (m_pending.value(id).kind == kind) {
        if (m_pending.value(id).kind == Operation::CardResolution) m_requestedResolutions.remove(m_pending.value(id).ref.key());
        m_pending.remove(id); if (m_controller) m_controller->cancel(id);
    }
}
void VodCatalogModel::resetRows()
{ beginResetModel(); m_rows.clear(); m_next.reset(); m_catalogLoaded = false; endResetModel(); }
void VodCatalogModel::selectSource(const QString &id)
{
    const QUuid profile(id);
    if (profile.isNull() || std::none_of(m_sources.cbegin(), m_sources.cend(), [&id](const QVariant &v) { return v.toMap().value(QStringLiteral("id")).toString() == id; })) return;
    if (profile == m_profile && !m_scope.catalogNamespace.isNull()) return;
    stopWaitingForStorage();
    cancelAll(); m_searchTimer.stop(); resetRows(); back(); m_categories.clear(); m_continueRows.clear();
    m_continueMoviesLoaded = false;
    m_profile = profile; m_scope = {}; m_category.clear(); m_search.clear(); m_error.clear();
    m_resolutions.clear(); m_requestedResolutions.clear();
    m_movieLists.clear(); m_episodeLabels.clear(); ++m_listsRevision; m_posters.clear(); m_requestedPosters.clear(); m_progress.clear(); m_requestedProgress.clear(); m_continueEligibility.clear(); m_continueChanged.clear(); ++m_queryGeneration;
    m_runtime->ensureForSource(profile); attach();
    if (m_runtime->ready()) loadScope();
    else waitForStorage();
    emit changed();
}
void VodCatalogModel::loadScope()
{
    if (!m_controller || !m_runtime->ready() || m_profile.isNull()) return;
    cancelAll(); m_error.clear(); track(m_controller->scope(m_profile), Operation::Scope);
}
void VodCatalogModel::waitForStorage()
{
    if (!m_open || m_profile.isNull() || m_runtime->ready() || m_storageWaitTimeout.isActive()) return;
    m_error.clear();
    m_storageWaitTimeout.start();
    m_storageRetry.start();
    m_runtime->retryInitialization();
}
void VodCatalogModel::stopWaitingForStorage()
{
    m_storageWaitTimeout.stop();
    m_storageRetry.stop();
}
void VodCatalogModel::selectCategory(const QString &id)
{ if (m_category == id) return; m_category = id; m_searchTimer.stop(); query(); emit changed(); }
void VodCatalogModel::setSearchText(const QString &text)
{
    if (m_search == text) return;
    m_search = text; ++m_queryGeneration; cancelKind(Operation::Query); m_next.reset();
    if (m_open) m_searchTimer.start(); emit changed();
}
void VodCatalogModel::setDescending(bool value)
{ if (m_descending == value) return; m_descending = value; m_searchTimer.stop(); query(); emit changed(); }
void VodCatalogModel::query(bool append)
{
    if (!m_open || !m_controller || m_scope.catalogNamespace.isNull()) return;
    if (append && (!m_next || pending(Operation::Query))) return;
    cancelKind(Operation::Query);
    if (!append) {
        ++m_queryGeneration;
        cancelKind(Operation::Artwork);
        cancelKind(Operation::CardProgress); cancelKind(Operation::CardResolution);
        m_requestedProgress.clear();
        m_requestedPosters.clear();
    }
    CatalogQuery query; query.scope = m_scope;
    query.continueWatchingOnly = m_category == QStringLiteral("__continue_watching__");
    if (m_category == QStringLiteral("__to_watch__")) query.movieList = MovieList::ToWatch;
    else if (m_category == QStringLiteral("__favourites__")) query.movieList = MovieList::Favourites;
    else if (!m_category.isEmpty() && !query.continueWatchingOnly) query.scope.categoryId = m_category;
    query.titleContains = m_search.trimmed(); query.sort = m_descending ? CatalogSort::TitleDescending : CatalogSort::TitleAscending;
    query.pageSize = 100; if (append) query.page = m_next;
    track(m_controller->query(query), Operation::Query, {}, append);
}
void VodCatalogModel::refresh()
{
    if (!m_controller || !m_open || m_runtime->syncing(m_profile)) return;
    if (!m_runtime->ready()) { waitForStorage(); emit changed(); return; }
    m_error.clear();
    if (series() && m_selected.valid()) {
        cancelKind(Operation::Details); cancelKind(Operation::EpisodeDetails);
        m_refreshSeriesDetails = m_selected;
    }
    m_runtime->synchronizeSource(m_profile.toString(QUuid::WithoutBraces));
}
void VodCatalogModel::fetchMoreMovies() { query(true); }
void VodCatalogModel::selectMovie(int row)
{
    const auto *selected = itemAt(row);
    if (!m_controller || !selected || startingPlayback()) return;
    back(); cancelKind(Operation::CardProgress); cancelKind(Operation::CardResolution); m_requestedProgress.clear(); cancelKind(Operation::Artwork); m_requestedPosters.clear();
    const auto &item = *selected; m_selected = item.ref; m_mediaProbe.reset(); m_playbackTrackPreferences = {};
    m_movie = {{QStringLiteral("title"), item.title}, {QStringLiteral("year"), item.year ? QString::number(*item.year) : QString{}},
        {QStringLiteral("poster"), item.artwork.isEmpty() ? QString{} : m_posters.value(item.artwork.first().id)}, {QStringLiteral("available"), item.availability != Availability::Unavailable},
        {QStringLiteral("resumeSeconds"), 0}, {QStringLiteral("movieKey"), QString::fromLatin1(item.ref.key().toHex())},
        {QStringLiteral("toWatch"), m_movieLists.value(item.ref.key()).toWatch},
        {QStringLiteral("favourite"), m_movieLists.value(item.ref.key()).favourite},
        {QStringLiteral("listsLoaded"), m_movieLists.contains(item.ref.key())}, {QStringLiteral("listsBusy"), listsBusy(item.ref)}};
    updateSubtitlePresentation();
    requestPoster(row);
    track(m_controller->details(item.ref), Operation::Details, item.ref);
    if (series()) m_episodes->load(item.ref);
    else track(m_controller->progress(item.ref), Operation::Progress, item.ref);
    track(m_controller->movieLists(item.ref), Operation::MovieLists, item.ref);
}
void VodCatalogModel::back()
{
    cancelKind(Operation::Details); cancelKind(Operation::EpisodeDetails); cancelKind(Operation::SeasonMetadata); cancelKind(Operation::Probe); cancelKind(Operation::Progress); cancelKind(Operation::Play);
    m_seasonTrackMetadata.reset(); m_trackMetadataSeason.clear();
    m_episodes->clear();
    m_selected = {}; m_refreshSeriesDetails = {}; m_trackOptionsEdited = false; m_mediaProbe.reset(); m_playbackTrackPreferences = {}; m_movie.clear(); emit changed();
}
void VodCatalogModel::play(bool fromBeginning)
{
    const auto playable = series() ? m_episodes->selectedRef() : m_selected;
    if (!m_controller || !playable.playable() || m_runtime->subtitleBusy(playable) || startingPlayback() || probePlayBlocked() || !m_movie.value(QStringLiteral("available")).toBool()
        || (series() && !m_episodes->selectedEpisode().value(QStringLiteral("available")).toBool())) return;
    cancelKind(Operation::CardProgress); cancelKind(Operation::CardResolution); m_requestedProgress.clear();
    // Playback takes priority over thumbnail work.
    cancelKind(Operation::Artwork); m_requestedPosters.clear();
    m_error.clear(); m_startTitle = m_movie.value(QStringLiteral("title")).toString();
    m_startYear = m_movie.value(QStringLiteral("year")).toString();
    PlaybackPreferences preferences; preferences.fromBeginning = fromBeginning;
    if (m_movie.value(QStringLiteral("progressLoaded")).toBool() && (!series() || !m_playbackTrackPreferences.isEmpty() || m_trackOptionsEdited)) preferences.trackPreferences = m_playbackTrackPreferences;
    if (series()) {
        const auto &snapshot=m_runtime->module()->session()->snapshot();
        if (parentSeries(snapshot.ref)==m_selected) preferences.inheritedTracks=snapshot.trackPreferences;
    }
    track(m_controller->play(playable, preferences), Operation::Play, playable);
}
void VodCatalogModel::playRow(int row)
{
    const auto *item = itemAt(row);
    if (m_purpose != Purpose::PlaybackSidebar || !m_controller || !item || startingPlayback()
        || item->availability == Availability::Unavailable) return;
    if (m_runtime->active() && item->ref == (series() ? parentSeries(m_runtime->module()->session()->snapshot().ref) : m_runtime->module()->session()->snapshot().ref)) {
        emit playbackStarted();
        return;
    }
    m_startTitle = item->title;
    m_startYear = item->year ? QString::number(*item->year) : QString{};
    m_error.clear();
    cancelKind(Operation::Artwork);
    m_requestedPosters.clear();
    track(m_controller->play(item->ref, PlaybackPreferences{}), Operation::Play, item->ref);
}
void VodCatalogModel::updatePlayingSummary()
{
    if (!m_playingRef.valid()) return;
    const auto title = series() ? QString{} : m_runtime->title();
    if (!title.isEmpty()) m_playingMovie.insert(QStringLiteral("title"), title);
    for (const auto &item : m_rows) {
        if (item.ref != m_playingRef) continue;
        if (title.isEmpty()) m_playingMovie.insert(QStringLiteral("title"), item.title);
        m_playingMovie.insert(QStringLiteral("year"), item.year ? QString::number(*item.year) : QString{});
        if (m_playingMovie.value(QStringLiteral("poster")).toString().isEmpty() && !item.artwork.isEmpty()) {
            const auto &art = item.artwork.first();
            const auto poster = m_posters.value(art.id);
            if (!poster.isEmpty()) m_playingMovie.insert(QStringLiteral("poster"), poster);
            else if (!pending(Operation::PlayingArtwork))
                track(m_controller->artwork(item.ref.profileId, art), Operation::PlayingArtwork, item.ref);
        }
        break;
    }
}
void VodCatalogModel::syncPlayback()
{
    if (!m_runtime->active()) {
        if (m_playingRef.valid()) {
            cancelKind(Operation::PlayingDetails); cancelKind(Operation::PlayingArtwork);
            m_playingRef = {}; m_playingMovie.clear(); emit changed();
        }
        return;
    }
    const auto active = m_runtime->module()->session()->snapshot().ref;
    if (!active.playable()) return;
    const auto kind = active.kind == ContentKind::Episode ? CatalogKind::Series : CatalogKind::Movies;
    if (m_kind != kind) {
        m_kind=kind; m_profile=QUuid{}; m_scope={}; m_category.clear(); m_search.clear(); cancelAll(); resetRows(); back();
        m_continueRows.clear(); m_categories.clear(); m_playingRef={};
    }
    const auto ref = series() ? parentSeries(active) : active;
    if (!m_open || m_profile != ref.profileId) {
        m_open = true;
        reloadSources();
        selectSource(ref.profileId.toString(QUuid::WithoutBraces));
    }
    if (!m_controller) return;
    if (ref != m_playingRef) {
        cancelKind(Operation::PlayingDetails); cancelKind(Operation::PlayingArtwork);
        m_playingRef = ref;
        m_playingMovie = {{QStringLiteral("title"), m_runtime->title()}, {QStringLiteral("year"), m_runtime->playbackYear()}};
        track(m_controller->details(ref), Operation::PlayingDetails, ref);
        updatePlayingSummary();
        emit changed();
    } else if (!series() && !m_runtime->title().isEmpty() && (m_playingMovie.value(QStringLiteral("title")).toString() != m_runtime->title()
        || m_playingMovie.value(QStringLiteral("year")).toString() != m_runtime->playbackYear())) {
        m_playingMovie.insert(QStringLiteral("title"), m_runtime->title());
        m_playingMovie.insert(QStringLiteral("year"), m_runtime->playbackYear()); emit changed();
    }
}
void VodCatalogModel::selectAudioOption(int index)
{
    if (!m_mediaProbe || !m_movie.value(QStringLiteral("trackOptionsEditable")).toBool()
        || startingPlayback() || !m_movie.value(QStringLiteral("progressLoaded")).toBool()) return;
    if (index <= 0) m_playbackTrackPreferences.remove(QStringLiteral("audio"));
    else if (index <= m_mediaProbe->audioTracks.size())
        m_playbackTrackPreferences.insert(QStringLiteral("audio"), probedPreference(m_mediaProbe->audioTracks.at(index - 1)));
    else return;
    m_trackOptionsEdited = true;
    updateMediaPresentation(); emit changed();
}
void VodCatalogModel::selectSubtitleOption(int index)
{
    const auto ref = series() ? m_episodes->selectedRef() : m_selected;
    const auto rows = m_movie.value(QStringLiteral("subtitleTrackOptions")).toList();
    if (!ref.playable() || startingPlayback() || index < 0 || index >= rows.size()) return;
    const auto row = rows[index].toMap();
    if (!row.value(QStringLiteral("optionEnabled"), true).toBool()) return;
    if (row.value(QStringLiteral("action")).toString() == QLatin1String("upload")) {
        m_runtime->beginSubtitleUpload(ref); emit changed(); return;
    }
    const auto preference = QJsonObject::fromVariantMap(row.value(QStringLiteral("preference")).toMap());
    if (preference.isEmpty()) m_playbackTrackPreferences.remove(QStringLiteral("sub"));
    else m_playbackTrackPreferences.insert(QStringLiteral("sub"), preference);
    m_trackOptionsEdited = true;
    m_runtime->selectSubtitlePreference(ref, preference);
}
void VodCatalogModel::removeSubtitleOption(int index)
{
    const auto rows = m_movie.value(QStringLiteral("subtitleTrackOptions")).toList();
    if (index < 0 || index >= rows.size()) return;
    m_runtime->removeUploadedSubtitle(series() ? m_episodes->selectedRef() : m_selected,
        rows[index].toMap().value(QStringLiteral("externalId")).toString());
}
void VodCatalogModel::updateSubtitlePresentation()
{
    if (m_movie.isEmpty()) return;
    const auto ref = series() ? m_episodes->selectedRef() : m_selected;
    if (!ref.playable()) return;
    m_runtime->requestSubtitleState(ref);
    const auto state = m_runtime->subtitleState(ref);
    const auto selected = state.contains(QStringLiteral("selection")) ? state.value(QStringLiteral("selection")).toObject()
        : m_playbackTrackPreferences.value(QStringLiteral("sub")).toObject();
    QVariantList rows;
    rows.append(QVariantMap{{QStringLiteral("label"), QStringLiteral("Default")}, {QStringLiteral("preference"), QVariantMap{}}});
    rows.append(QVariantMap{{QStringLiteral("label"), QStringLiteral("Off")}, {QStringLiteral("preference"), QVariantMap{{QStringLiteral("mode"), QStringLiteral("off")}}}});
    int selectedIndex = selected.value(QStringLiteral("mode")).toString() == QLatin1String("off") ? 1 : 0;
    const auto metadata = m_mediaProbe ? m_mediaProbe : m_seasonTrackMetadata;
    if (metadata) {
        const auto match = selectedProbeTrack(metadata->subtitleTracks, QStringLiteral("sub"), selected);
        for (qsizetype i = 0; i < metadata->subtitleTracks.size(); ++i) {
            const auto &track = metadata->subtitleTracks[i];
            if (m_mediaProbe && i == match) selectedIndex = static_cast<int>(rows.size());
            rows.append(QVariantMap{{QStringLiteral("label"), trackLabel(track)}, {QStringLiteral("optionEnabled"), m_mediaProbe.has_value()},
                {QStringLiteral("preference"), probedPreference(track).toVariantMap()}});
        }
    }
    QHash<QString, int> names;
    for (const auto &value : state.value(QStringLiteral("files")).toArray()) {
        const auto file = value.toObject();
        const auto id = file.value(QStringLiteral("id")).toString();
        auto name = file.value(QStringLiteral("name")).toString();
        const auto count = ++names[name];
        if (count > 1) name += QStringLiteral(" (%1)").arg(count);
        if (selected.value(QStringLiteral("externalId")).toString() == id) selectedIndex = static_cast<int>(rows.size());
        const QJsonObject preference{{QStringLiteral("mode"), QStringLiteral("external")}, {QStringLiteral("externalId"), id}, {QStringLiteral("ordinal"), 0}};
        rows.append(QVariantMap{{QStringLiteral("label"), name}, {QStringLiteral("externalId"), id}, {QStringLiteral("preference"), preference.toVariantMap()}});
    }
    rows.append(QVariantMap{{QStringLiteral("label"), QStringLiteral("Upload subtitles...")}, {QStringLiteral("action"), QStringLiteral("upload")}});
    m_movie.insert(QStringLiteral("subtitleTrackOptions"), rows);
    m_movie.insert(QStringLiteral("subtitleTrackIndex"), selectedIndex);
    m_movie.insert(QStringLiteral("subtitleOptionsEnabled"), !m_runtime->subtitleBusy(ref));
    m_movie.insert(QStringLiteral("subtitleBusy"), m_runtime->subtitleBusy(ref));
    m_movie.insert(QStringLiteral("subtitleError"), m_runtime->subtitleError());
}
void VodCatalogModel::updateMediaPresentation()
{
    if (series() && m_mediaProbe && (!m_seasonTrackMetadata || m_mediaProbe->observedAtUtc > m_seasonTrackMetadata->observedAtUtc))
        m_seasonTrackMetadata = m_mediaProbe;
    const auto metadata = m_mediaProbe ? m_mediaProbe : m_seasonTrackMetadata;
    m_movie.insert(QStringLiteral("mediaProbeReady"), m_mediaProbe.has_value());
    m_movie.insert(QStringLiteral("trackOptionsEditable"), m_mediaProbe.has_value());
    m_movie.insert(QStringLiteral("mediaProbeLoading"), pending(Operation::Probe));
    if (!metadata) {
        m_movie.insert(QStringLiteral("audioTrackOptions"), QVariantList{});
        m_movie.insert(QStringLiteral("subtitleTrackOptions"), QVariantList{});
        m_movie.insert(QStringLiteral("audioTrackIndex"), 0);
        m_movie.insert(QStringLiteral("subtitleTrackIndex"), 0);
        m_movie.insert(QStringLiteral("audioTrackCount"), 0);
        m_movie.insert(QStringLiteral("subtitleTrackCount"), 0);
        updateSubtitlePresentation();
        return;
    }
    if (!series()) applyResolution(m_selected, metadata->videoWidth, metadata->videoHeight);
    QVariantList audio{{QVariantMap{{QStringLiteral("label"), QStringLiteral("Default")}}}};
    for (const auto &track : metadata->audioTracks)
        audio.append(QVariantMap{{QStringLiteral("label"), trackLabel(track)}});
    QVariantList subtitles{{QVariantMap{{QStringLiteral("label"), QStringLiteral("Default")}}},
        {QVariantMap{{QStringLiteral("label"), QStringLiteral("Off")}}}};
    for (const auto &track : metadata->subtitleTracks)
        subtitles.append(QVariantMap{{QStringLiteral("label"), trackLabel(track)}});
    int audioIndex = 0;
    const auto audioPreference = m_playbackTrackPreferences.value(QStringLiteral("audio")).toObject();
    if (!audioPreference.isEmpty()) {
        const auto selected = selectedProbeTrack(metadata->audioTracks, QStringLiteral("audio"), audioPreference);
        if (selected >= 0) audioIndex = selected + 1;
    }
    int subtitleIndex = 0;
    const auto subtitlePreference = m_playbackTrackPreferences.value(QStringLiteral("sub")).toObject();
    if (subtitlePreference.value(QStringLiteral("mode")).toString() == QLatin1String("off")) subtitleIndex = 1;
    else if (!subtitlePreference.isEmpty()) {
        const auto selected = selectedProbeTrack(metadata->subtitleTracks, QStringLiteral("sub"), subtitlePreference);
        if (selected >= 0) subtitleIndex = selected + 2;
    }
    m_movie.insert(QStringLiteral("audioTrackOptions"), audio);
    m_movie.insert(QStringLiteral("subtitleTrackOptions"), subtitles);
    m_movie.insert(QStringLiteral("audioTrackIndex"), audioIndex);
    m_movie.insert(QStringLiteral("subtitleTrackIndex"), subtitleIndex);
    m_movie.insert(QStringLiteral("audioTrackCount"), metadata->audioTracks.size());
    m_movie.insert(QStringLiteral("subtitleTrackCount"), metadata->subtitleTracks.size());
    if (!series() && metadata->videoWidth && metadata->videoHeight)
        m_movie.insert(QStringLiteral("resolution"), QStringLiteral("%1 × %2").arg(*metadata->videoWidth).arg(*metadata->videoHeight));
    if (m_mediaProbe) m_movie.remove(QStringLiteral("mediaProbeError"));
    updateSubtitlePresentation();
}
void VodCatalogModel::readMediaMetadata(const ContentRef &ref, const VodDetails &details)
{
    m_mediaProbe = details.mediaProbe;
    if (const auto metadata = m_runtime->playbackMetadata(ref)) m_mediaProbe = metadata;
    updateMediaPresentation();
    const bool fresh = m_mediaProbe
        && m_mediaProbe->observedAtUtc >= QDateTime::currentDateTimeUtc().addDays(-7);
    const auto ordinaryEpisodes = series() ? orderedEpisodes(m_episodes->details()) : QList<EpisodeSummary>{};
    const bool probeAllowed = !series() || (!ordinaryEpisodes.isEmpty() && ref == ordinaryEpisodes.first().ref);
    if (m_purpose == Purpose::Library && m_open && !fresh && !m_runtime->active() && !startingPlayback()
        && probeAllowed
        && (!series() || m_episodes->selectedEpisode().value(QStringLiteral("available")).toBool())) {
        m_movie.insert(QStringLiteral("mediaProbeLoading"), true);
        track(m_controller->probe(ref), Operation::Probe, ref);
    }
}
void VodCatalogModel::applyResolution(const ContentRef &ref, std::optional<int> width, std::optional<int> height)
{
    if (const auto metadata = m_runtime->playbackMetadata(ref)) {
        width = metadata->videoWidth;
        height = metadata->videoHeight;
    }
    QString label;
    const int w = width.value_or(0), h = height.value_or(0);
    if (w > 0 || h > 0) {
        if (w >= 3840 || h >= 2160) label = QStringLiteral("4K");
        else if (w >= 2560 || h >= 1440) label = QStringLiteral("1440p");
        else if (w >= 1920 || h >= 1080) label = QStringLiteral("1080p");
        else if (w >= 1280 || h >= 720) label = QStringLiteral("720p");
        else label = QStringLiteral("480p");
    }
    const auto key = ref.key();
    if (ref == m_selected) m_movie.insert(QStringLiteral("resolutionLabel"), label);
    if (ref == m_playingRef) m_playingMovie.insert(QStringLiteral("resolutionLabel"), label);
    if (m_resolutions.contains(key) && m_resolutions.value(key) == label) return;
    m_resolutions.insert(key, label);
    for (int row = 0; row < count(); ++row)
        if (m_rows[row].ref == ref) emit dataChanged(index(row), index(row), {ResolutionRole});
    emit changed();
}
void VodCatalogModel::requestResolution(int row)
{
    if (series()) return;
    const auto *item = itemAt(row);
    if (!m_open || !m_controller || !item || m_selected.playable() || startingPlayback()) return;
    if (pending(Operation::Query) || pending(Operation::Refresh)) return;
    const auto key = item->ref.key();
    if (m_resolutions.contains(key) || m_requestedResolutions.contains(key)) return;
    const auto requests = std::count_if(m_pending.cbegin(), m_pending.cend(), [](const Pending &p) { return p.kind == Operation::CardResolution; });
    if (requests >= 2) return;
    m_requestedResolutions.insert(key);
    track(m_controller->details(item->ref), Operation::CardResolution, item->ref);
}
void VodCatalogModel::requestProgress(int row)
{
    const auto *item = itemAt(row);
    if (!m_open || !m_controller || !item) return;
    if (m_marking || pending(Operation::Query) || pending(Operation::Refresh) || pending(Operation::Play)) return;
    const auto &ref = item->ref;
    const auto key = ref.key();
    if (m_requestedProgress.contains(key)) return;
    const auto requests = std::count_if(m_pending.cbegin(), m_pending.cend(), [](const Pending &p) { return p.kind == Operation::CardProgress; });
    if (requests >= 4) return;
    m_requestedProgress.insert(key);
    track(m_controller->progress(ref), Operation::CardProgress, ref);
}
void VodCatalogModel::requestPoster(int row)
{
    const auto *item = itemAt(row);
    if (!m_open || !m_controller || !item) return;
    if (m_marking || pending(Operation::Query) || pending(Operation::Refresh) || pending(Operation::Play)) return;
    if (pending(Operation::CardProgress)) return;
    const auto images = std::count_if(m_pending.cbegin(), m_pending.cend(), [](const Pending &p) { return p.kind == Operation::Artwork; });
    if (images >= 24) return;
    if (item->artwork.isEmpty()) return;
    const auto &art = item->artwork.first();
    if (m_posters.contains(art.id) || m_requestedPosters.contains(art.id)) return;
    m_requestedPosters.insert(art.id);
    track(m_controller->artwork(m_profile, art), Operation::Artwork, item->ref);
}
void VodCatalogModel::rememberPosterPaths(const QList<ArtworkRef> &artwork)
{
    for (const auto &art : artwork) {
        if (const auto path = art.cachedPath) {
            m_posters.insert(art.id, QUrl::fromLocalFile(*path).toString());
            m_requestedPosters.remove(art.id);
        } else {
            m_posters.remove(art.id); // A file evicted from disk is a new cache miss.
            m_requestedPosters.remove(art.id);
        }
    }
}
const MovieSummary *VodCatalogModel::itemAt(int row) const
{
    // Negative indices address the bounded Continue watching shelf.
    if (row < 0) {
        const auto shelf = -static_cast<qint64>(row) - 1;
        return shelf < m_continueRows.size() ? &m_continueRows[shelf] : nullptr;
    }
    return row < count() ? &m_rows[row] : nullptr;
}
QVariantList VodCatalogModel::continueMovies() const
{
    QVariantList result;
    for (const auto &item : m_continueRows)
        result.append(QVariantMap{{QStringLiteral("movieKey"), QString::fromLatin1(item.ref.key().toHex())},
            {QStringLiteral("title"), item.title},
            {QStringLiteral("year"), series() ? m_episodeLabels.value(item.ref.key()) : item.year ? QString::number(*item.year) : QString{}},
            {QStringLiteral("poster"), item.artwork.isEmpty() ? QString{} : m_posters.value(item.artwork.first().id)},
            {QStringLiteral("resolutionLabel"), m_resolutions.value(item.ref.key())},
            {QStringLiteral("progressFraction"), m_progress.value(item.ref.key())},
            {QStringLiteral("toWatch"), m_movieLists.value(item.ref.key()).toWatch},
            {QStringLiteral("favourite"), m_movieLists.value(item.ref.key()).favourite},
            {QStringLiteral("listsBusy"), listsBusy(item.ref)}});
    return result;
}
void VodCatalogModel::queryContinue()
{
    if (m_purpose == Purpose::PlaybackSidebar || !m_open || !m_controller || m_scope.catalogNamespace.isNull()) return;
    cancelKind(Operation::ContinueQuery);
    CatalogQuery query; query.scope = m_scope; query.continueWatchingOnly = true; query.pageSize = 12;
    track(m_controller->query(query), Operation::ContinueQuery);
}
void VodCatalogModel::applyProgress(const ContentRef &ref, const VodProgress &progress)
{
    const bool watched = progress.status == WatchStatus::Watched || watchedByPosition(progress.positionMs, progress.durationMs);
    const auto fraction = watched ? 1.0 : progressFraction(progress);
    const auto key = ref.key();
    const bool eligible = !watched && progress.positionMs > 0;
    if (!m_continueEligibility.contains(key) || m_continueEligibility.value(key) != eligible)
        m_continueChanged.insert(key);
    m_continueEligibility.insert(key, eligible);
    m_progress.insert(key, fraction);
    for (int row = 0; row < count(); ++row)
        if (m_rows[row].ref == ref) emit dataChanged(index(row), index(row), {ProgressRole});
    if (ref == m_selected || (series() && ref == m_episodes->selectedRef())) {
        m_movie.insert(QStringLiteral("watched"), series() ? m_episodes->allWatched() : watched);
        m_movie.insert(QStringLiteral("resumeSeconds"), !watched && progress.positionMs >= 60000 ? progress.positionMs / 1000 : 0);
    }
    emit changed();
}
bool VodCatalogModel::listsBusy(const ContentRef &ref) const
{
    const auto *service = m_runtime->module()->progressService();
    return service && service->movieListWritePending(ref);
}
void VodCatalogModel::applyMovieLists(const ContentRef &ref, const MovieListState &state)
{
    if (ref.profileId != m_profile || ref.catalogNamespace != m_scope.catalogNamespace) return;
    m_movieLists.insert(ref.key(), state);
    for (int row = 0; row < count(); ++row)
        if (m_rows[row].ref == ref) emit dataChanged(index(row), index(row), {ToWatchRole, FavouriteRole, ListsBusyRole});
    if (ref == m_selected) {
        m_movie.insert(QStringLiteral("toWatch"), state.toWatch);
        m_movie.insert(QStringLiteral("favourite"), state.favourite);
        m_movie.insert(QStringLiteral("listsLoaded"), true);
        m_movie.insert(QStringLiteral("listsBusy"), listsBusy(ref));
    }
    emit changed();
}
void VodCatalogModel::toggleToWatch(const QString &key) { toggleMovieList(key, MovieList::ToWatch); }
void VodCatalogModel::toggleFavourite(const QString &key) { toggleMovieList(key, MovieList::Favourites); }
void VodCatalogModel::toggleMovieList(const QString &key, MovieList list)
{
    if (!m_open || !m_controller || startingPlayback()) return;
    const auto identity = QByteArray::fromHex(key.toLatin1());
    ContentRef ref;
    if (m_selected.key() == identity) ref = m_selected;
    else {
        for (const auto &item : m_rows) if (item.ref.key() == identity) { ref = item.ref; break; }
        if (!ref.valid()) for (const auto &item : m_continueRows) if (item.ref.key() == identity) { ref = item.ref; break; }
    }
    if (!ref.valid() || !m_movieLists.contains(identity) || listsBusy(ref)) return;
    const auto state = m_movieLists.value(identity);
    const bool enabled = list == MovieList::ToWatch ? !state.toWatch : !state.favourite;
    m_error.clear();
    const QPointer<VodCatalogModel> self(this);
    m_runtime->module()->progressService()->setMovieList(ref, list, enabled, [self, ref](Result<MovieListState> result) {
        if (!self || ref.profileId != self->m_profile || ref.catalogNamespace != self->m_scope.catalogNamespace) return;
        if (const auto *error = std::get_if<Error>(&result)) self->m_error = error->message();
        else self->applyMovieLists(ref, std::get<MovieListState>(result));
        emit self->changed();
    });
}
void VodCatalogModel::toggleWatched()
{
    if (series()) { m_episodes->toggleWatched(); return; }
    if (!m_controller || !m_selected.playable() || busy() || !m_movie.value(QStringLiteral("progressLoaded")).toBool()) return;
    const auto ref = m_selected;
    cancelKind(Operation::CardProgress); cancelKind(Operation::CardResolution); m_requestedProgress.clear();
    cancelKind(Operation::Progress);
    m_marking = true; m_error.clear(); emit changed();
    const QPointer<VodCatalogModel> self(this);
    m_runtime->module()->progressService()->setWatched(ref, !m_movie.value(QStringLiteral("watched")).toBool(),
        [self, ref](Result<VodProgress> result) {
            if (!self) return;
            self->m_marking = false;
            if (const auto *error = std::get_if<Error>(&result)) self->m_error = error->message();
            else if (ref.profileId == self->m_profile) self->applyProgress(ref, std::get<VodProgress>(result));
            emit self->changed();
        });
}
void VodCatalogModel::receive(const VodEvent &event)
{
    if (!m_pending.contains(event.operationId)) return;
    const auto request = m_pending.take(event.operationId);
    if (request.kind == Operation::Probe) m_probePlayDelay.stop();
    if (const auto *error = std::get_if<Error>(&event.result)) {
        if (request.kind == Operation::ContinueQuery && error->code != ErrorCode::Cancelled)
            m_continueMoviesLoaded = true;
        if (request.kind == Operation::Query && error->code == ErrorCode::Cancelled && request.generation == m_queryGeneration) query();
        else if (request.kind == Operation::Probe && request.ref == (series() ? m_episodes->selectedRef() : m_selected)) {
            m_movie.insert(QStringLiteral("mediaProbeLoading"), false);
            if (error->code != ErrorCode::Cancelled) m_movie.insert(QStringLiteral("mediaProbeError"), error->code == ErrorCode::UnsupportedCapability
                    ? QStringLiteral("ffprobe is unavailable; track selection will be available after playback starts.")
                    : QStringLiteral("Track information could not be read; you can still play this %1.").arg(series() ? QStringLiteral("episode") : QStringLiteral("movie")));
        }
        else if (request.kind != Operation::SeasonMetadata && request.kind != Operation::PlayingDetails && request.kind != Operation::PlayingArtwork && request.kind != Operation::Artwork && request.kind != Operation::CardProgress && request.kind != Operation::CardResolution && error->code != ErrorCode::Cancelled) {
            QString context;
            if (request.kind == Operation::Scope) context = QStringLiteral("VOD source could not be read: ");
            else if (request.kind == Operation::Categories) context = series()
                ? QStringLiteral("Series categories could not be loaded: ") : QStringLiteral("Movie categories could not be loaded: ");
            else if (request.kind == Operation::Query || request.kind == Operation::ContinueQuery) context = series()
                ? QStringLiteral("Series catalogue could not be read: ") : QStringLiteral("Movie catalogue could not be read: ");
            m_error = context + error->message();
        }
        emit changed(); return;
    }
    const auto &value = std::get<PublicValue>(event.result);
    switch (request.kind) {
    case Operation::Scope:
        m_scope = std::get<CatalogScope>(value); m_scope.kind=m_kind;
        track(m_controller->categories(m_scope), Operation::Categories); query(); queryContinue(); break;
    case Operation::Categories: {
        m_categories = {
            QVariantMap{{QStringLiteral("id"), QStringLiteral("__continue_watching__")}, {QStringLiteral("name"), QStringLiteral("Continue watching")}, {QStringLiteral("iconSource"), QStringLiteral("qrc:/resources/icons/play.svg")}},
            QVariantMap{{QStringLiteral("id"), QStringLiteral("__to_watch__")}, {QStringLiteral("name"), QStringLiteral("To Watch")}, {QStringLiteral("iconSource"), QStringLiteral("qrc:/resources/icons/bookmark.svg")}},
            QVariantMap{{QStringLiteral("id"), QStringLiteral("__favourites__")}, {QStringLiteral("name"), QStringLiteral("Favourites")}, {QStringLiteral("iconSource"), QStringLiteral("qrc:/resources/icons/favourites.svg")}}};
        const auto key = m_profile.toString(QUuid::WithoutBraces) + (series() ? QStringLiteral("|series") : QStringLiteral("|movies"));
        const auto hidden = m_settings->current().hiddenGroupsByProfile.value(key);
        const auto order = m_settings->current().groupOrderByProfile.value(key);
        auto categories = std::get<CategorySnapshot>(value).categories;
        std::stable_sort(categories.begin(), categories.end(), [&](const VodCategory &a, const VodCategory &b) {
            const auto ai = order.indexOf(a.id), bi = order.indexOf(b.id);
            return (ai < 0 ? order.size() : ai) < (bi < 0 ? order.size() : bi);
        });
        for (const auto &category : categories) if (!hidden.contains(category.id))
            m_categories.append(QVariantMap{{QStringLiteral("id"), category.id}, {QStringLiteral("name"), category.name}});
        break;
    }
    case Operation::Refresh: query(); queryContinue(); break;
    case Operation::ContinueQuery: {
        if (request.listsRevision != m_listsRevision) { queryContinue(); break; }
        const auto &page = std::get<CatalogPage>(value);
        if (!page.refreshedAtUtc.isValid()) break;
        m_continueMoviesLoaded = true;
        m_continueRows.clear();
        for (const auto &item : page.items)
            if (const auto movie = std::visit([](const auto &summary) { return std::optional<MovieSummary>{{summary.ref,summary.title,summary.year,summary.artwork,summary.categoryIds,summary.availability}}; }, item)) {
                rememberPosterPaths(movie->artwork);
                m_movieLists.insert(movie->ref.key(), page.movieLists.value(movie->ref.key()));
                m_episodeLabels.insert(movie->ref.key(),page.continuationLabels.value(movie->ref.key()));
                m_continueRows.append(*movie);
            }
        break;
    }
    case Operation::Query: {
        if (request.generation != m_queryGeneration) break;
        if (request.listsRevision != m_listsRevision) { query(); break; }
        const auto &page = std::get<CatalogPage>(value);
        if (!page.refreshedAtUtc.isValid()) { if (!m_runtime->syncing(m_profile) && m_runtime->syncError(m_profile, m_kind).isEmpty()) refresh(); break; }
        if (!request.append) resetRows();
        m_catalogLoaded = true;
        if (!page.items.isEmpty()) {
            const int first = count(); beginInsertRows({}, first, first + static_cast<int>(page.items.size()) - 1);
            for (const auto &item : page.items) if (const auto movie = std::visit([](const auto &summary) { return std::optional<MovieSummary>{{summary.ref,summary.title,summary.year,summary.artwork,summary.categoryIds,summary.availability}}; }, item)) {
                rememberPosterPaths(movie->artwork);
                m_movieLists.insert(movie->ref.key(), page.movieLists.value(movie->ref.key()));
                m_episodeLabels.insert(movie->ref.key(),page.continuationLabels.value(movie->ref.key()));
                m_rows.append(*movie);
            }
            endInsertRows();
        }
        m_next = page.next;
        if (m_purpose == Purpose::PlaybackSidebar) updatePlayingSummary();
        break;
    }
    case Operation::MovieLists: {
        if (request.ref.profileId != m_profile || request.ref.catalogNamespace != m_scope.catalogNamespace) break;
        if (request.listsRevision != m_listsRevision) { track(m_controller->movieLists(request.ref), Operation::MovieLists, request.ref); break; }
        applyMovieLists(request.ref, std::get<MovieListState>(value));
        break;
    }
    case Operation::CardResolution: {
        if (request.generation != m_queryGeneration) { m_requestedResolutions.remove(request.ref.key()); break; }
        const auto &details = std::get<VodDetails>(value);
        applyResolution(request.ref, details.mediaProbe ? details.mediaProbe->videoWidth : details.declaredVideoWidth,
            details.mediaProbe ? details.mediaProbe->videoHeight : details.declaredVideoHeight);
        break;
    }
    case Operation::EpisodeDetails: {
        if (request.ref!=m_episodes->selectedRef()) break;
        const auto &details=std::get<VodDetails>(value);
        readMediaMetadata(request.ref, details);
        break;
    }
    case Operation::SeasonMetadata: {
        if (request.ref != m_episodes->selectedRef()) break;
        const auto metadata = std::get<std::optional<VodMediaProbe>>(value);
        if (metadata && (!m_seasonTrackMetadata || metadata->observedAtUtc >= m_seasonTrackMetadata->observedAtUtc))
            m_seasonTrackMetadata = metadata;
        updateMediaPresentation();
        break;
    }
    case Operation::Details: {
        if (request.ref != m_selected) break;
        auto details = std::get<VodDetails>(value);
        if (const auto metadata = m_runtime->playbackMetadata(request.ref)) details.mediaProbe = metadata;
        applyResolution(request.ref, details.mediaProbe ? details.mediaProbe->videoWidth : details.declaredVideoWidth,
            details.mediaProbe ? details.mediaProbe->videoHeight : details.declaredVideoHeight);
        rememberPosterPaths(details.artwork);
        if (!details.artwork.isEmpty()) {
            const auto &art = details.artwork.first();
            const auto poster = m_posters.value(art.id);
            if (!poster.isEmpty()) m_movie.insert(QStringLiteral("poster"), poster);
            else if (m_movie.value(QStringLiteral("poster")).toString().isEmpty())
                track(m_controller->artwork(m_profile, art), Operation::Artwork, m_selected);
        }
        if (series() && !details.title.isEmpty()) m_movie.insert(QStringLiteral("title"),details.title);
        m_movie.insert(QStringLiteral("description"), details.description);
        m_movie.insert(QStringLiteral("genres"), details.genres.join(QStringLiteral(" · ")));
        m_movie.insert(QStringLiteral("cast"), details.cast.join(QStringLiteral(", ")));
        const auto duration = details.declaredDurationMs;
        m_movie.insert(QStringLiteral("durationMinutes"), duration ? *duration / 60000 : 0);
        if (details.declaredVideoWidth && details.declaredVideoHeight)
            m_movie.insert(QStringLiteral("resolution"), QStringLiteral("%1 × %2").arg(*details.declaredVideoWidth).arg(*details.declaredVideoHeight));
        if (!series()) readMediaMetadata(request.ref, details);
        else if (m_mediaProbe) updateMediaPresentation();
        break;
    }
    case Operation::PlayingDetails: {
        if (request.ref != m_playingRef) break;
        const auto &details = std::get<VodDetails>(value);
        if (const auto metadata = m_runtime->playbackMetadata(request.ref))
            m_controller->cachePlaybackMetadata(request.ref, *metadata, m_runtime->module()->session()->snapshot().durationMs);
        applyResolution(request.ref, details.mediaProbe ? details.mediaProbe->videoWidth : details.declaredVideoWidth,
            details.mediaProbe ? details.mediaProbe->videoHeight : details.declaredVideoHeight);
        if (series() && !details.title.isEmpty()) m_playingMovie.insert(QStringLiteral("title"),details.title);
        m_playingMovie.insert(QStringLiteral("description"), details.description);
        m_playingMovie.insert(QStringLiteral("cast"), details.cast.join(QStringLiteral(", ")));
        m_playingMovie.insert(QStringLiteral("genres"), details.genres.join(QStringLiteral(" · ")));
        const auto duration = m_runtime->module()->session()->snapshot().durationMs;
        m_playingMovie.insert(QStringLiteral("durationMinutes"), duration.value_or(details.declaredDurationMs.value_or(0)) / 60000);
        rememberPosterPaths(details.artwork);
        if (!details.artwork.isEmpty()) {
            const auto &art = details.artwork.first();
            const auto poster = m_posters.value(art.id);
            if (!poster.isEmpty()) m_playingMovie.insert(QStringLiteral("poster"), poster);
            else track(m_controller->artwork(request.ref.profileId, art), Operation::PlayingArtwork, request.ref);
        }
        break;
    }
    case Operation::PlayingArtwork: {
        if (request.ref != m_playingRef) break;
        const auto art = std::get<ArtworkRef>(value);
        rememberPosterPaths({art});
        const auto path = art.cachedPath;
        if (path) m_playingMovie.insert(QStringLiteral("poster"), QUrl::fromLocalFile(*path).toString());
        break;
    }
    case Operation::Probe:
        if (request.ref == (series() ? m_episodes->selectedRef() : m_selected)) { m_mediaProbe = std::get<VodMediaProbe>(value); updateMediaPresentation(); }
        break;
    case Operation::CardProgress: {
        if (request.generation != m_queryGeneration) break;
        const auto progress = std::get<std::optional<VodProgress>>(value);
        auto effective = progress.value_or(VodProgress{});
        const auto &snapshot = m_runtime->module()->session()->snapshot();
        if (snapshot.ref == request.ref && snapshot.positionValid)
            effective = m_runtime->module()->progressService()->observedProgress(snapshot);
        applyProgress(request.ref, effective);
        break;
    }
    case Operation::Progress: {
        if (request.ref != (series() ? m_episodes->selectedRef() : m_selected)) break;
        auto effective = std::get<std::optional<VodProgress>>(value).value_or(VodProgress{});
        const auto &snapshot = m_runtime->module()->session()->snapshot();
        if (snapshot.ref == request.ref && snapshot.positionValid)
            effective = m_runtime->module()->progressService()->observedProgress(snapshot);
        m_playbackTrackPreferences = effective.trackPreferences;
        applyProgress(request.ref, effective);
        m_movie.insert(QStringLiteral("progressLoaded"), true);
        updateMediaPresentation();
        break;
    }
    case Operation::Play:
        if (series()) {
            m_runtime->setSeriesTitle(m_startTitle);
            const auto episode=m_episodes->selectedEpisode();
            m_runtime->setPlaybackTitle(request.ref,m_startTitle+QStringLiteral(" · ")+episode.value(QStringLiteral("label")).toString()+QStringLiteral(" · ")+episode.value(QStringLiteral("title")).toString(),m_startYear);
        } else m_runtime->setPlaybackTitle(request.ref, m_startTitle, m_startYear);
        emit playbackStarted(); break;
    case Operation::Artwork: {
        const auto art = std::get<ArtworkRef>(value);
        const auto path = art.cachedPath;
        if (!path) break;
        const auto url = QUrl::fromLocalFile(*path).toString(); m_posters.insert(art.id, url);
        if (request.ref == m_selected) m_movie.insert(QStringLiteral("poster"), url);
        for (int row = 0; row < count(); ++row) {
            const auto &item = m_rows[row];
            if (item.artwork.isEmpty() || item.artwork.first().id != art.id) continue;
            emit dataChanged(index(row), index(row), {PosterRole});
            if (item.ref == m_selected) m_movie.insert(QStringLiteral("poster"), url);
        }
        break;
    }
    }
    emit changed();
}
}

namespace OKILTV::Vod {
VodCatalogModel::~VodCatalogModel() = default;
QObject *VodCatalogModel::episodesObject() const { return m_episodes.get(); }
void VodCatalogModel::openSeries(const ContentRef &ref) {
    if (!series() || !ref.valid()) return;
    open();
    if (m_profile != ref.profileId) selectSource(ref.profileId.toString(QUuid::WithoutBraces));
    back(); m_selected=ref;
    m_movie={{QStringLiteral("title"),m_runtime->playbackYear().isEmpty() ? QStringLiteral("Series") : m_runtime->title()},
        {QStringLiteral("movieKey"),QString::fromLatin1(ref.key().toHex())},{QStringLiteral("available"),true}};
    track(m_controller->details(ref),Operation::Details,ref);
    track(m_controller->movieLists(ref),Operation::MovieLists,ref);
    const auto active=m_runtime->module()->session()->snapshot().ref;
    m_episodes->load(ref,parentSeries(active)==ref ? active : ContentRef{});
    emit changed();
}
}
