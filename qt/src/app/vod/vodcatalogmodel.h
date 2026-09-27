#pragma once
#include "vodruntime.h"
#include <QAbstractListModel>
#include <QTimer>

namespace OKILTV::Vod {
class VodCatalogModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QVariantList sources READ sources NOTIFY changed)
    Q_PROPERTY(QVariantList categories READ categories NOTIFY changed)
    Q_PROPERTY(QString sourceId READ sourceId NOTIFY changed)
    Q_PROPERTY(QString categoryId READ categoryId NOTIFY changed)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY changed)
    Q_PROPERTY(bool descending READ descending WRITE setDescending NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool startingPlayback READ startingPlayback NOTIFY changed)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    Q_PROPERTY(QVariantList continueMovies READ continueMovies NOTIFY changed)
    Q_PROPERTY(QVariantMap movie READ movie NOTIFY changed)
public:
    enum Role { KeyRole = Qt::UserRole + 1, TitleRole, YearRole, PosterRole, AvailableRole, ProgressRole, ResolutionRole };
    VodCatalogModel(VodRuntime *, Core::SettingsManager *, QObject *parent = nullptr);
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
    bool hasMore() const { return m_next.has_value(); }
    int count() const { return static_cast<int>(m_rows.size()); }
    QString errorText() const { return m_error; }
    QVariantList continueMovies() const;
    QVariantMap movie() const { return m_movie; }
    void setSearchText(const QString &);
    void setDescending(bool);
    Q_INVOKABLE void open();
    Q_INVOKABLE void close();
    Q_INVOKABLE void selectSource(const QString &);
    Q_INVOKABLE void selectCategory(const QString &);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void fetchMoreMovies();
    Q_INVOKABLE void selectMovie(int row);
    Q_INVOKABLE void back();
    Q_INVOKABLE void toggleWatched();
    Q_INVOKABLE void play(bool fromBeginning = false);
    Q_INVOKABLE void selectAudioOption(int index);
    Q_INVOKABLE void selectSubtitleOption(int index);
    Q_INVOKABLE void requestPoster(int row);
    Q_INVOKABLE void requestProgress(int row);
    Q_INVOKABLE void requestResolution(int row);
    Q_INVOKABLE void reloadSources();
signals:
    void changed();
    void playbackStarted();
private:
    enum class Operation { Scope, Categories, Query, Refresh, Details, Probe, Progress, CardProgress, CardResolution, Play, Artwork, ContinueQuery };
    struct Pending { Operation kind; quint64 generation; ContentRef ref; bool append = false; };
    const MovieSummary *itemAt(int row) const;
    void queryContinue();
    void applyProgress(const ContentRef &, const VodProgress &);
    void updateMediaPresentation();
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
    VodRuntime *m_runtime;
    Core::SettingsManager *m_settings;
    QPointer<VodController> m_controller;
    QVariantList m_sources, m_categories;
    QUuid m_profile;
    CatalogScope m_scope;
    QString m_category, m_search, m_error;
    bool m_descending = false;
    bool m_open = false;
    quint64 m_queryGeneration = 0;
    QList<MovieSummary> m_rows, m_continueRows;
    bool m_marking = false;
    QHash<QByteArray, double> m_progress;
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
    std::optional<VodMediaProbe> m_mediaProbe;
    QJsonObject m_playbackTrackPreferences;
    QHash<QUuid, Pending> m_pending;
    QTimer m_searchTimer;
};
}
