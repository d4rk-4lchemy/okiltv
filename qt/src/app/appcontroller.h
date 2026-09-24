#pragma once

#include "../core/database_service.h"
#include "../core/epgcache_service.h"
#include "../core/epgservice.h"
#include "../core/iconcache_service.h"
#include "../core/m3uservice.h"
#include "../core/networkaccess.h"
#include "../core/settingsmanager.h"

#include "playercontroller.h"
#include <QPointer>
#include <QObject>
#include <QElapsedTimer>
#include <QFutureSynchronizer>
#include <QTimer>

#include <optional>

namespace OKILTV::App {

class CatchupDownloadController;
class ChannelListModel;
class DvrController;
class EpgGridModel;
class GuideStateModel;
class MultiViewController;
class NowNextModel;
class PlayerController;
class ProfilesModel;
class ShellController;
class SettingsController;
class TimeshiftController;
struct CatchupProgressSample;

class AppController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList groupAutoEnableNoticeProfileIds READ groupAutoEnableNoticeProfileIds NOTIFY groupAutoEnableNoticesChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(bool isBusy READ isBusy NOTIFY isBusyChanged)
    Q_PROPERTY(QString activeProfileId READ activeProfileId NOTIFY activeProfileIdChanged)
    Q_PROPERTY(QString epgLastRefreshText READ epgLastRefreshText NOTIFY epgRefreshStateChanged)
    Q_PROPERTY(bool epgRefreshInProgress READ epgRefreshInProgress NOTIFY epgRefreshStateChanged)
    Q_PROPERTY(bool epgCacheBootstrapPending READ epgCacheBootstrapPending NOTIFY epgRefreshStateChanged)

public:
    ~AppController() override;

    AppController(
        Core::SettingsManager *settings,
        Core::DatabaseService *database,
        std::shared_ptr<Core::NetworkAccess> network,
        ProfilesModel *profilesModel,
        ChannelListModel *channelListModel,
        NowNextModel *nowNextModel, // NOLINT(bugprone-easily-swappable-parameters)
        NowNextModel *playbackNowNextModel,
        EpgGridModel *epgGridModel,
        GuideStateModel *guideStateModel,
        ShellController *shellController,
        MultiViewController *multiViewController,
        PlayerController *playerController,
        DvrController *dvrController,
        TimeshiftController *timeshiftController,
        SettingsController *settingsController,
        Core::EpgService *epgService,
        QObject *parent = nullptr);

    CatchupDownloadController *downloadController() const { return m_downloadController; }
    QStringList groupAutoEnableNoticeProfileIds() const;
    Q_INVOKABLE void dismissGroupAutoEnableNotice(const QString &profileId);
    QString statusText() const;
    bool isBusy() const;
    QString activeProfileId() const;
    QString epgLastRefreshText() const;
    bool epgRefreshInProgress() const;
    bool epgCacheBootstrapPending() const;

public slots:
    void initialize();
    void savePlaybackForApplicationExit();
    void loadProfile(const QString &profileId);
    void refreshActiveProfile();
    Q_INVOKABLE void refreshActiveEpg();
    Q_INVOKABLE bool activatePreviousChannel();
    Q_INVOKABLE void playCatchup(const QVariantMap &channel, const QVariantMap &program);
    Q_INVOKABLE void resumeCatchup(const QVariantMap &channel, const QVariantMap &program);
    Q_INVOKABLE void playCatchupAtOffset(const QVariantMap &channel, const QVariantMap &program, double targetSeconds);
    Q_INVOKABLE QVariantMap catchupActionState(const QVariantMap &channel, const QVariantMap &program) const;
    Q_INVOKABLE QVariantMap catchupDownloadActionState(const QVariantMap &channel, const QVariantMap &program) const;
    Q_INVOKABLE QString enqueueCatchupDownload(const QVariantMap &channel, const QVariantMap &program, const QUrl &destination);
    Q_INVOKABLE quint64 requestEpgDetails(const QVariantMap &channel, const QVariantMap &program);
    Q_INVOKABLE bool toggleEpgRecording(const QVariantMap &channel, const QVariantMap &program);
    void dumpDebugReport();
    Q_INVOKABLE QString debugSummary() const;

signals:
    void groupAutoEnableNoticesChanged();
    void statusTextChanged();
    void isBusyChanged();
    void activeProfileIdChanged();
    void epgRefreshStateChanged();
    void profileLoadFinished(const QString &profileId, bool ok);
    void catchupProgressChanged();
    void epgDetailsReady(quint64 requestId, const QVariantMap &program, const QString &error);

private:
    void resolveEpgDetails(const QVariantMap &channel, const QVariantMap &program,
        const std::function<void(QVariantMap, QString)> &completed);
    quint64 m_epgDetailsRequest = 0;
    QString buildDebugSummary() const;
    void setStatusText(const QString &value);
    void setBusy(bool value);
    void setEpgCacheBootstrapPending(bool value);
    void triggerScheduledSourceAutoRefresh();
    void updateRefreshTimer();
    void triggerScheduledEpgRefresh();
    void applyEpgSnapshot(
        quint64 generation,
        const QUuid &profileId,
        const std::shared_ptr<const Core::EpgService::Snapshot> &snapshot,
        const QDateTime &fetchedAt,
        const QString &errorText = {},
        bool scheduleRefresh = true,
        bool setRefreshError = true,
        bool finishRefreshState = true,
        bool cacheBootstrapPending = false);
    void clearEpg(const QUuid &profileId, const QString &errorText = {});
    void updateChannelProgrammeMetadata();
    QList<Core::Channel> guideChannels() const;
    bool syncProfileGroupPreferences(const QUuid &profileId, const QList<Core::Channel> &channels);
    bool syncProfileGroupIds(const QUuid &profileId, QStringList discoveredGroups);
    void beginWatchTrackingForCurrentChannel();
    void flushTrackedWatchSeconds();
    bool restoreStartupCatchup(const Core::Channel &channel);
    std::optional<Core::EpgEntry> catchupProgramAt(const Core::Channel &channel, const QDateTime &time,
        const std::optional<Core::EpgEntry> &validatedProgram = std::nullopt) const;
    void playCatchupAtOffsetInternal(const QVariantMap &channel, const QVariantMap &program,
        double targetSeconds);
    void connectPlaybackSession(PlayerController *controller);
    void recordCatchupProgress(const CatchupProgressSample &sample, PlayerController *source = nullptr);
    void flushCatchupProgress();
    void pruneCatchupProgress();
    void scheduleGuideGridRebuild(bool asyncRequested);
    void rebuildGuideGrid();
    void rebuildGuideGridAsync();
    void activatePrimaryChannel(const Core::Channel &channel);
    void activateChannel(int channelId);
    void prefetchIconsAsync(const QList<Core::Channel> &channels);
    void loadEpgAsync(const Core::ServerProfile &profile, bool forceRefresh = false);

