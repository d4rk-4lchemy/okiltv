#pragma once
#include "vodmodule.h"
#include "vodartworkcache.h"
#include "core/vod/storage/sqlitevodstore.h"
#include <QPointer>
#include <QFutureSynchronizer>

namespace OKILTV::Core { class SettingsManager; struct ServerProfile; struct ChannelCategory; }
namespace OKILTV::App { class MultiViewController; class PlayerController; class DvrController; class TimeshiftController; }
namespace OKILTV::Vod {
class VodEpisodesModel;
// Production composition and active transport; no catalog screens or routing.
class VodRuntime final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool activeSeries READ activeSeries NOTIFY stateChanged)
    Q_PROPERTY(QObject *episodesModel READ episodesObject CONSTANT)
    Q_PROPERTY(bool hasPreviousEpisode READ hasPreviousEpisode NOTIFY stateChanged)
    Q_PROPERTY(bool hasNextEpisode READ hasNextEpisode NOTIFY stateChanged)
    Q_PROPERTY(bool episodeTransition READ episodeTransition NOTIFY stateChanged)
    Q_PROPERTY(bool libraryReturnPending READ libraryReturnPending NOTIFY stateChanged)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(bool liveTransition READ liveTransition NOTIFY stateChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY stateChanged)
    Q_PROPERTY(bool isPaused READ isPaused NOTIFY stateChanged)
    Q_PROPERTY(bool isLoading READ isLoading NOTIFY stateChanged)
    Q_PROPERTY(QObject *playerObject READ playerObject NOTIFY stateChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY stateChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds NOTIFY stateChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds NOTIFY stateChanged)
    Q_PROPERTY(bool seekable READ seekable NOTIFY stateChanged)
    Q_PROPERTY(QString title READ title NOTIFY stateChanged)
public:
    using EngineFactory = std::function<std::unique_ptr<Player::IPlaybackEngine>()>;
    VodRuntime(Core::SettingsManager *, App::MultiViewController *, App::DvrController *, App::TimeshiftController *,
        QObject *parent = nullptr, EngineFactory engineFactory = {});
    ~VodRuntime() override;
    bool active() const;
    bool liveTransition() const { return m_liveTransition; }
    bool activeSeries() const;
    QObject *episodesObject() const;
    bool hasPreviousEpisode() const;
    bool hasNextEpisode() const;
    bool episodeTransition() const { return m_episodeTransition; }
    bool libraryReturnPending() const { return m_returnSeries.valid() && !m_returnAfterStop; }
    Q_INVOKABLE void previousEpisode();
    Q_INVOKABLE void nextEpisode();
    Q_INVOKABLE void returnToSeriesLibrary();
    Q_INVOKABLE void deliverLibraryReturn();
    bool ready() const { return m_ready; }
    bool isPlaying() const;
    bool isPaused() const;
    bool isLoading() const;
    QObject *playerObject() const;
    double volume() const;
    double positionSeconds() const;
    double durationSeconds() const;
    bool seekable() const;
    QString title() const;
    QString playbackYear() const;
    void setSeriesTitle(const QString &title) { m_seriesTitle=title; }
    void setPlaybackTitle(const ContentRef &, const QString &, const QString &year = {});
    std::optional<VodMediaProbe> playbackMetadata(const ContentRef &ref) const;
    Q_INVOKABLE QVariantMap debugOverlaySnapshot() const;
    Q_INVOKABLE QVariantList audioTracks() const;
    Q_INVOKABLE QVariantList subtitleTracks() const;
    Q_INVOKABLE void selectAudioTrack(int id);
    Q_INVOKABLE void selectSubtitleTrack(int id);
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void beginLibraryBrowsing();
    Q_INVOKABLE void finishLibraryBrowsing(bool resumePlayback = true);
    Q_INVOKABLE void returnToLive();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seekRelative(double seconds);
    Q_INVOKABLE void seekTo(double seconds);
    Q_INVOKABLE void toggleMute();
    void setVolume(double);
    void applyPictureSettings();
    void shutdown();
    void ensureForSource(const QUuid &profile);
    Q_INVOKABLE void synchronizeSource(const QString &profile);
    void policyChanged(const QUuid &profile);
    QList<Core::ChannelCategory> sourceCategories(const QUuid &, CatalogKind) const;
    QString syncError(const QUuid &id) const { return m_syncErrors.value(id); }
    bool syncing(const QUuid &id) const { return m_syncing.contains(id); }
    void retryInitialization();
    VodModule *module() const { return m_module.get(); }
    std::shared_ptr<SqliteVodStore> store() const { return m_store; }
