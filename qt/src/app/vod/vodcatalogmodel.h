#pragma once
#include "vodruntime.h"
#include <QAbstractListModel>
#include <QTimer>

namespace OKILTV::Vod {
class VodEpisodesModel;
class VodCatalogModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool series READ series NOTIFY changed)
    Q_PROPERTY(QObject *episodesModel READ episodesObject CONSTANT)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY changed)
    Q_PROPERTY(QVariantList categories READ categories NOTIFY changed)
    Q_PROPERTY(QString sourceId READ sourceId NOTIFY changed)
    Q_PROPERTY(QString categoryId READ categoryId NOTIFY changed)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY changed)
    Q_PROPERTY(bool descending READ descending WRITE setDescending NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool startingPlayback READ startingPlayback NOTIFY changed)
    Q_PROPERTY(bool probePlayBlocked READ probePlayBlocked NOTIFY changed)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    Q_PROPERTY(bool catalogLoaded READ catalogLoaded NOTIFY changed)
    Q_PROPERTY(QVariantList continueMovies READ continueMovies NOTIFY changed)
    Q_PROPERTY(bool continueMoviesLoaded READ continueMoviesLoaded NOTIFY changed)
    Q_PROPERTY(QVariantMap movie READ movie NOTIFY changed)
    Q_PROPERTY(QVariantMap playingMovie READ playingMovie NOTIFY changed)
    Q_PROPERTY(QString playingMovieKey READ playingMovieKey NOTIFY changed)
public:
    enum Role { KeyRole = Qt::UserRole + 1, TitleRole, YearRole, PosterRole, AvailableRole, ProgressRole, ResolutionRole, ToWatchRole, FavouriteRole, ListsBusyRole };
    enum class Purpose { Library, PlaybackSidebar };
    VodCatalogModel(VodRuntime *, Core::SettingsManager *, QObject *parent = nullptr, Purpose purpose = Purpose::Library, CatalogKind kind = CatalogKind::Movies);
    ~VodCatalogModel() override;
    bool series() const { return m_kind == CatalogKind::Series; }
    QObject *episodesObject() const;
    void openSeries(const ContentRef &);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QVariantList sources() const { return m_sources; }
    QVariantList categories() const { return m_categories; }
    QString sourceId() const { return m_profile.toString(QUuid::WithoutBraces); }
    QString categoryId() const { return m_category; }
    QString searchText() const { return m_search; }
    bool descending() const { return m_descending; }
    bool busy() const;
    bool startingPlayback() const;
    bool probePlayBlocked() const { return m_probePlayDelay.isActive(); }
    bool hasMore() const { return m_next.has_value(); }
    int count() const { return static_cast<int>(m_rows.size()); }
    QString errorText() const;
    bool catalogLoaded() const { return m_catalogLoaded; }
    QVariantList continueMovies() const;
    bool continueMoviesLoaded() const { return m_continueMoviesLoaded; }
    QVariantMap movie() const { return m_movie; }
    QVariantMap playingMovie() const { return m_playingMovie; }
    QString playingMovieKey() const { return m_playingRef.valid() ? QString::fromLatin1(m_playingRef.key().toHex()) : QString{}; }
    void setSearchText(const QString &);
    void setDescending(bool);
    Q_INVOKABLE void open();
    Q_INVOKABLE void close();
    Q_INVOKABLE void selectSource(const QString &);
    Q_INVOKABLE void selectCategory(const QString &);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void fetchMoreMovies();
    Q_INVOKABLE void selectMovie(int row);
    Q_INVOKABLE void playRow(int row);
    Q_INVOKABLE void back();
    Q_INVOKABLE void toggleWatched();
    Q_INVOKABLE void toggleToWatch(const QString &movieKey);
    Q_INVOKABLE void toggleFavourite(const QString &movieKey);
    Q_INVOKABLE void play(bool fromBeginning = false);
    Q_INVOKABLE void selectAudioOption(int index);
    Q_INVOKABLE void selectSubtitleOption(int index);
    Q_INVOKABLE void removeSubtitleOption(int index);
    Q_INVOKABLE void requestPoster(int row);
    Q_INVOKABLE void requestProgress(int row);
    Q_INVOKABLE void requestResolution(int row);
    Q_INVOKABLE void reloadSources();
