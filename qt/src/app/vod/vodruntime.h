#pragma once
#include "vodmodule.h"
#include "vodartworkcache.h"
#include "core/vod/storage/sqlitevodstore.h"
#include <QPointer>
#include <QFutureSynchronizer>

namespace OKILTV::Core { class SettingsManager; struct ServerProfile; }
namespace OKILTV::App { class MultiViewController; class PlayerController; class DvrController; class TimeshiftController; }
namespace OKILTV::Vod {
// Production composition and active transport; no catalog screens or routing.
class VodRuntime final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
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
    void setPlaybackTitle(const ContentRef &, const QString &, const QString &year = {});
    std::optional<VodMediaProbe> playbackMetadata(const ContentRef &ref) const;
    Q_INVOKABLE QVariantMap debugOverlaySnapshot() const;
    Q_INVOKABLE QVariantList audioTracks() const;
    Q_INVOKABLE QVariantList subtitleTracks() const;
    Q_INVOKABLE void selectAudioTrack(int id);
    Q_INVOKABLE void selectSubtitleTrack(int id);
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seekRelative(double seconds);
    Q_INVOKABLE void seekTo(double seconds);
    Q_INVOKABLE void toggleMute();
    void setVolume(double);
    void applyPictureSettings();
    void shutdown();
    void enableForSession(const QUuid &profile);
    void retryInitialization();
    VodModule *module() const { return m_module.get(); }
    std::shared_ptr<SqliteVodStore> store() const { return m_store; }
signals:
    void stateChanged();
    void errorOccurred(const QString &message);
    void sourcesReconciled();
    void notification(const QString &message);
    void sourceInvalidated(const QUuid &profile);
    void sourceUpdated(const QUuid &profile);
    void playbackMetadataChanged(const OKILTV::Vod::ContentRef &ref);
private:
    void initialize(EngineFactory factory = {});
    void updatePlaybackMetadata(const SessionSnapshot &);
    QUuid m_metadataLoad;
    ContentRef m_metadataRef;
    std::optional<VodMediaProbe> m_metadata;
    void reconcileSources();
    EngineFactory m_deferredFactory;
    ContentRef m_titleRef;
    QString m_title;
    QString m_year;
    struct SessionSources { QMutex mutex; QSet<QUuid> enabled; };
    std::shared_ptr<SessionSources> m_sessionSources = std::make_shared<SessionSources>();
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