signals:
    void seriesReturnRequested(const OKILTV::Vod::ContentRef &);
    void libraryRequested();
    void liveRequested();
    void episodeStarted();
    void stateChanged();
    void errorOccurred(const QString &message);
    void sourcesReconciled();
    void notification(const QString &message);
    void sourceInvalidated(const QUuid &profile);
    void sourceUpdated(const QUuid &profile);
    void categoriesUpdated(const QUuid &profile);
    void sourceSyncChanged(const QUuid &profile);
    void sourceSyncFinished(const QUuid &profile);
    void playbackMetadataChanged(const OKILTV::Vod::ContentRef &ref);
private:
    void cancelLiveNavigation();
    QUuid m_libraryPausedSession;
    quint64 m_liveGeneration = 0;
    bool m_liveTransition = false;
    void initialize(EngineFactory factory = {});
    void observeEpisode(const SessionSnapshot &);
    void navigateEpisode(int);
    void finishSeriesReturn();
    bool m_returnAfterStop = false;
    void completeEpisode(const SessionSnapshot &, quint64 generation);
    std::optional<std::pair<SessionSnapshot,quint64>> m_pendingEpisodeEnd;
    void startEpisode(const ContentRef &, bool fromBeginning);
    void cancelEpisodeTransition();
    std::unique_ptr<VodEpisodesModel> m_episodes;
    quint64 m_episodeGeneration = 0;
    QUuid m_episodeSession, m_handledEnd, m_episodePlay;
    ContentRef m_returnSeries;
    bool m_episodeTransition = false;
    void updatePlaybackMetadata(const SessionSnapshot &);
    QUuid m_metadataLoad;
    ContentRef m_metadataRef;
    std::optional<VodMediaProbe> m_metadata;
    void reconcileSources();
    EngineFactory m_deferredFactory;
    ContentRef m_titleRef;
    QString m_title;
    QString m_year;
    QString m_seriesTitle;
    struct Policy { quint64 generation = 0; QStringList categories; QStringList hidden; QStringList seriesCategories; QStringList seriesHidden; bool seriesConfigured = false; bool configured = false; bool enabled = false; };
    struct Policies { std::shared_ptr<std::recursive_mutex> mutex = std::make_shared<std::recursive_mutex>(); QHash<QUuid, Policy> values; };
    std::shared_ptr<Policies> m_policies = std::make_shared<Policies>();
    void updatePolicy(const QUuid &, std::optional<QStringList> categories = {}, CatalogKind kind = CatalogKind::Movies);
    enum class SyncStage { Scope, Movies, Series, Catalog, SeriesCatalog };
    struct SyncRequest { QUuid profile; SyncStage stage; CatalogScope scope; };
    QHash<QUuid, SyncRequest> m_syncRequests;
    QSet<QUuid> m_syncing;
    QSet<QUuid> m_queuedSync;
    QHash<QUuid, QString> m_syncErrors;
    void receiveSync(const VodEvent &);
    bool reconcileCategories(const CategorySnapshot &);
    void finishSync(const QUuid &);
    std::shared_ptr<VodArtworkCache> m_artwork;
    bool gate(QObject *owner, std::function<void()>);
    void bindPlayer(App::PlayerController *);
    void prepareRecording(std::function<void(bool)>);
    QList<std::function<void(bool)>> m_recordingWaiters;
    bool m_recordingRetryPending = false;
    void finishMutation(const QUuid &);
    QSet<QUuid> m_preparingEdits;
    QSet<QUuid> m_finishingEdits;
    QSet<QUuid> m_editCompletionRequested;
    bool prepareMutation(const QUuid &, const Core::ServerProfile *, QString *);
    Core::SettingsManager *m_settings;
    App::MultiViewController *m_multiview;
    App::DvrController *m_dvr;
    App::TimeshiftController *m_timeshift;
    QFutureSynchronizer<Outcome> m_sourceWork;
    QFuture<Result<QList<QUuid>>> m_reconcileWork;
    mutable QMutex m_storeMutex;
    std::shared_ptr<SqliteVodStore> m_store;
    std::unique_ptr<VodModule> m_module;
    QList<QPointer<App::PlayerController>> m_players;
    std::function<void()> m_liveAction;
    bool m_approvedLive = false;
    bool m_stopped = false;
    bool m_ready = false;
    bool m_reconciling = false;
    QHash<QUuid, PlaybackCoordinator::Completion> m_removals;
};
}