signals:
    void changed();
    void playbackStarted();
private:
    enum class Operation { Scope, Categories, Query, Refresh, Details, EpisodeDetails, SeasonMetadata, Probe, Progress, CardProgress, CardResolution, Play, Artwork, ContinueQuery, PlayingDetails, PlayingArtwork, MovieLists };
    struct Pending { Operation kind; quint64 generation; ContentRef ref; bool append = false; quint64 listsRevision = 0; };
    const MovieSummary *itemAt(int row) const;
    void queryContinue();
    void toggleMovieList(const QString &, MovieList);
    void applyMovieLists(const ContentRef &, const MovieListState &);
    bool listsBusy(const ContentRef &) const;
    void rememberPosterPaths(const QList<ArtworkRef> &);
    void syncPlayback();
    void updatePlayingSummary();
    void applyProgress(const ContentRef &, const VodProgress &);
    void updateMediaPresentation();
    void updateSubtitlePresentation();
    void readMediaMetadata(const ContentRef &, const VodDetails &);
    void waitForStorage();
    void stopWaitingForStorage();
    void applyResolution(const ContentRef &, std::optional<int> width, std::optional<int> height);
    void attach();
    void loadScope();
    void query(bool append = false);
    void receive(const VodEvent &);
    void track(const QUuid &, Operation, const ContentRef & = {}, bool append = false);
    void cancelAll();
    void cancelKind(Operation);
    void resetRows();
    bool pending(Operation) const;
    CatalogKind m_kind;
    std::unique_ptr<VodEpisodesModel> m_episodes;
    VodRuntime *m_runtime;
    Core::SettingsManager *m_settings;
    QPointer<VodController> m_controller;
    QVariantList m_sources, m_categories;
    QUuid m_profile;
    CatalogScope m_scope;
    QString m_category, m_search, m_error;
    bool m_descending = false;
    bool m_open = false;
    Purpose m_purpose;
    ContentRef m_playingRef;
    QVariantMap m_playingMovie;
    QString m_startTitle;
    QString m_startYear;
    quint64 m_queryGeneration = 0;
    quint64 m_listsRevision = 0;
    QHash<QByteArray, MovieListState> m_movieLists;
    QList<MovieSummary> m_rows, m_continueRows;
    bool m_continueMoviesLoaded = false;
    bool m_catalogLoaded = false;
    bool m_marking = false;
    QHash<QByteArray, double> m_progress;
    QHash<QByteArray,QString> m_episodeLabels;
    QHash<QByteArray, QString> m_resolutions;
    QSet<QByteArray> m_requestedResolutions;
    QHash<QByteArray, bool> m_continueEligibility;
    QSet<QByteArray> m_continueChanged;
    QSet<QByteArray> m_requestedProgress;
    QHash<QString, QString> m_posters;
    QSet<QString> m_requestedPosters;
    std::optional<LocalPageToken> m_next;
    QVariantMap m_movie;
    ContentRef m_selected;
    ContentRef m_refreshSeriesDetails;
    bool m_trackOptionsEdited = false;
    std::optional<VodMediaProbe> m_mediaProbe;
    std::optional<VodMediaProbe> m_seasonTrackMetadata;
    QString m_trackMetadataSeason;
    QJsonObject m_playbackTrackPreferences;
    QHash<QUuid, Pending> m_pending;
    QTimer m_searchTimer;
    QTimer m_probePlayDelay;
    QTimer m_storageWaitTimeout;
    QTimer m_storageRetry;
};
}
