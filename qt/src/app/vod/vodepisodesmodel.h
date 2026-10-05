#pragma once
#include "vodruntime.h"
#include <QAbstractListModel>

namespace OKILTV::Vod {
// Independent selection instances for library details and active playback panel.
class VodEpisodesModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QVariantList seasons READ seasons NOTIFY changed)
    Q_PROPERTY(QVariantList episodes READ episodes NOTIFY changed)
    Q_PROPERTY(QAbstractItemModel *rows READ rows CONSTANT)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY changed)
    Q_PROPERTY(bool allWatched READ allWatched NOTIFY changed)
    Q_PROPERTY(bool statusReady READ statusReady NOTIFY changed)
    Q_PROPERTY(bool statusBusy READ statusBusy NOTIFY changed)
    Q_PROPERTY(QString seasonId READ seasonId NOTIFY changed)
    Q_PROPERTY(QVariantMap selectedEpisode READ selectedEpisode NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
public:
    explicit VodEpisodesModel(VodRuntime *, QObject *parent = nullptr);
    void load(const ContentRef &, const ContentRef &preferred = {}, bool refresh = false);
    void clear();
    QVariantList seasons() const;
    QVariantList episodes() const;
    QAbstractItemModel *rows() { return this; }
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &, int) const override;
    QHash<int, QByteArray> roleNames() const override;
    Q_INVOKABLE QVariantMap get(int) const;
    int currentIndex() const;
    bool allWatched() const;
    bool statusReady() const { return m_historyLoaded && !m_details.episodes.isEmpty() && m_pending.isEmpty(); }
    bool statusBusy() const;
    QString seasonId() const { return m_season; }
    QVariantMap selectedEpisode() const;
    bool busy() const;
    QString errorText() const { return m_error; }
    const VodDetails &details() const { return m_details; }
    ContentRef selectedRef() const { return m_selected; }
    ContentRef seriesRef() const { return m_series; }
    const SeriesProgress &history() const { return m_history; }
    Q_INVOKABLE void selectSeason(const QString &);
    Q_INVOKABLE void selectEpisode(int);
    Q_INVOKABLE void playEpisode(int, bool fromBeginning = false);
    Q_INVOKABLE void toggleWatched();
    Q_INVOKABLE void toggleEpisodeWatched(const QString &episodeKey);
    Q_INVOKABLE void requestArtwork(int);
signals:
    void changed();
    void selectionChanged();
    void playRequested(const OKILTV::Vod::ContentRef &, bool fromBeginning);
    void loaded();
    void rowsChanging();
    void rowsChanged();
private:
    QVariantMap present(const EpisodeSummary &) const;
    void receive(const VodEvent &);
    void choose();
    void updateRows();
    void refreshHistory();
    VodRuntime *m_runtime;
    QPointer<VodController> m_controller;
    QHash<QUuid,QString> m_artworkRequests;
    QSet<QString> m_requestedArtwork;
    QHash<QUuid, bool> m_pending; // true = details, false = durable history
    ContentRef m_series, m_selected, m_preferred;
    QString m_season, m_error;
    VodDetails m_details;
    SeriesProgress m_history;
    QList<QVariantMap> m_rows;
    bool m_historyLoaded = false;
    quint64 m_generation = 0;
};
}