    static QList<Core::ChannelCategory> buildM3uCategories(const QList<Core::Channel> &channels);

    CatchupDownloadController *m_downloadController { nullptr };
    Core::SettingsManager *m_settings;
    Core::DatabaseService *m_database;
    std::shared_ptr<Core::NetworkAccess> m_network;
    ProfilesModel *m_profilesModel;
    ChannelListModel *m_channelListModel;
    NowNextModel *m_nowNextModel;
    NowNextModel *m_playbackNowNextModel;
    EpgGridModel *m_epgGridModel;
    GuideStateModel *m_guideStateModel;
    ShellController *m_shellController;
    MultiViewController *m_multiViewController;
    PlayerController *m_playerController;
    DvrController *m_dvrController;
    TimeshiftController *m_timeshiftController;
    SettingsController *m_settingsController;
    Core::EpgService *m_epgService;
    Core::EpgCacheService m_epgCacheService;
    Core::IconCacheService m_iconCacheService;
    QList<Core::Channel> m_loadedChannels;
    QStringList m_groupAutoEnableNoticeProfileIds;
    QString m_statusText { QStringLiteral("Ready") };
    bool m_isBusy { false };
    QTimer m_epgUiTimer;
    QTimer m_refreshTimer;
    QTimer m_guideRebuildTimer;
    QTimer m_watchStatsFlushTimer;
    QTimer m_catchupProgressFlushTimer;
    QJsonObject m_startupCatchupSession;
    QJsonObject m_observedCatchupSession;
    std::optional<Core::EpgEntry> m_restoredCatchupProgram;
    QString m_restoredCatchupProgramKey;
    QHash<QString, Core::CatchupProgress> m_catchupProgress;
    QSet<QString> m_dirtyCatchupProgress;
    QHash<PlayerController *, QString> m_catchupProgressSessionKeys;
    QHash<PlayerController *, QString> m_catchupProgressProgrammeKeys;
    QTimer m_selectedNowNextRefreshTimer;
    QTimer m_selectedGuideRefreshTimer;
    bool m_guideRebuildAsyncRequested { false };
    QFutureSynchronizer<void> m_backgroundTasks;
    quint64 m_profileLoadGeneration { 0 };
    quint64 m_epgLoadGeneration { 0 };
    Core::EpgCacheService::Cancellation m_epgImportCancellation;
    struct CatchupEpgWindow {
        std::shared_ptr<const Core::EpgService::Snapshot> snapshot;
        QDateTime from, to;
        QList<Core::EpgEntry> entries;
    };
    QHash<QString, CatchupEpgWindow> m_catchupEpgWindows;
    QSet<QString> m_catchupEpgPending;
    QHash<PlayerController *, QPair<QPointer<PlayerController>, CatchupProgressSample>> m_pendingCatchupSamples;
    void requestCatchupPrograms(const Core::Channel &channel, const QDateTime &time);

    quint64 m_programInfoGeneration { 0 };
    quint64 m_catchupPlayGeneration { 0 };
    bool m_programInfoRefreshInFlight { false };
    bool m_programInfoRefreshQueued { false };
    QUuid m_epgLoadedProfileId;
    QDateTime m_epgFetchedAt;
    QDateTime m_epgNextRefreshAt;
    QString m_epgLastRefreshError;
    QHash<int, qint64> m_watchSecondsByChannelId;
    QUuid m_watchTrackingProfileId;
    int m_watchTrackingChannelId { -1 };
    bool m_watchTrackingActive { false };
    QElapsedTimer m_watchTrackingElapsed;
    bool m_manualEpgRefreshPending { false };
    bool m_epgRefreshInProgress { false };
    bool m_epgCacheBootstrapPending { false };
    QString m_lastAutoRefreshProfileId;
    std::optional<Core::Channel> m_pendingSelectedNowNextChannel;
    int m_pendingSelectedGuideChannelId { -1 };
    int m_previousPlaybackChannelId { -1 };
};

} // namespace OKILTV::App
