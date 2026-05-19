#include <sstream>

#define private public
#include "../src/app/appcontroller.h"
#undef private
#include "../src/app/channellistmodel.h"
#define private public
#include "../src/app/dvrcontroller.h"
#undef private
#include "../src/app/epggridmodel.h"
#include "../src/app/guidestatemodel.h"
#define private public
#include "../src/app/multiviewcontroller.h"
#undef private
#define private public
#include "../src/app/nownextmodel.h"
#undef private
#include "../src/app/portableruntimecontroller.h"
#define private public
#include "../src/app/playercontroller.h"
#undef private
#include "../src/app/profilesmodel.h"
#include "../src/app/settingscontroller.h"
#include "../src/app/shellcontroller.h"
#include "../src/app/sourcegroupsmodel.h"
#define private public
#include "../src/app/timeshiftcontroller.h"
#undef private
#include "../src/core/appdatapaths.h"
#include "../src/core/debuglogger.h"
#include "../src/core/database_service.h"
#include "../src/core/epgcache_service.h"
#include "../src/core/epgservice.h"
#include "../src/core/catchupurlresolver.h"
#include "../src/core/networkaccess.h"
#include "../src/core/portablebootstrap.h"
#include "../src/core/settingsmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTcpSocket>
#include <QThread>
#include <QtTest>

#include <cmath>
#include <algorithm>
#include <cerrno>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>

#if !defined(Q_OS_WIN)
#include <signal.h>
#endif

using namespace OKILTV::App;
using namespace OKILTV::Core;

namespace {

QByteArray xmltvPayload(const QString &title)
{
    return QStringLiteral(
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
               "<tv>"
               "  <programme start=\"20260317180000 +0000\" stop=\"20260317190000 +0000\" channel=\"channel.one\">"
               "    <title>%1</title>"
               "    <desc>%2 description</desc>"
               "  </programme>"
               "</tv>")
        .arg(title, title)
        .toUtf8();
}

std::unique_ptr<QProcess> startTermIgnoringProcess()
{
#if defined(Q_OS_WIN)
    return {};
#else
    auto process = std::make_unique<QProcess>();
    process->setProgram(QStringLiteral("/bin/sh"));
    process->setArguments({ QStringLiteral("-c"), QStringLiteral("trap '' TERM; while :; do sleep 1; done") });
    process->start();
    return process;
#endif
}

bool processIsAlive(const qint64 pid)
{
#if defined(Q_OS_WIN)
    Q_UNUSED(pid);
    return false;
#else
    if (pid <= 0) {
        return false;
    }

    const auto result = ::kill(static_cast<pid_t>(pid), 0);
    return result == 0 || errno != ESRCH;
#endif
}

bool writeExecutableTextFile(const QString &path, const QString &contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    if (file.write(contents.toUtf8()) < 0) {
        return false;
    }
    file.close();
#if defined(Q_OS_WIN)
    return true;
#else
    return QFile::setPermissions(
        path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
            | QFileDevice::ReadGroup | QFileDevice::ExeGroup
            | QFileDevice::ReadOther | QFileDevice::ExeOther);
#endif
}

class ScopedPathOverride
{
public:
    explicit ScopedPathOverride(const QString &prependPath)
        : m_hadPath(qEnvironmentVariableIsSet("PATH"))
        , m_originalPath(qgetenv("PATH"))
    {
        auto updated = prependPath.toUtf8();
        if (m_hadPath && !m_originalPath.isEmpty()) {
            updated += QByteArray(1, static_cast<char>(QDir::listSeparator().toLatin1()));
            updated += m_originalPath;
        }
        qputenv("PATH", updated);
    }

    ~ScopedPathOverride()
    {
        if (m_hadPath) {
            qputenv("PATH", m_originalPath);
        } else {
            qunsetenv("PATH");
        }
    }

private:
    bool m_hadPath { false };
    QByteArray m_originalPath;
};

class MockNetworkAccess final : public NetworkAccess
{
public:
    struct Response
    {
        QByteArray payload;
        QString errorText;
        int delayMs { 0 };
    };

    void setResponse(const QUrl &url, Response response)
    {
        QMutexLocker locker(&m_mutex);
        m_responses.insert(url.toString(), std::move(response));
    }

    QByteArray get(const QUrl &url) const override
    {
        Response response;
        {
            QMutexLocker locker(&m_mutex);
            response = m_responses.value(url.toString());
            m_calls[url.toString()] += 1;
        }

        if (response.delayMs > 0) {
            QThread::msleep(static_cast<unsigned long>(response.delayMs));
        }

        if (!response.errorText.isEmpty()) {
            throw std::runtime_error(response.errorText.toStdString());
        }

        return response.payload;
    }

    int callCount(const QUrl &url) const
    {
        QMutexLocker locker(&m_mutex);
        return m_calls.value(url.toString());
    }

private:
    mutable QMutex m_mutex;
    mutable QHash<QString, int> m_calls;
    QHash<QString, Response> m_responses;
};

struct StartupHarness
{
    QTemporaryDir tempDir;
    QByteArray previousAppData;
    QString settingsPath;
    QString playlistPath;

    std::unique_ptr<SettingsManager> settings;
    std::unique_ptr<DatabaseService> database;
    std::shared_ptr<NetworkAccess> network;
    std::unique_ptr<EpgService> epgService;
    std::unique_ptr<ProfilesModel> profilesModel;
    std::unique_ptr<ChannelListModel> channelListModel;
    std::unique_ptr<NowNextModel> nowNextModel;
    std::unique_ptr<NowNextModel> playbackNowNextModel;
    std::unique_ptr<EpgGridModel> epgGridModel;
    std::unique_ptr<GuideStateModel> guideStateModel;
    std::unique_ptr<ShellController> shellController;
    std::unique_ptr<PlayerController> playerController;
    std::unique_ptr<MultiViewController> multiViewController;
    std::unique_ptr<DvrController> dvrController;
    std::unique_ptr<TimeshiftController> timeshiftController;
    std::unique_ptr<SettingsController> settingsController;
    std::unique_ptr<AppController> appController;

    ~StartupHarness()
    {
        if (previousAppData.isEmpty()) {
            qunsetenv("APPDATA");
        } else {
            qputenv("APPDATA", previousAppData);
        }
    }

    bool initialize(
        const std::optional<int> &lastWatchedChannelId,
        std::shared_ptr<NetworkAccess> customNetwork = {},
        const QString &xmltvUrl = {})
    {
        if (!tempDir.isValid()) {
            return false;
        }

        previousAppData = qgetenv("APPDATA");
        qputenv("APPDATA", tempDir.path().toUtf8());

        playlistPath = tempDir.filePath(QStringLiteral("playlist.m3u"));
        QFile playlist(playlistPath);
        if (!playlist.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }

        playlist.write(
            "#EXTM3U\n"
            "#EXTINF:-1 tvg-id=\"channel.one\" tvg-name=\"Channel One\" group-title=\"News\",Channel One\n"
            "http://127.0.0.1/channel-one\n"
            "#EXTINF:-1 tvg-id=\"channel.two\" tvg-name=\"Channel Two\" group-title=\"Sports\",Channel Two\n"
            "http://127.0.0.1/channel-two\n");
        playlist.close();

        settingsPath = tempDir.filePath(QStringLiteral("settings.json"));
        settings = std::make_unique<SettingsManager>(settingsPath);
        settings->load();

        ServerProfile profile;
        profile.name = QStringLiteral("Local Playlist");
        profile.type = ProfileType::M3UFile;
        profile.m3uFilePath = playlistPath;
        profile.xmltvUrl = xmltvUrl;
        settings->current().profiles = { profile };
        settings->setActiveProfileId(profile.id);
        if (lastWatchedChannelId.has_value()) {
            settings->current().lastWatchedChannelId.insert(guidToString(profile.id), lastWatchedChannelId.value());
        }
        settings->save();

        database = std::make_unique<DatabaseService>();
        network = customNetwork ? std::move(customNetwork) : makeDefaultNetworkAccess();
        epgService = std::make_unique<EpgService>();
        profilesModel = std::make_unique<ProfilesModel>(settings.get());
        channelListModel = std::make_unique<ChannelListModel>(settings.get());
        nowNextModel = std::make_unique<NowNextModel>(epgService.get());
        playbackNowNextModel = std::make_unique<NowNextModel>(epgService.get());
        epgGridModel = std::make_unique<EpgGridModel>(epgService.get());
        guideStateModel = std::make_unique<GuideStateModel>(epgService.get(), settings.get());
        shellController = std::make_unique<ShellController>(settings.get());
        playerController = std::make_unique<PlayerController>();
        multiViewController = std::make_unique<MultiViewController>(settings.get(), channelListModel.get(), playerController.get());
        dvrController = std::make_unique<DvrController>(settings.get(), playerController.get());
        timeshiftController = std::make_unique<TimeshiftController>(
            settings.get(),
            playerController.get(),
            dvrController.get(),
            multiViewController.get());
        playerController->setTimeshiftController(timeshiftController.get());
        settingsController = std::make_unique<SettingsController>(
            settings.get(),
            playerController.get(),
            multiViewController.get(),
            profilesModel.get());
        appController = std::make_unique<AppController>(
            settings.get(),
            database.get(),
            network,
            profilesModel.get(),
            channelListModel.get(),
            nowNextModel.get(),
            playbackNowNextModel.get(),
            epgGridModel.get(),
            guideStateModel.get(),
            shellController.get(),
            multiViewController.get(),
            playerController.get(),
            dvrController.get(),
            timeshiftController.get(),
            settingsController.get(),
            epgService.get());
        return true;
    }

    QUuid activeProfileId() const
    {
        return settings->current().profiles.first().id;
    }
};

class ScopedRuntimeContext
{
public:
    ScopedRuntimeContext()
        : previous(AppDataPaths::runtimeContext())
    {
        AppDataPaths::resetRuntimeForTests();
    }

    ~ScopedRuntimeContext()
    {
        AppDataPaths::initializeRuntime(previous);
    }

private:
    RuntimeContext previous;
};

} // namespace

class AppModelTests final : public QObject
{
    Q_OBJECT

private slots:
    void shellControllerRestoreLastViewClearsOverlayState();
    void shellControllerOpenOverlayPreservesOverlayStateExclusive();
    void appControllerKeepsSettingsOverlayOpenDuringProfileLoad();
    void appControllerLoadProfileDoesNotAutoActivateInactiveProfile();
    void playerControllerMuteToggleRestoresPreviousVolume();
    void playerControllerIsPlayingTracksBackendPauseState();
    void playerControllerShowsLoadingIndicatorAfterTuneDelay();
    void playerControllerBufferingTracksBackendState();
    void playerControllerInitialTuneErrorStartsReconnectWithoutImmediateFailure();
    void playerControllerInitialTunePlaybackEndedStartsReconnectWithoutImmediateFailure();
    void playerControllerInitialTuneReconnectSuccessClearsFailure();
    void playerControllerInitialTuneRetryExhaustionEmitsFinalFailure();
    void playerControllerReconnectStaysBufferingUntilRecovered();
    void playerControllerReconnectTimesOutAfterPlaybackEnds();
    void playerControllerVideoReconfiguredIsNonFatal();
    void playerControllerMetadataRefreshDoesNotEmitPlaybackActivation();
    void playerControllerCatchupModeRoundTripsToLive();
    void playerControllerPauseDuringCatchupBypassesTimeshiftStartup();
    void playerControllerInitialCatchupTuneRecoveryTargetsCatchupUrl();
    void playerControllerCatchupBypassesLiveStartupBufferGate();
    void playerControllerCatchupReconnectTargetsCatchupUrl();
    void playerControllerCatchupDebugSnapshotUsesEffectiveBufferMetric();
    void playerControllerCatchupReconnectStabilizationIgnoresCacheDurationOutliers();
    void playerControllerXtreamCatchupSeekRegeneratesUrlTransparently();
    void playerControllerCatchupRegeneratedSeekStopsReconnectWithoutStrictStartupRestore();
    void playerControllerSharedPrimarySignalsAndRetuneStayOnActivePlayer();
    void mpvPlayerDemuxerMaxBytesMapping();
    void mpvPlayerCacheWindowSecondsMapping();
    void mpvPlayerSteadyStateCacheBandMapping();
    void playerControllerStartupBufferFallbackTimeoutMapping();
    void playerControllerAdaptiveSteadyStateMaxBytesMapping();
    void playerControllerReconnectDepletionTimeoutFollowsWaitForDataRule();
    void playerControllerPreemptiveReconnectHeuristic();
    void playerControllerDeadStreamDisconnectHeuristic();
    void playerControllerDebugHelpersExtractStreamFields();
    void playerControllerDebugBufferDurationFormatting();
    void playerControllerDebugFramerateFormatting();
    void playerControllerDebugBitrateFormatting();
    void playerControllerDebugTimestampFormat();
    void appControllerTracksWatchTimeAndFlushesOnPlaybackBoundaries();
    void appControllerFlushTrackedWatchSecondsAllowsChannelIdZero();
    void settingsControllerTracksDirtyStateForRegularSettings();
    void settingsControllerDisablesFfmpegDependentOptionsWhenToolsUnavailable();
    void settingsControllerAllowsFfmpegDependentOptionsWhenToolsAvailable();
    void timeshiftControllerServesPlaybackOverLocalHttp();
    void timeshiftControllerStartupCleanupOnlyRemovesManagedSessionDirectories();
    void timeshiftControllerCurrentPlaybackEpochStaysOnAttachedStreamWhileDelayedLoadIsPending();
    void timeshiftUserStopRequestKillsIngestImmediately();
    void timeshiftUserChannelSwitchRequestKillsIngestImmediately();
    void multiviewPictureInPictureEmptyOpenAssignsFocusedSecondaryAndClosesOnToggle();
    void multiviewControllerOpensPictureInPictureGridAndSwapsChannels();
    void appControllerRoutesActivationToFocusedMultiviewTile();
    void appControllerSameChannelActivationSkipsRetuneWhileActiveOrInFlight();
    void appControllerSameChannelActivationRetunesLiveWhenCatchupActive();
    void appControllerSourceActivationStopsCatchupBeforeCrossProfileLoad();
    void appControllerPlayCatchupResolvesLegacyM3uTimeshiftTemplate();
    void appControllerPlayCatchupGuideUtcPayloadResolvesExpectedEpochUrl();
    void appControllerPlayCatchupGuideOffsetPayloadResolvesExpectedEpochUrl();
    void appControllerPlayCatchupXtreamPreResolvesRedirectUrl();
    void appControllerPlayCatchupXtreamRedirectResolutionDoesNotBlockUiThread();
    void appControllerPlayCatchupXtreamRedirectFailureFallsBackToOriginalUrl();
    void appControllerPlayCatchupRejectsUnresolvedTemplate();
    void appControllerPlayCatchupRejectsFutureProgram();
    void appControllerPlayCatchupRejectsProgrammeChannelMismatch();
    void multiviewExitPromotesFocusedSecondaryToPrimary();
    void multiviewGridToggleExitWithoutRetainStillPerformsFullCleanup();
    void multiviewGridToggleWithRetainSoftPromotesAndKeepsSecondaryStreams();
    void multiviewGridStopFocusedSecondaryKeepsFocusWithoutRetain();
    void multiviewGridStopFocusedSecondaryKeepsFocusWithRetain();
    void multiviewGridStopLastRemainingTileReturnsToDefaultPlaybackState();
    void multiviewRetainedSelectionReopenRestoresWarmSecondarySlots();
    void multiviewRetainedSelectionStopReopensGridAndStopsPromotedTile();
    void multiviewRetainedSelectionKeepsHiddenTilesAudioWarmAndSchedulesDeferredRefresh();
    void multiviewRetainedSelectionClearsOnFullPromoteShortcut();
    void multiviewRetainedSelectionClearsOnDegradeToOff();
    void multiviewRetainedSelectionClearsOnProfileChange();
    void multiviewExitAfterSwapKeepsPromotedPrimaryWithoutRetune();
    void multiviewExitAfterSwapDefersDetachedPlayerCleanup();
    void multiviewRetiredSecondarySignalsDoNotCorruptReusedSlot();
    void multiviewPrimaryAssignmentRetunesSharedPrimaryInPlaceAfterSwap();
    void dvrControllerToggleScheduleAndExitGuard();
    void dvrControllerFinalizeActiveWindowSchedulesRestart();
    void dvrControllerRestartStateMaintainedAndClearedByWindowState();
    void dvrControllerWindowsCrashWithoutFinishedDefersFinalize();
    void dvrControllerRemuxDeletesTempWhenDurationMatchesRegardlessOfExitCode();
    void dvrControllerRemuxKeepsTempWhenDurationMismatched();
    void portableRuntimeControllerTracksPortableOverrideWithoutDirtyingSettings();
    void channelListModelSupportsAutoFavouritesAndGroupPrefs();
    void channelListModelHidesDeselectedGroupsUntilExplicitGroupIsChosen();
    void channelListModelKeyboardSelectionHelpersWrapAndJump();
    void channelListModelSelectByIdNoOpWhenUnchanged();
    void channelListModelActivatesByDisplayNumber();
    void channelListModelExposesCurrentProgramRoles();
    void channelListModelExposesDvrRecordingRole();
    void sourceGroupsModelAppliesSelectionThresholdAndPersistsReorder();
    void sourceGroupsModelReorderVisibleGroupsAppendsHiddenInRelativeOrder();
    void sourceGroupsModelClearsStaleRowsForInvalidProfile();
    void epgGridModelInitializesTimeWindow();
    void epgGridModelSelectionUpdatesOnlyAffectedRows();
    void epgGridModelNavigationHelpersFollowTimeAndBounds();
    void epgGridModelUsesConfiguredPastAndFutureWindow();
    void epgGridModelUsesConfiguredLookAheadWindow();
    void epgGridModelStreamsProgramsForViewport();
    void appControllerGuideRebuildUsesConfiguredPastAndFutureRanges();
    void guideGridFilteringStaysIndependentFromLiveSearch();
    void startupResumeLastWatchedChannel();
    void startupWithoutSavedChannelStaysBlack();
    void startupWithMissingSavedChannelStaysBlack();
    void nowNextModelRefreshIsAsyncAndDeduplicatesUpcoming();
    void nowNextModelCoalescesRefreshRequestsToLatestSelection();
    void nowNextModelExposesLoadingStateDuringRefresh();
    void guideStateModelSelectChannelLoadsProgramsAsync();
    void guideStateModelSelectChannelNoOpWhenUnchanged();
    void guideStateModelRefreshesProgramsWhenSelectedChannelTvgIdChanges();
    void guideStateModelPreferredProgramStartSurvivesAsyncReload();
    void epgMissingCacheFetchesFromSource();
    void epgFreshCacheSkipsNetworkUntilDue();
    void manualEpgRefreshBypassesFreshCache();
    void epgStaleCacheLoadsThenRefreshesInBackground();
    void epgRefreshFailureKeepsStaleCacheLoaded();
    void xtreamProfileRefreshKeepsStoredTimezoneWhenResponseMissingTimezone();
    void scheduledSourceAutoRefreshTriggersAtExactIntervalBoundary();
    void sourceRefreshFailureWithCachedFallbackKeepsPreviousLastRefreshed();
    void profileRefreshPrunesRemovedChannelsFromDatabase();
};

void AppModelTests::shellControllerRestoreLastViewClearsOverlayState()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    ShellController shell(&settings);
    shell.openOverlay(QStringLiteral("guide"));
    shell.setOverlaysVisible(true);
    QCOMPARE(shell.activeOverlay(), QStringLiteral("guide"));
    QVERIFY(shell.overlaysVisible());

    shell.restoreLastView();
    QCOMPARE(shell.activeOverlay(), QStringLiteral("none"));
    QVERIFY(!shell.overlaysVisible());
}

void AppModelTests::shellControllerOpenOverlayPreservesOverlayStateExclusive()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    ShellController shell(&settings);
    shell.openOverlay(QStringLiteral("guide"));
    QCOMPARE(shell.activeOverlay(), QStringLiteral("guide"));
    QVERIFY(shell.overlaysVisible());

    shell.openOverlay(QStringLiteral("settings"));
    QCOMPARE(shell.activeOverlay(), QStringLiteral("settings"));
    QVERIFY(shell.overlaysVisible());
}

void AppModelTests::appControllerKeepsSettingsOverlayOpenDuringProfileLoad()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));

    harness.shellController->openOverlay(QStringLiteral("settings"), QStringLiteral("sources"));
    QCOMPARE(harness.shellController->activeOverlay(), QStringLiteral("settings"));
    QCOMPARE(harness.shellController->overlaySection(), QStringLiteral("sources"));

    QSignalSpy profileLoadSpy(harness.appController.get(), &AppController::profileLoadFinished);
    harness.appController->loadProfile(guidToString(harness.activeProfileId()));
    QTRY_VERIFY_WITH_TIMEOUT(profileLoadSpy.count() > 0, 8000);

    QCOMPARE(harness.shellController->activeOverlay(), QStringLiteral("settings"));
    QCOMPARE(harness.shellController->overlaySection(), QStringLiteral("sources"));
}

void AppModelTests::appControllerLoadProfileDoesNotAutoActivateInactiveProfile()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));

    const auto activeProfile = guidToString(harness.activeProfileId());
    QSignalSpy profileLoadSpy(harness.appController.get(), &AppController::profileLoadFinished);

    harness.appController->loadProfile(activeProfile);
    QTRY_VERIFY_WITH_TIMEOUT(profileLoadSpy.count() > 0, 8000);
    QCOMPARE(harness.appController->activeProfileId(), activeProfile);
    QCOMPARE(harness.channelListModel->activeProfileId(), activeProfile);

    const auto secondPlaylistPath = harness.tempDir.filePath(QStringLiteral("playlist-second.m3u"));
    QFile secondPlaylist(secondPlaylistPath);
    QVERIFY(secondPlaylist.open(QIODevice::WriteOnly | QIODevice::Truncate));
    secondPlaylist.write(
        "#EXTM3U\n"
        "#EXTINF:-1 tvg-id=\"channel.three\" tvg-name=\"Channel Three\" group-title=\"News\",Channel Three\n"
        "http://127.0.0.1/channel-three\n");
    secondPlaylist.close();

    const auto secondProfileId = harness.profilesModel->addM3uFileProfile(
        QStringLiteral("Second Playlist"),
        secondPlaylistPath,
        QString {});
    QVERIFY(!secondProfileId.isEmpty());

    profileLoadSpy.clear();
    harness.appController->loadProfile(secondProfileId);
    QTRY_VERIFY_WITH_TIMEOUT(profileLoadSpy.count() > 0, 8000);

    QCOMPARE(harness.appController->activeProfileId(), activeProfile);
    QCOMPARE(harness.profilesModel->activeProfileId(), activeProfile);
    QCOMPARE(harness.channelListModel->activeProfileId(), activeProfile);
}

void AppModelTests::playerControllerMuteToggleRestoresPreviousVolume()
{
    PlayerController playerController;

    playerController.setVolume(49);
    QCOMPARE(playerController.volume(), 49.0);
    QVERIFY(!playerController.muted());

    playerController.toggleMute();
    QCOMPARE(playerController.volume(), 0.0);
    QVERIFY(playerController.muted());

    playerController.toggleMute();
    QCOMPARE(playerController.volume(), 49.0);
    QVERIFY(!playerController.muted());

    playerController.setVolume(137);
    QCOMPARE(playerController.volume(), 100.0);
    QVERIFY(!playerController.muted());

    playerController.setVolume(30);
    playerController.setVolume(0);
    QVERIFY(playerController.muted());

    playerController.toggleMute();
    QCOMPARE(playerController.volume(), 30.0);
    QVERIFY(!playerController.muted());
}

void AppModelTests::playerControllerIsPlayingTracksBackendPauseState()
{
    PlayerController playerController;
    QSignalSpy isPlayingSpy(&playerController, &PlayerController::isPlayingChanged);

    Channel channel;
    channel.id = 11;
    channel.name = QStringLiteral("Channel Eleven");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/channel-11");

    playerController.playChannel(channel);
    QVERIFY(!playerController.isPlaying());

    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(!playerController.isPlaying());

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));
    QVERIFY(playerController.isPlaying());

    playerController.togglePause();
    QVERIFY(isPlayingSpy.count() >= 1);

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, true)));
    QVERIFY(!playerController.isPlaying());

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));
    QVERIFY(playerController.isPlaying());

    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "playbackEnded", Qt::DirectConnection));
    QVERIFY(!playerController.isPlaying());
}

void AppModelTests::playerControllerShowsLoadingIndicatorAfterTuneDelay()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 111;
    channel.name = QStringLiteral("Channel One Eleven");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/channel-111");

    playerController.playChannel(channel);
    QVERIFY(!playerController.isLoading());

    QTRY_VERIFY_WITH_TIMEOUT(
        playerController.isLoading() || playerController.channelLoadFailed() || playerController.isPlaying(),
        2500);
    if (playerController.channelLoadFailed()) {
        QSKIP("libmpv failed to load stream in test environment before delayed loading indicator could be observed.");
    }
    if (playerController.isPlaying()) {
        QSKIP("Playback started before 1.5s delay; loading indicator is intentionally suppressed.");
    }
    QVERIFY(playerController.isLoading());

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));
    QVERIFY(playerController.isPlaying());
    QVERIFY(!playerController.isLoading());
}

void AppModelTests::playerControllerBufferingTracksBackendState()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 12;
    channel.name = QStringLiteral("Channel Twelve");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/channel-12");

    playerController.playChannel(channel);
    QVERIFY(!playerController.isBuffering());

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "bufferingStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, true)));
    QVERIFY(playerController.isBuffering());

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "bufferingStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));
    QVERIFY(!playerController.isBuffering());

    playerController.stop();
    QVERIFY(!playerController.isBuffering());
}

void AppModelTests::playerControllerInitialTuneErrorStartsReconnectWithoutImmediateFailure()
{
    PlayerController playerController;
    QSignalSpy playbackErrorSpy(&playerController, &PlayerController::playbackError);
    playerController.applySettings(
        QString(),
        {},
        2.4,
        false,
        3.0,
        QString());
    QStringList reconnectStartLogs;
    const auto subscriptionId = DebugLogger::instance().subscribe([&reconnectStartLogs](const DebugLogger::Entry &entry) {
        if (entry.category == QStringLiteral("player")
            && entry.message.contains(QStringLiteral("Reconnect loop started:"))) {
            reconnectStartLogs.push_back(entry.message);
        }
    });

    Channel channel;
    channel.id = 115;
    channel.name = QStringLiteral("Initial Error Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/channel-115");

    playerController.playChannel(channel);
    QVERIFY(playerController.m_resumePlaybackAfterLoad);

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "errorOccurred",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("simulated-startup-error"))));

    QVERIFY(!playerController.m_resumePlaybackAfterLoad);
    QVERIFY(playerController.m_reconnectActive);
    QVERIFY(playerController.m_reconnectAttemptInFlight);
    QVERIFY(!playerController.channelLoadFailed());
    QTRY_VERIFY_WITH_TIMEOUT(!reconnectStartLogs.isEmpty(), 1000);
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("wait-for-data=2.4s")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("attempt-timeout=2400ms")));
    QCOMPARE(playbackErrorSpy.count(), 0);
    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::playerControllerInitialTunePlaybackEndedStartsReconnectWithoutImmediateFailure()
{
    PlayerController playerController;
    QSignalSpy playbackErrorSpy(&playerController, &PlayerController::playbackError);

    Channel channel;
    channel.id = 116;
    channel.name = QStringLiteral("Initial End Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/channel-116");

    playerController.playChannel(channel);
    QVERIFY(playerController.m_resumePlaybackAfterLoad);

    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "playbackEnded", Qt::DirectConnection));

    QVERIFY(!playerController.m_resumePlaybackAfterLoad);
    QVERIFY(playerController.m_reconnectActive);
    QVERIFY(playerController.m_reconnectAttemptInFlight);
    QVERIFY(!playerController.channelLoadFailed());
    QCOMPARE(playbackErrorSpy.count(), 0);
}

void AppModelTests::playerControllerInitialTuneReconnectSuccessClearsFailure()
{
    PlayerController playerController;
    QSignalSpy playbackErrorSpy(&playerController, &PlayerController::playbackError);

    Channel channel;
    channel.id = 117;
    channel.name = QStringLiteral("Initial Recovery Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/channel-117");

    playerController.playChannel(channel);
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "errorOccurred",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("simulated-startup-error"))));
    QVERIFY(playerController.m_reconnectActive);

    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    QVERIFY(!playerController.channelLoadFailed());
    QVERIFY(playerController.isPlaying());
    QCOMPARE(playbackErrorSpy.count(), 0);
}

void AppModelTests::playerControllerInitialTuneRetryExhaustionEmitsFinalFailure()
{
    PlayerController playerController;
    QSignalSpy playbackErrorSpy(&playerController, &PlayerController::playbackError);
    playerController.applySettings(
        QString(),
        {},
        0.1,
        false,
        3.0,
        QString());

    Channel channel;
    channel.id = 118;
    channel.name = QStringLiteral("Initial Exhaustion Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/channel-118");

    playerController.playChannel(channel);
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "errorOccurred",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("simulated-startup-error"))));

    QVERIFY(playerController.m_reconnectActive);
    QCOMPARE(playbackErrorSpy.count(), 0);

    QTRY_VERIFY_WITH_TIMEOUT(playerController.channelLoadFailed(), 10000);
    QVERIFY(playbackErrorSpy.count() >= 1);
    QVERIFY(playbackErrorSpy.last().first().toString().contains(QStringLiteral("couldn't be loaded")));
}

void AppModelTests::playerControllerReconnectStaysBufferingUntilRecovered()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 112;
    channel.name = QStringLiteral("Reconnect Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/channel-112");

    playerController.playChannel(channel);
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));
    QVERIFY(playerController.isPlaying());

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "bufferingStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, true)));
    QVERIFY(playerController.isBuffering());

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "bufferingStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));
    QVERIFY(!playerController.channelLoadFailed());

    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "bufferingStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));
    QTRY_VERIFY_WITH_TIMEOUT(!playerController.isBuffering(), 2000);
    QVERIFY(!playerController.channelLoadFailed());
}

void AppModelTests::playerControllerReconnectTimesOutAfterPlaybackEnds()
{
    PlayerController playerController;
    QSignalSpy playbackErrorSpy(&playerController, &PlayerController::playbackError);
    playerController.applySettings(
        QString(),
        {},
        0.1,
        false,
        3.0,
        QString());

    Channel channel;
    channel.id = 113;
    channel.name = QStringLiteral("Reconnect Timeout Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/channel-113");

    playerController.playChannel(channel);
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));
    QVERIFY(playerController.isPlaying());

    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "playbackEnded", Qt::DirectConnection));
    QVERIFY(playerController.isBuffering());

    QTRY_VERIFY_WITH_TIMEOUT(playerController.channelLoadFailed(), 10000);
    QVERIFY(playbackErrorSpy.count() >= 1);
    QVERIFY(playbackErrorSpy.last().first().toString().contains(QStringLiteral("couldn't be loaded")));
}

void AppModelTests::playerControllerVideoReconfiguredIsNonFatal()
{
    PlayerController playerController;
    QSignalSpy playbackErrorSpy(&playerController, &PlayerController::playbackError);

    Channel channel;
    channel.id = 114;
    channel.name = QStringLiteral("Reconfig Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/channel-114");

    playerController.playChannel(channel);
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));
    QVERIFY(playerController.isPlaying());

    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "videoReconfigured", Qt::DirectConnection));
    QVERIFY(playerController.isPlaying());
    QVERIFY(!playerController.channelLoadFailed());
    QCOMPARE(playerController.currentChannel().value(QStringLiteral("id")).toInt(), channel.id);
    QCOMPARE(playbackErrorSpy.count(), 0);
}

void AppModelTests::playerControllerMetadataRefreshDoesNotEmitPlaybackActivation()
{
    PlayerController playerController;
    QSignalSpy activationSpy(&playerController, &PlayerController::playbackChannelActivated);
    QSignalSpy currentChannelSpy(&playerController, &PlayerController::currentChannelChanged);

    Channel channel;
    channel.id = 7;
    channel.name = QStringLiteral("Original");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/original");

    playerController.playChannel(channel);
    QCOMPARE(activationSpy.count(), 1);
    QCOMPARE(currentChannelSpy.count(), 1);

    channel.cachedIconPath = QStringLiteral("/tmp/icon.png");
    channel.name = QStringLiteral("Updated");
    playerController.refreshCurrentChannelMetadata(channel);

    QCOMPARE(activationSpy.count(), 1);
    QCOMPARE(currentChannelSpy.count(), 2);
    QCOMPARE(playerController.currentChannel().value(QStringLiteral("cachedIconPath")).toString(), QStringLiteral("/tmp/icon.png"));
    QCOMPARE(playerController.nowPlayingName(), QStringLiteral("Updated"));
}

void AppModelTests::playerControllerCatchupModeRoundTripsToLive()
{
    PlayerController playerController;
    QStringList playRequestLogs;
    const auto subscriptionId = DebugLogger::instance().subscribe([&playRequestLogs](const DebugLogger::Entry &entry) {
        if (entry.category == QStringLiteral("player")
            && entry.message.startsWith(QStringLiteral("Play requested:"))) {
            playRequestLogs.push_back(entry.message);
        }
    });

    Channel channel;
    channel.id = 70;
    channel.name = QStringLiteral("Archive Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live");

    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup"),
        QStringLiteral("18:00 - 18:30  Archive Show"));
    QCOMPARE(playerController.playbackMode(), QStringLiteral("catchup"));
    QCOMPARE(playerController.catchupProgramLabel(), QStringLiteral("18:00 - 18:30  Archive Show"));
    QCOMPARE(playerController.currentChannel().value(QStringLiteral("id")).toInt(), channel.id);
    QCOMPARE(playerController.currentPlaybackUrl(), QStringLiteral("http://127.0.0.1/catchup"));

    QVERIFY(QMetaObject::invokeMethod(&playerController, "returnToLiveFromCatchup", Qt::DirectConnection));
    QCOMPARE(playerController.playbackMode(), QStringLiteral("live"));
    QCOMPARE(playerController.catchupProgramLabel(), QStringLiteral(""));
    QCOMPARE(playerController.currentPlaybackUrl(), QStringLiteral("http://127.0.0.1/live"));
    auto livePlayLog = std::find_if(playRequestLogs.cbegin(), playRequestLogs.cend(), [](const QString &line) {
        return line.startsWith(QStringLiteral("Play requested: http://127.0.0.1/live"));
    });
    QVERIFY(livePlayLog != playRequestLogs.cend());
    QVERIFY(!livePlayLog->contains(QStringLiteral("cache-secs=120")));
    QVERIFY(!livePlayLog->contains(QStringLiteral("demuxer-hysteresis-secs=115")));
    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::playerControllerPauseDuringCatchupBypassesTimeshiftStartup()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    settings.current().timeshiftEnabled = true;

    PlayerController playerController;
    MultiViewController multiViewController(&settings, nullptr, &playerController);
    DvrController dvrController(&settings, &playerController);
    TimeshiftController timeshiftController(&settings, &playerController, &dvrController, &multiViewController);
    playerController.setTimeshiftController(&timeshiftController);

    Channel channel;
    channel.id = 71;
    channel.name = QStringLiteral("Archive Pause Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live71");

    playerController.playCatchupChannel(channel, QStringLiteral("http://127.0.0.1/catchup71"), QStringLiteral("Past Show"));
    playerController.togglePause();

    QVERIFY(!timeshiftController.isActive());
    QVERIFY(!timeshiftController.isPreparing());
}

void AppModelTests::playerControllerInitialCatchupTuneRecoveryTargetsCatchupUrl()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 119;
    channel.name = QStringLiteral("Initial Archive Recovery Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live119");

    QStringList reconnectStartLogs;
    const auto subscriptionId = DebugLogger::instance().subscribe([&reconnectStartLogs](const DebugLogger::Entry &entry) {
        if (entry.category == QStringLiteral("player")
            && entry.message.contains(QStringLiteral("Reconnect loop started:"))) {
            reconnectStartLogs.push_back(entry.message);
        }
    });

    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup119"),
        QStringLiteral("Past Show"));
    QVERIFY(!playerController.m_resumePlaybackAfterLoad);

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "errorOccurred",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("simulated-initial-archive-error"))));

    QVERIFY(playerController.m_reconnectActive);
    QVERIFY(!playerController.channelLoadFailed());
    QTRY_VERIFY_WITH_TIMEOUT(!reconnectStartLogs.isEmpty(), 1000);
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("http://127.0.0.1/catchup119")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("force-seekable=yes")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("cache-secs=120")));
    QCOMPARE(playerController.playbackMode(), QStringLiteral("catchup"));
    QCOMPARE(playerController.currentPlaybackUrl(), QStringLiteral("http://127.0.0.1/catchup119"));

    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::playerControllerCatchupBypassesLiveStartupBufferGate()
{
    PlayerController playerController;
    playerController.applySettings(
        QString(),
        {},
        5.0,
        false,
        2.0,
        QString());

    QStringList startupBufferLogs;
    const auto subscriptionId = DebugLogger::instance().subscribe([&startupBufferLogs](const DebugLogger::Entry &entry) {
        if (entry.category == QStringLiteral("player")
            && entry.message.contains(QStringLiteral("Startup buffer"))) {
            startupBufferLogs.push_back(entry.message);
        }
    });

    Channel channel;
    channel.id = 120;
    channel.name = QStringLiteral("Archive Startup Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live120");

    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup120"),
        QStringLiteral("Past Show"));

    QVERIFY(!playerController.m_resumePlaybackAfterLoad);
    QVERIFY(!playerController.m_startupBufferFallbackTimer.isActive());
    QVERIFY(!playerController.m_startupBufferProbeTimer.isActive());

    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(!playerController.m_resumePlaybackAfterLoad);
    QVERIFY(!playerController.m_startupBufferFallbackTimer.isActive());
    QVERIFY(!playerController.m_startupBufferProbeTimer.isActive());
    QVERIFY(startupBufferLogs.isEmpty());

    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::playerControllerCatchupReconnectTargetsCatchupUrl()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 72;
    channel.name = QStringLiteral("Archive Reconnect Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live72");

    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup72"),
        QStringLiteral("Past Show"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    QStringList reconnectStartLogs;
    QStringList playRequestLogs;
    QStringList catchupSeekLogs;
    const auto subscriptionId = DebugLogger::instance().subscribe([&reconnectStartLogs, &playRequestLogs, &catchupSeekLogs](const DebugLogger::Entry &entry) {
        if (entry.category == QStringLiteral("player")
            && entry.message.contains(QStringLiteral("Reconnect loop started:"))) {
            reconnectStartLogs.push_back(entry.message);
        }
        if (entry.category == QStringLiteral("player")
            && entry.message.startsWith(QStringLiteral("Play requested:"))) {
            playRequestLogs.push_back(entry.message);
        }
        if (entry.category == QStringLiteral("player")
            && entry.message.startsWith(QStringLiteral("Catch-up timeline seek:"))) {
            catchupSeekLogs.push_back(entry.message);
        }
    });

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "errorOccurred",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("simulated-archive-error"))));

    QTRY_VERIFY_WITH_TIMEOUT(!reconnectStartLogs.isEmpty(), 1000);
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("http://127.0.0.1/catchup72")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("force-seekable=yes")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("hr-seek=no")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("cache=yes")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("cache-secs=120")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("demuxer-readahead-secs=120")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("demuxer-hysteresis-secs=115")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("demuxer-seekable-cache=yes")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("demuxer-max-bytes=524288000")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("demuxer-max-back-bytes=262144000")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("length=120")));
    if (!playRequestLogs.isEmpty()) {
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("force-seekable=yes")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("hr-seek=no")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("cache=yes")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("cache-secs=120")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("demuxer-readahead-secs=120")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("demuxer-hysteresis-secs=115")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("demuxer-seekable-cache=yes")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("demuxer-max-bytes=524288000")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("demuxer-max-back-bytes=262144000")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("length=120")));
    }
    playerController.seekTimeshiftToFraction(0.25);
    QTRY_VERIFY_WITH_TIMEOUT(!catchupSeekLogs.isEmpty(), 1000);
    QVERIFY(catchupSeekLogs.constLast().contains(QStringLiteral("mode=absolute+keyframes")));
    QVERIFY(catchupSeekLogs.constLast().contains(QStringLiteral("host=127.0.0.1")));
    QCOMPARE(playerController.playbackMode(), QStringLiteral("catchup"));
    QCOMPARE(playerController.currentPlaybackUrl(), QStringLiteral("http://127.0.0.1/catchup72"));

    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::playerControllerCatchupDebugSnapshotUsesEffectiveBufferMetric()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 172;
    channel.name = QStringLiteral("Archive Debug Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live172");

    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup172"),
        QStringLiteral("Past Show"));

    playerController.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = 4096.0;
    const auto snapshot = playerController.debugOverlaySnapshot();
    QCOMPARE(snapshot.value(QStringLiteral("bufferDurationSourceText")).toString(), QStringLiteral("Catch-up cache"));
    QCOMPARE(snapshot.value(QStringLiteral("bufferDurationText")).toString(), QStringLiteral("120.00 s"));
    QCOMPARE(snapshot.value(QStringLiteral("bufferDurationSeconds")).toDouble(), 120.0);
    QCOMPARE(snapshot.value(QStringLiteral("mpvBufferDurationSeconds")).toDouble(), 4096.0);
}

void AppModelTests::playerControllerCatchupReconnectStabilizationIgnoresCacheDurationOutliers()
{
    PlayerController playerController;
    QStringList strictEnabledLogs;
    const auto subscriptionId = DebugLogger::instance().subscribe([&strictEnabledLogs](const DebugLogger::Entry &entry) {
        if (entry.category == QStringLiteral("mpv")
            && entry.message.contains(QStringLiteral("Startup buffering strict mode is now enabled"))) {
            strictEnabledLogs.push_back(entry.message);
        }
    });

    Channel channel;
    channel.id = 174;
    channel.name = QStringLiteral("Archive Reconnect Stabilization Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live174");

    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup174"),
        QStringLiteral("Past Show"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "errorOccurred",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("simulated-archive-error-stabilization"))));
    QVERIFY(playerController.m_reconnectActive);
    QVERIFY(playerController.m_reconnectAttemptInFlight);

    playerController.m_isPlaying = true;
    playerController.m_backendBuffering = false;
    playerController.m_playbackStalled = false;
    playerController.m_lastPlaybackPositionSeconds = -1.0;

    for (int i = 0; i < 14; ++i) {
        playerController.m_player.m_cachedTelemetry.positionSeconds = static_cast<double>(i + 1);
        playerController.m_player.m_cachedTelemetry.displayedVideoFramePtsSeconds = static_cast<double>(i + 1);
        playerController.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = 4096.0;
        playerController.updatePosition();
    }

    QVERIFY(!playerController.m_reconnectActive);
    QVERIFY(!playerController.m_reconnectAttemptInFlight);
    QVERIFY(strictEnabledLogs.isEmpty());

    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::playerControllerXtreamCatchupSeekRegeneratesUrlTransparently()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 73;
    channel.name = QStringLiteral("Xtream Archive Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/73.ts");

    const auto startUtc = QDateTime::fromString(QStringLiteral("2026-05-18T12:00:00Z"), Qt::ISODate);
    const auto stopUtc = startUtc.addSecs(3600);
    const auto canonicalUrl =
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/73.ts");
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://cdn.example/archive/initial.ts"),
        QStringLiteral("12:00 - 13:00  Archive Show"),
        startUtc,
        stopUtc,
        canonicalUrl);

    QStringList seekLogs;
    const auto subscriptionId = DebugLogger::instance().subscribe([&seekLogs](const DebugLogger::Entry &entry) {
        if (entry.category == QStringLiteral("player")
            && entry.message.startsWith(QStringLiteral("Catch-up timeline seek:"))) {
            seekLogs.push_back(entry.message);
        }
    });

    playerController.seekTimeshiftToFraction(158.0 / 3600.0);

    QTRY_VERIFY_WITH_TIMEOUT(!seekLogs.isEmpty(), 1000);
    QVERIFY(seekLogs.constLast().contains(QStringLiteral("mode=url-regenerate")));
    QVERIFY(seekLogs.constLast().contains(QStringLiteral("streamBase=120.000s")));
    QVERIFY(seekLogs.constLast().contains(QStringLiteral("residual=38.000s")));
    QCOMPARE(
        playerController.currentPlaybackUrl(),
        QStringLiteral("http://provider.example/timeshift/user/pass/59/2026-05-18:12-02/73.ts"));
    QCOMPARE(playerController.playbackMode(), QStringLiteral("catchup"));
    QCOMPARE(playerController.catchupProgramLabel(), QStringLiteral("12:00 - 13:00  Archive Show"));
    QCOMPARE(playerController.catchupTimelinePositionSeconds(), 158.0);

    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::playerControllerCatchupRegeneratedSeekStopsReconnectWithoutStrictStartupRestore()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 173;
    channel.name = QStringLiteral("Xtream Archive Reconnect Seek Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/173.ts");

    const auto startUtc = QDateTime::fromString(QStringLiteral("2026-05-18T12:00:00Z"), Qt::ISODate);
    const auto stopUtc = startUtc.addSecs(3600);
    const auto canonicalUrl =
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/173.ts");
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://cdn.example/archive/initial173.ts"),
        QStringLiteral("12:00 - 13:00  Archive Show"),
        startUtc,
        stopUtc,
        canonicalUrl);
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    QStringList reconnectStartLogs;
    QStringList reconnectStopLogs;
    QStringList seekLogs;
    const auto subscriptionId = DebugLogger::instance().subscribe(
        [&reconnectStartLogs, &reconnectStopLogs, &seekLogs](const DebugLogger::Entry &entry) {
            if (entry.category != QStringLiteral("player")) {
                return;
            }
            if (entry.message.startsWith(QStringLiteral("Reconnect loop started:"))) {
                reconnectStartLogs.push_back(entry.message);
            }
            if (entry.message.startsWith(QStringLiteral("Reconnect loop stopped:"))) {
                reconnectStopLogs.push_back(entry.message);
            }
            if (entry.message.startsWith(QStringLiteral("Catch-up timeline seek:"))) {
                seekLogs.push_back(entry.message);
            }
        });

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "errorOccurred",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("simulated-archive-error-before-seek"))));
    QTRY_VERIFY_WITH_TIMEOUT(!reconnectStartLogs.isEmpty(), 1000);

    playerController.seekTimeshiftToFraction(158.0 / 3600.0);

    QTRY_VERIFY_WITH_TIMEOUT(!seekLogs.isEmpty(), 1000);
    QVERIFY(seekLogs.constLast().contains(QStringLiteral("mode=url-regenerate")));
    QTRY_VERIFY_WITH_TIMEOUT(!reconnectStopLogs.isEmpty(), 1000);
    QVERIFY(reconnectStopLogs.constLast().contains(QStringLiteral("catchup-seek-url-regenerate")));
    QCOMPARE(playerController.playbackMode(), QStringLiteral("catchup"));
    QCOMPARE(
        playerController.currentPlaybackUrl(),
        QStringLiteral("http://provider.example/timeshift/user/pass/59/2026-05-18:12-02/173.ts"));

    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::playerControllerSharedPrimarySignalsAndRetuneStayOnActivePlayer()
{
    PlayerController playerController;
    OKILTV::Player::MpvPlayer promotedPrimaryPlayer;

    Channel primaryChannel;
    primaryChannel.id = 21;
    primaryChannel.name = QStringLiteral("Primary Channel");
    primaryChannel.profileId = QUuid::createUuid();
    primaryChannel.streamUrl = QStringLiteral("http://127.0.0.1/primary");

    Channel promotedChannel;
    promotedChannel.id = 22;
    promotedChannel.name = QStringLiteral("Promoted Channel");
    promotedChannel.profileId = QUuid::createUuid();
    promotedChannel.streamUrl = QStringLiteral("http://127.0.0.1/promoted");

    Channel replacementChannel;
    replacementChannel.id = 23;
    replacementChannel.name = QStringLiteral("Replacement Channel");
    replacementChannel.profileId = QUuid::createUuid();
    replacementChannel.streamUrl = QStringLiteral("http://127.0.0.1/replacement");

    playerController.playChannel(primaryChannel);
    playerController.attachSharedPlayback(&promotedPrimaryPlayer, promotedChannel, true, false);

    QVERIFY(playerController.usingSharedPlayback());
    QCOMPARE(playerController.player(), &promotedPrimaryPlayer);
    QCOMPARE(playerController.currentChannel().value(QStringLiteral("id")).toInt(), promotedChannel.id);

    QVERIFY(QMetaObject::invokeMethod(
        &promotedPrimaryPlayer,
        "bufferingStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, true)));
    QVERIFY(playerController.isBuffering());

    QVERIFY(QMetaObject::invokeMethod(
        &promotedPrimaryPlayer,
        "bufferingStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));
    QVERIFY(!playerController.isBuffering());

    playerController.playChannel(replacementChannel);
    QVERIFY(playerController.usingSharedPlayback());
    QCOMPARE(playerController.player(), &promotedPrimaryPlayer);
    QCOMPARE(playerController.currentChannel().value(QStringLiteral("id")).toInt(), replacementChannel.id);

    QVERIFY(QMetaObject::invokeMethod(&promotedPrimaryPlayer, "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        &promotedPrimaryPlayer,
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));
    QVERIFY(QMetaObject::invokeMethod(&promotedPrimaryPlayer, "playbackEnded", Qt::DirectConnection));
    QVERIFY(playerController.isBuffering());
}

void AppModelTests::mpvPlayerDemuxerMaxBytesMapping()
{
    QCOMPARE(
        OKILTV::Player::MpvPlayer::demuxerMaxBytesForBufferSeconds(2.0),
        static_cast<qint64>(64) * 1024 * 1024);
    QCOMPARE(
        OKILTV::Player::MpvPlayer::demuxerMaxBytesForBufferSeconds(10.0),
        static_cast<qint64>(160) * 1024 * 1024);
    QCOMPARE(
        OKILTV::Player::MpvPlayer::demuxerMaxBytesForBufferSeconds(60.0),
        static_cast<qint64>(512) * 1024 * 1024);
}

void AppModelTests::mpvPlayerCacheWindowSecondsMapping()
{
    QCOMPARE(OKILTV::Player::MpvPlayer::cacheWindowSecondsForBufferTarget(2.0), 10.0);
    QCOMPARE(OKILTV::Player::MpvPlayer::cacheWindowSecondsForBufferTarget(10.0), 30.0);
    QCOMPARE(OKILTV::Player::MpvPlayer::cacheWindowSecondsForBufferTarget(60.0), 120.0);
}

void AppModelTests::mpvPlayerSteadyStateCacheBandMapping()
{
    QCOMPARE(OKILTV::Player::MpvPlayer::steadyStateCacheLimitSecondsForBufferTarget(2.0), 5.0);
    QCOMPARE(OKILTV::Player::MpvPlayer::steadyStateCacheLimitSecondsForBufferTarget(10.0), 13.0);
    QCOMPARE(OKILTV::Player::MpvPlayer::steadyStateCacheHysteresisSecondsForBufferTarget(2.0), 4.0);
    QCOMPARE(OKILTV::Player::MpvPlayer::steadyStateCacheHysteresisSecondsForBufferTarget(10.0), 12.0);
}

void AppModelTests::playerControllerStartupBufferFallbackTimeoutMapping()
{
    QCOMPARE(PlayerController::startupBufferFallbackTimeoutMs(2.0), 2000);
    QCOMPARE(PlayerController::startupBufferFallbackTimeoutMs(10.0), 10000);
    QCOMPARE(PlayerController::startupBufferFallbackTimeoutMs(2.0, 2), 5000);
    QCOMPARE(PlayerController::startupBufferFallbackTimeoutMs(10.0, 5), 16000);
}

void AppModelTests::playerControllerAdaptiveSteadyStateMaxBytesMapping()
{
    QCOMPARE(
        PlayerController::adaptiveSteadyStateCacheLimitSeconds(3.0),
        6.0);
    QCOMPARE(
        PlayerController::adaptiveSteadyStateCacheHysteresisSeconds(3.0),
        5.0);
    QCOMPARE(
        PlayerController::adaptiveSteadyStateMaxBytes(3.0, std::nullopt),
        OKILTV::Player::MpvPlayer::demuxerMaxBytesForBufferSeconds(6.0));
    QCOMPARE(
        PlayerController::adaptiveSteadyStateMaxBytes(3.0, 8.0 * 1000.0 * 1000.0),
        static_cast<qint64>(8) * 1024 * 1024);
    QVERIFY(
        PlayerController::adaptiveSteadyStateMaxBytes(3.0, 160.0 * 1000.0 * 1000.0)
        > static_cast<qint64>(8) * 1024 * 1024);
}

void AppModelTests::playerControllerReconnectDepletionTimeoutFollowsWaitForDataRule()
{
    QCOMPARE(PlayerController::reconnectDepletionTimeoutMsForWaitSeconds(0.1), 10000);
    QCOMPARE(PlayerController::reconnectDepletionTimeoutMsForWaitSeconds(5.0), 10000);
    QCOMPARE(PlayerController::reconnectDepletionTimeoutMsForWaitSeconds(9.9), 10000);
    QCOMPARE(PlayerController::reconnectDepletionTimeoutMsForWaitSeconds(12.0), 12000);
    QCOMPARE(PlayerController::reconnectDepletionTimeoutMsForWaitSeconds(12.1), 12100);
    QCOMPARE(
        PlayerController::reconnectDepletionTimeoutMsForWaitSeconds(std::numeric_limits<double>::quiet_NaN()),
        10000);
}

void AppModelTests::playerControllerPreemptiveReconnectHeuristic()
{
    QVERIFY(PlayerController::shouldStartPreemptiveReconnect(1.0, 5.0, 0.0, true, false));
    QVERIFY(PlayerController::shouldStartPreemptiveReconnect(9.0, 10.0, 500.0, false, false));
    QVERIFY(PlayerController::shouldStartPreemptiveReconnect(1.0, 5.0, 0.0, false, true));
    QVERIFY(!PlayerController::shouldStartPreemptiveReconnect(1.0, 5.0, 0.0, true, true));
    QVERIFY(!PlayerController::shouldStartPreemptiveReconnect(1.0, 5.0, 50000.0, true, false));
    QVERIFY(!PlayerController::shouldStartPreemptiveReconnect(std::nullopt, 5.0, std::nullopt, true, false));
}

void AppModelTests::playerControllerDeadStreamDisconnectHeuristic()
{
    QVERIFY(PlayerController::deadStreamLikelyDisconnected(0.0, 0.0));
    QVERIFY(PlayerController::deadStreamLikelyDisconnected(0.03, std::nullopt));
    QVERIFY(PlayerController::deadStreamLikelyDisconnected(0.05, 512.0));

    QVERIFY(!PlayerController::deadStreamLikelyDisconnected(std::nullopt, 0.0));
    QVERIFY(!PlayerController::deadStreamLikelyDisconnected(0.2, 0.0));
    QVERIFY(!PlayerController::deadStreamLikelyDisconnected(0.01, 50000.0));
    QVERIFY(!PlayerController::deadStreamLikelyDisconnected(
        std::numeric_limits<double>::quiet_NaN(),
        std::nullopt));
}

void AppModelTests::playerControllerDebugHelpersExtractStreamFields()
{
    QCOMPARE(
        PlayerController::debugStreamHostFromUrl(QStringLiteral("https://cdn.example.com/live/2137.ts?token=abc")),
        QStringLiteral("cdn.example.com"));
    QCOMPARE(
        PlayerController::debugStreamIdFromUrl(QStringLiteral("https://cdn.example.com/live/2137.ts?token=abc")),
        QStringLiteral("2137.ts"));
    QCOMPARE(
        PlayerController::debugStreamIdFromUrl(
            QStringLiteral("http://edge.example.net/play/26f1aa82-d526-45a1-b7b8-1eb6a117394d?foo=bar")),
        QStringLiteral("26f1aa82-d526-45a1-b7b8-1eb6a117394d"));
    QCOMPARE(PlayerController::debugStreamHostFromUrl(QStringLiteral("")), QStringLiteral("N/A"));
    QCOMPARE(PlayerController::debugStreamIdFromUrl(QStringLiteral("")), QStringLiteral("N/A"));
}

void AppModelTests::playerControllerDebugBufferDurationFormatting()
{
    QCOMPARE(PlayerController::formatDebugBufferDuration(3.0), QStringLiteral("3.00 s"));
    QCOMPARE(PlayerController::formatDebugBufferDuration(16.6), QStringLiteral("16.60 s"));
    QCOMPARE(PlayerController::formatDebugBufferDuration(0.0), QStringLiteral("0.00 s"));
    QCOMPARE(PlayerController::formatDebugBufferDuration(-1.0), QStringLiteral("N/A"));
    QCOMPARE(PlayerController::formatDebugBufferDuration(std::numeric_limits<double>::quiet_NaN()), QStringLiteral("N/A"));
}

void AppModelTests::playerControllerDebugFramerateFormatting()
{
    QCOMPARE(PlayerController::formatDebugFramerate(25.0), QStringLiteral("25.00 fps"));
    QCOMPARE(PlayerController::formatDebugFramerate(59.94), QStringLiteral("59.94 fps"));
    QCOMPARE(PlayerController::formatDebugFramerate(0.0), QStringLiteral("N/A"));
    QCOMPARE(PlayerController::formatDebugFramerate(-1.0), QStringLiteral("N/A"));
    QCOMPARE(PlayerController::formatDebugFramerate(std::numeric_limits<double>::quiet_NaN()), QStringLiteral("N/A"));
}

void AppModelTests::playerControllerDebugBitrateFormatting()
{
    QCOMPARE(PlayerController::formatDebugBitrate(0.0), QStringLiteral("0 Kbps"));
    QCOMPARE(PlayerController::formatDebugBitrate(1250000.0), QStringLiteral("1250 Kbps"));
    QCOMPARE(PlayerController::formatDebugBitrate(-1.0), QStringLiteral("N/A"));
    QCOMPARE(PlayerController::formatDebugBitrate(std::numeric_limits<double>::quiet_NaN()), QStringLiteral("N/A"));
}

void AppModelTests::playerControllerDebugTimestampFormat()
{
    const auto timestamp = PlayerController::debugTimestampNowLocal();
    const QRegularExpression pattern(
        QStringLiteral(R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6}$)"));
    QVERIFY2(pattern.match(timestamp).hasMatch(), qPrintable(timestamp));
}

void AppModelTests::appControllerTracksWatchTimeAndFlushesOnPlaybackBoundaries()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    const auto channelId = channels.first().id;

    QVERIFY(harness.channelListModel->activateById(channelId));
    QVERIFY(QMetaObject::invokeMethod(
        harness.playerController->player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));
    QTest::qWait(2200);
    QVERIFY(QMetaObject::invokeMethod(
        harness.playerController->player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, true)));

    const auto statsAfterFirstSegment = harness.database->loadWatchSecondsByProfile(harness.activeProfileId());
    if (statsAfterFirstSegment.value(channelId, 0) < 1) {
        QSKIP("Playback timing could not be observed reliably in this headless libmpv test environment.");
    }

    const auto baselineSeconds = statsAfterFirstSegment.value(channelId, 0);
    QVERIFY(harness.channelListModel->activateById(channelId));
    QVERIFY(QMetaObject::invokeMethod(
        harness.playerController->player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));
    QTest::qWait(200);
    QVERIFY(QMetaObject::invokeMethod(
        harness.playerController->player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, true)));

    const auto statsAfterShortSegment = harness.database->loadWatchSecondsByProfile(harness.activeProfileId());
    QCOMPARE(statsAfterShortSegment.value(channelId, 0), baselineSeconds);
}

void AppModelTests::appControllerFlushTrackedWatchSecondsAllowsChannelIdZero()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));

    harness.appController->m_watchTrackingProfileId = harness.activeProfileId();
    harness.appController->m_watchTrackingChannelId = 0;
    harness.appController->m_watchTrackingActive = true;
    harness.appController->m_watchTrackingElapsed.start();

    QTest::qWait(1100);
    harness.appController->flushTrackedWatchSeconds();

    const auto watchSecondsByChannelId = harness.database->loadWatchSecondsByProfile(harness.activeProfileId());
    QVERIFY2(
        watchSecondsByChannelId.value(0, 0) >= 1,
        "Watch stats flush should persist elapsed time for channel id 0.");
}

void AppModelTests::settingsControllerTracksDirtyStateForRegularSettings()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    PlayerController playerController;
    MultiViewController multiViewController(&settings, nullptr, &playerController);
    ProfilesModel profilesModel(&settings);
    SettingsController controller(&settings, &playerController, &multiViewController, &profilesModel);
    QSignalSpy dirtySpy(&controller, &SettingsController::dirtyChanged);

    QVERIFY(!controller.dirty());

    controller.setPreventDisplaySleep(false);
    QVERIFY(controller.dirty());
    controller.cancel();
    QVERIFY(!controller.dirty());
    QCOMPARE(controller.preventDisplaySleep(), true);

    controller.setOverlayAutoHide(false);
    QVERIFY(controller.dirty());

    controller.setOverlayAutoHide(true);
    QVERIFY(!controller.dirty());

    controller.setOverlayAutoHideSeconds(5);
    QVERIFY(controller.dirty());
    controller.cancel();
    QVERIFY(!controller.dirty());
    QCOMPARE(controller.overlayAutoHideSeconds(), 3);

    controller.setTheme(QStringLiteral("   "));
    QVERIFY(!controller.dirty());

    controller.setRefreshIntervalMinutes(120);
    QVERIFY(controller.dirty());
    controller.cancel();
    QVERIFY(!controller.dirty());
    QCOMPARE(controller.refreshIntervalMinutes(), settings.current().refreshIntervalMinutes);

    controller.setRefreshIntervalMinutes(240);
    QVERIFY(controller.dirty());
    controller.save();
    QVERIFY(!controller.dirty());
    QCOMPARE(settings.current().refreshIntervalMinutes, 240);

    controller.setGuidePastHours(12);
    QVERIFY(controller.dirty());
    controller.cancel();
    QVERIFY(!controller.dirty());
    QCOMPARE(controller.guidePastHours(), 6);

    controller.setGuidePastHours(999);
    QVERIFY(controller.dirty());
    controller.save();
    QVERIFY(!controller.dirty());
    QCOMPARE(settings.current().guidePastHours, 48);
    QCOMPARE(controller.guidePastHours(), 48);

    controller.setWaitForDataStreamSeconds(6.4);
    QVERIFY(controller.dirty());
    controller.cancel();
    QVERIFY(!controller.dirty());
    QVERIFY(std::abs(controller.waitForDataStreamSeconds() - 5.0) < 0.0001);

    controller.setBufferSizeSeconds(4.8);
    QVERIFY(controller.dirty());
    controller.cancel();
    QVERIFY(!controller.dirty());
    QVERIFY(std::abs(controller.bufferSizeSeconds() - 3.0) < 0.0001);

    controller.setDeinterlaceEnabled(false);
    QVERIFY(controller.dirty());
    controller.cancel();
    QVERIFY(!controller.dirty());
    QCOMPARE(controller.deinterlaceEnabled(), true);

    controller.setPlayerUserAgent(QStringLiteral("  CustomAgent/2.0  "));
    QVERIFY(controller.dirty());
    controller.cancel();
    QVERIFY(!controller.dirty());
    QCOMPARE(controller.playerUserAgent(), QStringLiteral(""));

    controller.setTimeshiftEnabled(true);
    controller.setTimeshiftWindowMinutes(120);
    controller.setTimeshiftSegmentSeconds(12);
    controller.setTimeshiftStorageDirectory(QStringLiteral("/tmp/timeshift"));
    controller.setTimeshiftMaxDiskGb(12);
    QVERIFY(controller.dirty());
    controller.cancel();
    QVERIFY(!controller.dirty());
    QCOMPARE(controller.timeshiftEnabled(), false);
    QCOMPARE(controller.timeshiftWindowMinutes(), 90);
    QCOMPARE(controller.timeshiftSegmentSeconds(), 2);
    QCOMPARE(controller.timeshiftStorageDirectory(), QStringLiteral(""));
    QCOMPARE(controller.timeshiftMaxDiskGb(), 8);

    controller.setWaitForDataStreamSeconds(999.9);
    controller.setBufferSizeSeconds(0.01);
    controller.setDeinterlaceEnabled(false);
    controller.setPlayerUserAgent(QStringLiteral("OKILTV-Agent/3.0"));
    controller.setTimeshiftEnabled(true);
    controller.setTimeshiftWindowMinutes(999);
    controller.setTimeshiftSegmentSeconds(999);
    controller.setTimeshiftStorageDirectory(QStringLiteral("/tmp/ts-cache"));
    controller.setTimeshiftMaxDiskGb(999);
    QVERIFY(controller.dirty());
    controller.save();
    QVERIFY(!controller.dirty());
    QVERIFY(std::abs(settings.current().playerWaitForStreamSeconds - 120.0) < 0.0001);
    QVERIFY(std::abs(settings.current().playerBufferSeconds - 0.1) < 0.0001);
    QCOMPARE(settings.current().playerDeinterlaceEnabled, false);
    QCOMPARE(settings.current().playerUserAgent, QStringLiteral("OKILTV-Agent/3.0"));
    QCOMPARE(settings.current().timeshiftEnabled, true);
    QCOMPARE(settings.current().timeshiftWindowMinutes, 360);
    QCOMPARE(settings.current().timeshiftSegmentSeconds, 60);
    QCOMPARE(settings.current().timeshiftStorageDirectory, QStringLiteral("/tmp/ts-cache"));
    QCOMPARE(settings.current().timeshiftMaxDiskGb, 128);
    QVERIFY(std::abs(controller.waitForDataStreamSeconds() - 120.0) < 0.0001);
    QVERIFY(std::abs(controller.bufferSizeSeconds() - 0.1) < 0.0001);
    QCOMPARE(controller.deinterlaceEnabled(), false);
    QCOMPARE(controller.playerUserAgent(), QStringLiteral("OKILTV-Agent/3.0"));
    QCOMPARE(controller.timeshiftEnabled(), true);
    QCOMPARE(controller.timeshiftWindowMinutes(), 360);
    QCOMPARE(controller.timeshiftSegmentSeconds(), 60);
    QCOMPARE(controller.timeshiftStorageDirectory(), QStringLiteral("/tmp/ts-cache"));
    QCOMPARE(controller.timeshiftMaxDiskGb(), 128);

    controller.setMinimizeToTrayOnMinimize(false);
    QVERIFY(controller.dirty());
    controller.cancel();
    QVERIFY(!controller.dirty());
    QCOMPARE(controller.minimizeToTrayOnMinimize(), true);

    controller.setMultiviewRetainSelectionOnPromotion(true);
    QVERIFY(controller.dirty());
    controller.cancel();
    QVERIFY(!controller.dirty());
    QCOMPARE(controller.multiviewRetainSelectionOnPromotion(), false);

    controller.setMultiviewRetainSelectionOnPromotion(true);
    QVERIFY(controller.dirty());
    controller.save();
    QVERIFY(!controller.dirty());
    QCOMPARE(settings.current().multiviewRetainSelectionOnPromotion, true);
    QCOMPARE(controller.multiviewRetainSelectionOnPromotion(), true);

    controller.setDvrRecordingsDirectory(QStringLiteral("/tmp/dvr"));
    controller.setDvrRemuxToMkv(false);
    controller.setDvrStartOffsetMinutes(-4);
    controller.setDvrEndOffsetMinutes(9);
    QVERIFY(controller.dirty());
    controller.save();
    QVERIFY(!controller.dirty());
    QCOMPARE(settings.current().dvrRecordingsDirectory, QStringLiteral("/tmp/dvr"));
    QCOMPARE(settings.current().dvrRemuxToMkv, false);
    QCOMPARE(settings.current().dvrStartOffsetMinutes, -4);
    QCOMPARE(settings.current().dvrEndOffsetMinutes, 9);

    QVERIFY(dirtySpy.count() >= 9);
}

void AppModelTests::settingsControllerDisablesFfmpegDependentOptionsWhenToolsUnavailable()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const auto toolsDir = tempDir.filePath(QStringLiteral("empty-tools"));
    QVERIFY(QDir().mkpath(toolsDir));
    ScopedPathOverride scopedPath(toolsDir);

#if defined(Q_OS_WIN)
    const QString ffmpegName = QStringLiteral("ffmpeg.exe");
    const QString ffprobeName = QStringLiteral("ffprobe.exe");
#else
    const QString ffmpegName = QStringLiteral("ffmpeg");
    const QString ffprobeName = QStringLiteral("ffprobe");
#endif
    const auto bundledFfmpegPath = QDir(QCoreApplication::applicationDirPath()).filePath(ffmpegName);
    const auto bundledFfprobePath = QDir(QCoreApplication::applicationDirPath()).filePath(ffprobeName);
    if (QFileInfo::exists(bundledFfmpegPath) || QFileInfo::exists(bundledFfprobePath)) {
        QSKIP("Test requires no bundled ffmpeg/ffprobe in application directory.");
    }

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    settings.current().timeshiftEnabled = true;
    settings.current().remuxRecordingsToMkv = true;
    settings.current().dvrRemuxToMkv = true;

    PlayerController playerController;
    MultiViewController multiViewController(&settings, nullptr, &playerController);
    ProfilesModel profilesModel(&settings);
    SettingsController controller(&settings, &playerController, &multiViewController, &profilesModel);

    if (controller.ffmpegToolsAvailable()) {
        QSKIP("Environment still resolves ffmpeg/ffprobe; unavailable-tools path cannot be isolated in this runtime.");
    }
    QVERIFY(!controller.timeshiftEnabled());
    QVERIFY(!controller.remuxRecordingsToMkv());
    QVERIFY(!controller.dvrRemuxToMkv());
    QVERIFY(!controller.dirty());

    controller.setTimeshiftEnabled(true);
    controller.setRemuxRecordingsToMkv(true);
    controller.setDvrRemuxToMkv(true);
    QVERIFY(!controller.timeshiftEnabled());
    QVERIFY(!controller.remuxRecordingsToMkv());
    QVERIFY(!controller.dvrRemuxToMkv());

    controller.save();
    QVERIFY(!settings.current().timeshiftEnabled);
    QVERIFY(!settings.current().remuxRecordingsToMkv);
    QVERIFY(!settings.current().dvrRemuxToMkv);
}

void AppModelTests::settingsControllerAllowsFfmpegDependentOptionsWhenToolsAvailable()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const auto toolsDir = tempDir.filePath(QStringLiteral("tools"));
    QVERIFY(QDir().mkpath(toolsDir));

#if defined(Q_OS_WIN)
    const auto ffmpegPath = QDir(toolsDir).filePath(QStringLiteral("ffmpeg.exe"));
    const auto ffprobePath = QDir(toolsDir).filePath(QStringLiteral("ffprobe.exe"));
    const QString shim = QStringLiteral("MZ");
#else
    const auto ffmpegPath = QDir(toolsDir).filePath(QStringLiteral("ffmpeg"));
    const auto ffprobePath = QDir(toolsDir).filePath(QStringLiteral("ffprobe"));
    const QString shim = QStringLiteral("#!/usr/bin/env bash\nexit 0\n");
#endif

    QVERIFY(writeExecutableTextFile(ffmpegPath, shim));
    QVERIFY(writeExecutableTextFile(ffprobePath, shim));
    ScopedPathOverride scopedPath(toolsDir);

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    PlayerController playerController;
    MultiViewController multiViewController(&settings, nullptr, &playerController);
    ProfilesModel profilesModel(&settings);
    SettingsController controller(&settings, &playerController, &multiViewController, &profilesModel);

    QVERIFY(controller.ffmpegToolsAvailable());

    controller.setTimeshiftEnabled(true);
    controller.setRemuxRecordingsToMkv(true);
    controller.setDvrRemuxToMkv(true);
    QVERIFY(controller.timeshiftEnabled());
    QVERIFY(controller.remuxRecordingsToMkv());
    QVERIFY(controller.dvrRemuxToMkv());
    QVERIFY(controller.dirty());

    controller.save();
    QVERIFY(settings.current().timeshiftEnabled);
    QVERIFY(settings.current().remuxRecordingsToMkv);
    QVERIFY(settings.current().dvrRemuxToMkv);
}

void AppModelTests::timeshiftControllerServesPlaybackOverLocalHttp()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    settings.current().timeshiftStorageDirectory = tempDir.filePath(QStringLiteral("timeshift"));

    PlayerController playerController;
    DvrController dvrController(&settings, &playerController);
    MultiViewController multiViewController(&settings, nullptr, &playerController);
    TimeshiftController controller(&settings, &playerController, &dvrController, &multiViewController);

    QVERIFY2(controller.ensurePlaybackServer(), qPrintable(controller.m_playbackServer.errorString()));

    controller.m_session.emplace();
    auto &session = controller.m_session.value();
    session.id = QStringLiteral("test-session");
    session.sessionDirectory = tempDir.filePath(QStringLiteral("session"));
    session.playlistPath = QDir(session.sessionDirectory).filePath(QStringLiteral("stream_0.m3u8"));
    session.avMasterPlaylistPath = QDir(session.sessionDirectory).filePath(QStringLiteral("av_master.m3u8"));
    QDir().mkpath(session.sessionDirectory);
    session.playbackUrl = controller.localPlaybackUrl(session);
    TimeshiftController::SubtitleRendition subtitle;
    subtitle.playlistFileName = QStringLiteral("subtitle_0.m3u8");
    subtitle.playlistPath = QDir(session.sessionDirectory).filePath(subtitle.playlistFileName);
    subtitle.name = QStringLiteral("English");
    subtitle.language = QStringLiteral("eng");
    subtitle.isDefault = true;
    session.subtitleRenditions.push_back(subtitle);

    QFile avMaster(session.avMasterPlaylistPath);
    QVERIFY(avMaster.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    avMaster.write(
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"group_aud\",NAME=\"audio_1\",DEFAULT=YES,LANGUAGE=\"eng\",URI=\"stream_1.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=111111,AUDIO=\"group_aud\"\n"
        "stream_0.m3u8\n");
    avMaster.close();

    QFile playlist(session.playlistPath);
    QVERIFY(playlist.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    playlist.write(
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:2\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXTINF:2.0,\n"
        "#EXT-X-PROGRAM-DATE-TIME:2026-03-29T13:41:44.000Z\n"
        "segment_000000.ts\n"
        "#EXTINF:2.0,\n"
        "#EXT-X-PROGRAM-DATE-TIME:2026-03-29T13:41:46.000Z\n"
        "segment_000001.ts\n");
    playlist.close();
    session.playlistInfo = controller.parsePlaylistFile(session.playlistPath);
    QVERIFY(session.playlistInfo.valid);

    QFile segment(QDir(session.sessionDirectory).filePath(QStringLiteral("segment_000000.ts")));
    QVERIFY(segment.open(QIODevice::WriteOnly | QIODevice::Truncate));
    segment.write("segment-bytes");
    segment.close();

    QFile segment1(QDir(session.sessionDirectory).filePath(QStringLiteral("segment_000001.ts")));
    QVERIFY(segment1.open(QIODevice::WriteOnly | QIODevice::Truncate));
    segment1.write("segment-next!");
    segment1.close();

    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, controller.m_playbackServer.serverPort());
    QVERIFY(socket.waitForConnected(2000));
    socket.write(
        "GET /test-session/master.m3u8 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "\r\n");
    socket.flush();
    QTRY_VERIFY_WITH_TIMEOUT(
        socket.bytesAvailable() > 0 || socket.state() == QAbstractSocket::UnconnectedState,
        2000);
    QByteArray response = socket.readAll();
    while (socket.waitForReadyRead(50)) {
        response += socket.readAll();
    }

    QCOMPARE(
        session.playbackUrl,
        QStringLiteral("http://127.0.0.1:%1/test-session/master.m3u8").arg(controller.m_playbackServer.serverPort()));
    QVERIFY(response.startsWith("HTTP/1.1 200 OK\r\n"));
    QVERIFY(response.contains("Accept-Ranges: bytes\r\n"));
    QVERIFY(response.contains("#EXTM3U"));
    QVERIFY(response.contains("SUBTITLES=\"ts_subs\""));
    QVERIFY(response.contains("TYPE=SUBTITLES"));
    QVERIFY(response.contains("URI=\"subtitle_0.m3u8\""));

    QTcpSocket pdtMasterSocket;
    pdtMasterSocket.connectToHost(QHostAddress::LocalHost, controller.m_playbackServer.serverPort());
    QVERIFY(pdtMasterSocket.waitForConnected(2000));
    pdtMasterSocket.write(
        "GET /test-session/master.m3u8?pdt=2026-03-29T13:41:44.000Z&target_pdt=2026-03-29T13:41:46.000Z HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "\r\n");
    pdtMasterSocket.flush();
    QTRY_VERIFY_WITH_TIMEOUT(
        pdtMasterSocket.bytesAvailable() > 0 || pdtMasterSocket.state() == QAbstractSocket::UnconnectedState,
        2000);
    QByteArray pdtMasterResponse = pdtMasterSocket.readAll();
    while (pdtMasterSocket.waitForReadyRead(50)) {
        pdtMasterResponse += pdtMasterSocket.readAll();
    }
    QVERIFY(pdtMasterResponse.startsWith("HTTP/1.1 200 OK\r\n"));
    QVERIFY(
        pdtMasterResponse.contains("stream_0.m3u8?pdt=2026-03-29T13:41:44.000Z&target_pdt=2026-03-29T13:41:46.000Z")
        || pdtMasterResponse.contains("stream_0.m3u8?pdt=2026-03-29T13%3A41%3A44.000Z&target_pdt=2026-03-29T13%3A41%3A46.000Z"));
    QVERIFY(
        pdtMasterResponse.contains("subtitle_0.m3u8?pdt=2026-03-29T13:41:44.000Z&target_pdt=2026-03-29T13:41:46.000Z")
        || pdtMasterResponse.contains("subtitle_0.m3u8?pdt=2026-03-29T13%3A41%3A44.000Z&target_pdt=2026-03-29T13%3A41%3A46.000Z"));

    QTcpSocket pdtMediaSocket;
    pdtMediaSocket.connectToHost(QHostAddress::LocalHost, controller.m_playbackServer.serverPort());
    QVERIFY(pdtMediaSocket.waitForConnected(2000));
    pdtMediaSocket.write(
        "GET /test-session/stream_0.m3u8?pdt=2026-03-29T13:41:44.000Z&target_pdt=2026-03-29T13:41:46.000Z HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "\r\n");
    pdtMediaSocket.flush();
    QTRY_VERIFY_WITH_TIMEOUT(
        pdtMediaSocket.bytesAvailable() > 0 || pdtMediaSocket.state() == QAbstractSocket::UnconnectedState,
        2000);
    QByteArray pdtMediaResponse = pdtMediaSocket.readAll();
    while (pdtMediaSocket.waitForReadyRead(50)) {
        pdtMediaResponse += pdtMediaSocket.readAll();
    }
    QVERIFY(pdtMediaResponse.startsWith("HTTP/1.1 200 OK\r\n"));
    QVERIFY(pdtMediaResponse.contains("#EXT-X-PLAYLIST-TYPE:EVENT"));
    QVERIFY(pdtMediaResponse.contains("#EXT-X-MEDIA-SEQUENCE:0"));
    QVERIFY(pdtMediaResponse.contains("#EXT-X-START:TIME-OFFSET=2.000,PRECISE=YES"));
    QVERIFY(pdtMediaResponse.contains("#EXT-X-PROGRAM-DATE-TIME:2026-03-29T13:41:44.000Z"));
    QVERIFY(pdtMediaResponse.contains("#EXT-X-PROGRAM-DATE-TIME:2026-03-29T13:41:46.000Z"));
    QVERIFY(pdtMediaResponse.contains("segment_000000.ts"));
    QVERIFY(pdtMediaResponse.contains("segment_000001.ts"));

    QTcpSocket legacyAnchorSocket;
    legacyAnchorSocket.connectToHost(QHostAddress::LocalHost, controller.m_playbackServer.serverPort());
    QVERIFY(legacyAnchorSocket.waitForConnected(2000));
    legacyAnchorSocket.write(
        "GET /test-session/master.m3u8?utc=1774791703976 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "\r\n");
    legacyAnchorSocket.flush();
    QTRY_VERIFY_WITH_TIMEOUT(
        legacyAnchorSocket.bytesAvailable() > 0 || legacyAnchorSocket.state() == QAbstractSocket::UnconnectedState,
        2000);
    QByteArray legacyAnchorResponse = legacyAnchorSocket.readAll();
    while (legacyAnchorSocket.waitForReadyRead(50)) {
        legacyAnchorResponse += legacyAnchorSocket.readAll();
    }
    QVERIFY(legacyAnchorResponse.startsWith("HTTP/1.1 400 Bad Request\r\n"));
    QVERIFY(legacyAnchorResponse.contains("legacy-anchor-unsupported"));

    QTcpSocket rangeSocket;
    rangeSocket.connectToHost(QHostAddress::LocalHost, controller.m_playbackServer.serverPort());
    QVERIFY(rangeSocket.waitForConnected(2000));
    rangeSocket.write(
        "GET /test-session/segment_000000.ts HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Range: bytes=0-5\r\n"
        "\r\n");
    rangeSocket.flush();
    QTRY_VERIFY_WITH_TIMEOUT(
        rangeSocket.bytesAvailable() > 0 || rangeSocket.state() == QAbstractSocket::UnconnectedState,
        2000);
    QByteArray rangeResponse = rangeSocket.readAll();
    while (rangeSocket.waitForReadyRead(50)) {
        rangeResponse += rangeSocket.readAll();
    }
    QVERIFY(rangeResponse.startsWith("HTTP/1.1 206 Partial Content\r\n"));
    QVERIFY(rangeResponse.contains("Accept-Ranges: bytes\r\n"));
    QVERIFY(rangeResponse.contains("Content-Range: bytes 0-5/13\r\n"));
    QVERIFY(rangeResponse.endsWith("segmen"));

    QTcpSocket invalidRangeSocket;
    invalidRangeSocket.connectToHost(QHostAddress::LocalHost, controller.m_playbackServer.serverPort());
    QVERIFY(invalidRangeSocket.waitForConnected(2000));
    invalidRangeSocket.write(
        "GET /test-session/segment_000000.ts HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Range: bytes=99-120\r\n"
        "\r\n");
    invalidRangeSocket.flush();
    QTRY_VERIFY_WITH_TIMEOUT(
        invalidRangeSocket.bytesAvailable() > 0 || invalidRangeSocket.state() == QAbstractSocket::UnconnectedState,
        2000);
    QByteArray invalidRangeResponse = invalidRangeSocket.readAll();
    while (invalidRangeSocket.waitForReadyRead(50)) {
        invalidRangeResponse += invalidRangeSocket.readAll();
    }
    QVERIFY(invalidRangeResponse.startsWith("HTTP/1.1 416 Range Not Satisfiable\r\n"));
    QVERIFY(invalidRangeResponse.contains("Content-Range: bytes */13\r\n"));

    QTcpSocket forbiddenSocket;
    forbiddenSocket.connectToHost(QHostAddress::LocalHost, controller.m_playbackServer.serverPort());
    QVERIFY(forbiddenSocket.waitForConnected(2000));
    forbiddenSocket.write(
        "GET /test-session/../settings.json HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "\r\n");
    forbiddenSocket.flush();
    QTRY_VERIFY_WITH_TIMEOUT(
        forbiddenSocket.bytesAvailable() > 0 || forbiddenSocket.state() == QAbstractSocket::UnconnectedState,
        2000);
    const auto forbiddenResponse = forbiddenSocket.readAll();
    QVERIFY(forbiddenResponse.startsWith("HTTP/1.1 404 Not Found\r\n"));
}

void AppModelTests::timeshiftControllerStartupCleanupOnlyRemovesManagedSessionDirectories()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const auto storageRoot = tempDir.filePath(QStringLiteral("shared-root"));
    const auto managedSessionDir = QDir(storageRoot).filePath(QStringLiteral("managed-session"));
    const auto unrelatedDir = QDir(storageRoot).filePath(QStringLiteral("family-videos"));
    const auto legacyLikeDir = QDir(storageRoot).filePath(
        QStringLiteral("12345678-1234-1234-1234-123456789abc_42_20260331112233444"));

    QVERIFY(QDir().mkpath(managedSessionDir));
    QVERIFY(QDir().mkpath(unrelatedDir));
    QVERIFY(QDir().mkpath(legacyLikeDir));

    QFile managedMarker(QDir(managedSessionDir).filePath(QStringLiteral(".okiltv-timeshift-session")));
    QVERIFY(managedMarker.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    managedMarker.write("okiltv-timeshift-session\n");
    managedMarker.close();

    QFile unrelatedFile(QDir(unrelatedDir).filePath(QStringLiteral("keep.txt")));
    QVERIFY(unrelatedFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    unrelatedFile.write("keep");
    unrelatedFile.close();

    QFile legacyLikeFile(QDir(legacyLikeDir).filePath(QStringLiteral("old.ts")));
    QVERIFY(legacyLikeFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    legacyLikeFile.write("legacy");
    legacyLikeFile.close();

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    settings.current().timeshiftStorageDirectory = storageRoot;

    PlayerController playerController;
    DvrController dvrController(&settings, &playerController);
    MultiViewController multiViewController(&settings, nullptr, &playerController);
    TimeshiftController controller(&settings, &playerController, &dvrController, &multiViewController);

    QVERIFY(!QDir(managedSessionDir).exists());
    QVERIFY(QDir(unrelatedDir).exists());
    QVERIFY(QFileInfo::exists(QDir(unrelatedDir).filePath(QStringLiteral("keep.txt"))));
    QVERIFY(QDir(legacyLikeDir).exists());
    QVERIFY(QFileInfo::exists(QDir(legacyLikeDir).filePath(QStringLiteral("old.ts"))));
}

void AppModelTests::timeshiftControllerCurrentPlaybackEpochStaysOnAttachedStreamWhileDelayedLoadIsPending()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    settings.current().timeshiftEnabled = true;
    settings.current().timeshiftStorageDirectory = tempDir.filePath(QStringLiteral("timeshift"));

    PlayerController playerController;
    DvrController dvrController(&settings, &playerController);
    MultiViewController multiViewController(&settings, nullptr, &playerController);
    TimeshiftController controller(&settings, &playerController, &dvrController, &multiViewController);

    controller.m_session.emplace();
    auto &session = controller.m_session.value();
    session.playbackAttached = true;
    session.playbackLoadPending = true;
    session.attachedWindowStartEpochMs = 1000;
    session.attachedWindowEndEpochMs = 20000;
    session.pendingPlaybackAnchorEpochMs = 6000;
    session.pendingPlaybackTargetEpochMs = 11000;
    session.playlistInfo.windowStartUtc = QDateTime::fromMSecsSinceEpoch(1000, QTimeZone::UTC);
    session.playlistInfo.liveEdgeUtc = QDateTime::fromMSecsSinceEpoch(20000, QTimeZone::UTC);
    session.playlistInfo.availableSeconds = 19.0;
    session.playlistInfo.valid = true;

    QCOMPARE(controller.attachedWindowStartEpochMs(), 1000);
    QCOMPARE(controller.currentPlaybackEpochMs(), 1000);
    QCOMPARE(controller.currentPositionSeconds(), 0.0);
    QCOMPARE(controller.behindLiveSeconds(), 19.0);
}

void AppModelTests::timeshiftUserStopRequestKillsIngestImmediately()
{
#if defined(Q_OS_WIN)
    QSKIP("Timing-based immediate-kill assertion is only covered on POSIX platforms.");
#else
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    settings.current().timeshiftEnabled = true;
    settings.current().timeshiftStorageDirectory = tempDir.filePath(QStringLiteral("timeshift"));

    PlayerController playerController;
    DvrController dvrController(&settings, &playerController);
    MultiViewController multiViewController(&settings, nullptr, &playerController);
    TimeshiftController controller(&settings, &playerController, &dvrController, &multiViewController);

    controller.m_session.emplace();
    auto &session = controller.m_session.value();
    session.id = QStringLiteral("user-stop-session");
    session.channel.id = 1001;
    session.channel.profileId = QUuid::createUuid();
    session.channel.name = QStringLiteral("Timeshifted Channel");
    session.ingestProcess = startTermIgnoringProcess();
    QVERIFY(session.ingestProcess);
    QVERIFY(session.ingestProcess->waitForStarted(2000));
    const auto pid = static_cast<qint64>(session.ingestProcess->processId());
    QVERIFY(processIsAlive(pid));

    QElapsedTimer elapsed;
    elapsed.start();
    controller.handleUserStopRequest();

    QVERIFY(!controller.m_session.has_value());
    QVERIFY2(elapsed.elapsed() < 700, "User stop should force immediate kill instead of graceful linger.");
    QTRY_VERIFY_WITH_TIMEOUT(!processIsAlive(pid), 1000);
#endif
}

void AppModelTests::timeshiftUserChannelSwitchRequestKillsIngestImmediately()
{
#if defined(Q_OS_WIN)
    QSKIP("Timing-based immediate-kill assertion is only covered on POSIX platforms.");
#else
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    settings.current().timeshiftEnabled = true;
    settings.current().timeshiftStorageDirectory = tempDir.filePath(QStringLiteral("timeshift"));

    PlayerController playerController;
    DvrController dvrController(&settings, &playerController);
    MultiViewController multiViewController(&settings, nullptr, &playerController);
    TimeshiftController controller(&settings, &playerController, &dvrController, &multiViewController);

    controller.m_session.emplace();
    auto &session = controller.m_session.value();
    session.id = QStringLiteral("user-switch-session");
    session.channel.id = 1002;
    session.channel.profileId = QUuid::createUuid();
    session.channel.name = QStringLiteral("Source Channel");
    session.ingestProcess = startTermIgnoringProcess();
    QVERIFY(session.ingestProcess);
    QVERIFY(session.ingestProcess->waitForStarted(2000));
    const auto pid = static_cast<qint64>(session.ingestProcess->processId());
    QVERIFY(processIsAlive(pid));

    Channel nextChannel;
    nextChannel.id = 1003;
    nextChannel.profileId = QUuid::createUuid();
    nextChannel.name = QStringLiteral("Destination Channel");
    nextChannel.streamUrl = QStringLiteral("http://127.0.0.1/destination");

    QElapsedTimer elapsed;
    elapsed.start();
    controller.handleUserChannelSwitchRequest(nextChannel);

    QVERIFY(!controller.m_session.has_value());
    QVERIFY2(elapsed.elapsed() < 700, "User channel switch should force immediate kill instead of graceful linger.");
    QTRY_VERIFY_WITH_TIMEOUT(!processIsAlive(pid), 1000);
#endif
}

void AppModelTests::multiviewPictureInPictureEmptyOpenAssignsFocusedSecondaryAndClosesOnToggle()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    QVERIFY(harness.multiViewController->togglePictureInPicture(-1));
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("pip"));
    QCOMPARE(harness.multiViewController->focusedTileIndex(), 1);

    const auto emptyTiles = harness.multiViewController->tiles();
    QCOMPARE(emptyTiles.size(), 2);
    QCOMPARE(emptyTiles.at(1).toMap().value(QStringLiteral("isEmpty")).toBool(), true);
    QCOMPARE(emptyTiles.at(1).toMap().value(QStringLiteral("channelId")).toInt(), -1);

    QVERIFY(harness.channelListModel->activateById(channels.last().id));
    QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), channels.first().id);
    QTRY_COMPARE(
        harness.multiViewController->tiles().at(1).toMap().value(QStringLiteral("channelId")).toInt(),
        channels.last().id);

    QVERIFY(harness.multiViewController->togglePictureInPicture(-1));
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("off"));
}

void AppModelTests::multiviewControllerOpensPictureInPictureGridAndSwapsChannels()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    QVERIFY(harness.multiViewController->togglePictureInPicture(channels.last().id));
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("pip"));
    QCOMPARE(harness.multiViewController->maxTiles(), 2);
    QCOMPARE(harness.multiViewController->focusedTileIndex(), 0);

    const auto tiles = harness.multiViewController->tiles();
    QCOMPARE(tiles.size(), 2);
    QCOMPARE(tiles.at(1).toMap().value(QStringLiteral("channelId")).toInt(), channels.last().id);
    auto *expectedPrimaryPlayer =
        tiles.at(0).toMap().value(QStringLiteral("playerObject")).value<QObject *>();
    auto *expectedSecondaryPlayer =
        tiles.at(1).toMap().value(QStringLiteral("playerObject")).value<QObject *>();
    QVERIFY(expectedPrimaryPlayer != nullptr);
    QVERIFY(expectedSecondaryPlayer != nullptr);
    QVERIFY(expectedPrimaryPlayer != expectedSecondaryPlayer);

    auto expectedPrimaryId = channels.first().id;
    auto expectedSecondaryId = channels.last().id;
    for (int swapIndex = 0; swapIndex < 3; ++swapIndex) {
        QVERIFY(harness.multiViewController->swapPrimaryWithPictureInPicture());
        std::swap(expectedPrimaryId, expectedSecondaryId);
        std::swap(expectedPrimaryPlayer, expectedSecondaryPlayer);
        QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), expectedPrimaryId);
        const auto swappedTiles = harness.multiViewController->tiles();
        QCOMPARE(swappedTiles.at(1).toMap().value(QStringLiteral("channelId")).toInt(), expectedSecondaryId);
        QCOMPARE(swappedTiles.at(0).toMap().value(QStringLiteral("playerObject")).value<QObject *>(), expectedPrimaryPlayer);
        QCOMPARE(swappedTiles.at(1).toMap().value(QStringLiteral("playerObject")).value<QObject *>(), expectedSecondaryPlayer);
        QCOMPARE(swappedTiles.at(1).toMap().value(QStringLiteral("playerState")).toString(), QStringLiteral("ready"));
    }

    harness.settingsController->setMultiviewMaxTiles(6);
    harness.settingsController->save();
    QVERIFY(harness.multiViewController->toggleGrid());
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("grid3x2"));
    QCOMPARE(harness.multiViewController->maxTiles(), 6);
    QCOMPARE(harness.multiViewController->layoutColumns(), 3);
    QCOMPARE(harness.multiViewController->layoutRows(), 2);

    QVERIFY(harness.multiViewController->toggleGrid());
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("off"));
}

void AppModelTests::appControllerRoutesActivationToFocusedMultiviewTile()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    harness.settingsController->setMultiviewMaxTiles(2);
    harness.settingsController->save();
    QVERIFY(harness.multiViewController->toggleGrid());
    harness.multiViewController->focusNextTile();
    QVERIFY(harness.channelListModel->activateById(channels.last().id));

    QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), channels.first().id);
    const auto tiles = harness.multiViewController->tiles();
    QCOMPARE(tiles.at(1).toMap().value(QStringLiteral("channelId")).toInt(), channels.last().id);
}

void AppModelTests::appControllerSameChannelActivationSkipsRetuneWhileActiveOrInFlight()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    const auto channelId = channels.first().id;

    QVERIFY(harness.channelListModel->activateById(channelId));
    QVERIFY(harness.playerController->channelSwitchInProgress());

    QSignalSpy playbackActivationSpy(harness.playerController.get(), &PlayerController::playbackChannelActivated);
    QVERIFY(harness.channelListModel->activateById(channelId));
    QCOMPARE(playbackActivationSpy.count(), 0);

    QVERIFY(QMetaObject::invokeMethod(harness.playerController->player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        harness.playerController->player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));
    QVERIFY(harness.playerController->isPlaying());

    QVERIFY(harness.channelListModel->activateById(channelId));
    QCOMPARE(playbackActivationSpy.count(), 0);

    QVERIFY(QMetaObject::invokeMethod(harness.playerController->player(), "playbackEnded", Qt::DirectConnection));
    QVERIFY(!harness.playerController->isPlaying());

    QVERIFY(harness.channelListModel->activateById(channelId));
    QCOMPARE(playbackActivationSpy.count(), 1);
}

void AppModelTests::appControllerSameChannelActivationRetunesLiveWhenCatchupActive()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    channels[0].catchupSupported = true;
    channels[0].catchupWindowHours = 48;
    harness.channelListModel->setChannels(channels, {
        { QStringLiteral("News"), QStringLiteral("News"), 0 },
        { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
    });

    const auto channel = channels.first();
    harness.playerController->playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup"),
        QStringLiteral("Past Show"));
    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("catchup"));

    QSignalSpy playbackActivationSpy(harness.playerController.get(), &PlayerController::playbackChannelActivated);
    QVERIFY(harness.channelListModel->activateById(channel.id));

    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("live"));
    QCOMPARE(harness.playerController->currentPlaybackUrl(), channel.streamUrl);
    QVERIFY(playbackActivationSpy.count() >= 1);
}

void AppModelTests::appControllerSourceActivationStopsCatchupBeforeCrossProfileLoad()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    channels[0].catchupSupported = true;
    channels[0].catchupWindowHours = 48;
    harness.channelListModel->setChannels(channels, {
        { QStringLiteral("News"), QStringLiteral("News"), 0 },
        { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
    });

    const auto sourceAChannel = channels.first();
    harness.playerController->playCatchupChannel(
        sourceAChannel,
        QStringLiteral("http://127.0.0.1/catchup"),
        QStringLiteral("Past Show"));
    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("catchup"));
    QCOMPARE(harness.playerController->currentPlaybackUrl(), QStringLiteral("http://127.0.0.1/catchup"));

    const auto secondPlaylistPath = harness.tempDir.filePath(QStringLiteral("playlist-second-switch.m3u"));
    QFile secondPlaylist(secondPlaylistPath);
    QVERIFY(secondPlaylist.open(QIODevice::WriteOnly | QIODevice::Truncate));
    secondPlaylist.write(
        "#EXTM3U\n"
        "#EXTINF:-1 tvg-id=\"channel.three\" tvg-name=\"Channel Three\" group-title=\"News\",Channel Three\n"
        "http://127.0.0.1/channel-three\n");
    secondPlaylist.close();

    const auto secondProfileId = harness.profilesModel->addM3uFileProfile(
        QStringLiteral("Second Playlist"),
        secondPlaylistPath,
        QString {});
    QVERIFY(!secondProfileId.isEmpty());

    QSignalSpy profileLoadSpy(harness.appController.get(), &AppController::profileLoadFinished);
    QVERIFY(harness.profilesModel->selectProfile(secondProfileId));
    QTRY_VERIFY_WITH_TIMEOUT(profileLoadSpy.count() > 0, 8000);

    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("live"));
    QVERIFY(!harness.playerController->currentChannelValue().has_value());
    QCOMPARE(harness.playerController->currentPlaybackUrl(), QString {});
    QCOMPARE(harness.appController->activeProfileId(), secondProfileId);
    QCOMPARE(harness.profilesModel->activeProfileId(), secondProfileId);
}

void AppModelTests::appControllerPlayCatchupRejectsFutureProgram()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    channels[0].catchupSupported = true;
    channels[0].catchupWindowHours = 48;
    harness.channelListModel->setChannels(channels, {
        { QStringLiteral("News"), QStringLiteral("News"), 0 },
        { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
    });
    harness.guideStateModel->setChannels(channels);

    const auto futureStart = QDateTime::currentDateTimeUtc().addSecs(1800);
    const auto futureStop = futureStart.addSecs(1800);
    const auto channelVariant = toVariantMap(channels.first());
    const QVariantMap programVariant {
        { QStringLiteral("channelId"), channels.first().tvgId },
        { QStringLiteral("title"), QStringLiteral("Future Show") },
        { QStringLiteral("start"), futureStart.toString(Qt::ISODateWithMs) },
        { QStringLiteral("stop"), futureStop.toString(Qt::ISODateWithMs) }
    };

    harness.appController->playCatchup(channelVariant, programVariant);

    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("live"));
    QVERIFY(harness.appController->statusText().contains(QStringLiteral("after the programme starts")));
}

void AppModelTests::appControllerPlayCatchupRejectsProgrammeChannelMismatch()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    channels[0].catchupSupported = true;
    channels[0].catchupWindowHours = 48;
    harness.channelListModel->setChannels(channels, {
        { QStringLiteral("News"), QStringLiteral("News"), 0 },
        { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
    });
    harness.guideStateModel->setChannels(channels);

    const auto programStart = QDateTime::currentDateTimeUtc().addSecs(-1800);
    const auto programStop = programStart.addSecs(1800);
    const auto channelVariant = toVariantMap(channels.first());
    const QVariantMap programVariant {
        { QStringLiteral("channelId"), QStringLiteral("stale.channel.id") },
        { QStringLiteral("title"), QStringLiteral("Past Show") },
        { QStringLiteral("start"), programStart.toString(Qt::ISODateWithMs) },
        { QStringLiteral("stop"), programStop.toString(Qt::ISODateWithMs) }
    };

    harness.appController->playCatchup(channelVariant, programVariant);

    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("live"));
    QVERIFY(harness.appController->statusText().contains(QStringLiteral("does not belong to the selected channel")));
}

void AppModelTests::appControllerPlayCatchupResolvesLegacyM3uTimeshiftTemplate()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    channels[0].catchupSupported = true;
    channels[0].catchupWindowHours = 72;
    channels[0].catchupMode = QStringLiteral("append");
    channels[0].catchupSourceTemplate = QStringLiteral("utc={utc}&lutc={lutc}");
    channels[0].streamUrl = QStringLiteral("http://127.0.0.1/channel-one?existing=1");
    harness.channelListModel->setChannels(channels, {
        { QStringLiteral("News"), QStringLiteral("News"), 0 },
        { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
    });
    harness.guideStateModel->setChannels(channels);

    const auto programStart = QDateTime::currentDateTimeUtc().addSecs(-7200);
    const auto programStop = programStart.addSecs(1800);
    const auto channelVariant = toVariantMap(channels.first());
    const QVariantMap programVariant {
        { QStringLiteral("channelId"), channels.first().tvgId },
        { QStringLiteral("title"), QStringLiteral("Past Show") },
        { QStringLiteral("start"), programStart.toString(Qt::ISODateWithMs) },
        { QStringLiteral("stop"), programStop.toString(Qt::ISODateWithMs) }
    };

    harness.appController->playCatchup(channelVariant, programVariant);

    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("catchup"));
    QCOMPARE(
        harness.playerController->currentPlaybackUrl(),
        QStringLiteral("http://127.0.0.1/channel-one?existing=1&utc=%1&lutc=%2")
            .arg(programStart.toSecsSinceEpoch())
            .arg(programStop.toSecsSinceEpoch()));

    harness.playerController->returnToLiveFromCatchup();
    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("live"));
    QCOMPARE(harness.playerController->currentPlaybackUrl(), channels.first().streamUrl);
}

void AppModelTests::appControllerPlayCatchupGuideUtcPayloadResolvesExpectedEpochUrl()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    channels[0].catchupSupported = true;
    channels[0].catchupWindowHours = 72;
    channels[0].catchupMode = QStringLiteral("append");
    channels[0].catchupSourceTemplate = QStringLiteral("utc={utc}&lutc={lutc}");
    channels[0].streamUrl = QStringLiteral("http://provider/live.m3u8?token=secret_token");
    harness.channelListModel->setChannels(channels, {
        { QStringLiteral("News"), QStringLiteral("News"), 0 },
        { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
    });
    harness.guideStateModel->setChannels(channels);

    const auto programStart = QDateTime::fromString(QStringLiteral("2026-05-18T07:45:00Z"), Qt::ISODate);
    const auto programStop = QDateTime::fromString(QStringLiteral("2026-05-18T08:45:00Z"), Qt::ISODate);
    QVERIFY(programStart.isValid());
    QVERIFY(programStop.isValid());
    const auto channelVariant = toVariantMap(channels.first());
    const QVariantMap programVariant {
        { QStringLiteral("channelId"), channels.first().tvgId },
        { QStringLiteral("title"), QStringLiteral("Past Show") },
        { QStringLiteral("start"), programStart.toString(Qt::ISODateWithMs) },
        { QStringLiteral("stop"), programStop.toString(Qt::ISODateWithMs) }
    };

    harness.appController->playCatchup(channelVariant, programVariant);

    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("catchup"));
    QCOMPARE(
        harness.playerController->currentPlaybackUrl(),
        QStringLiteral("http://provider/live.m3u8?token=secret_token&utc=1779090300&lutc=1779093900"));
}

void AppModelTests::appControllerPlayCatchupGuideOffsetPayloadResolvesExpectedEpochUrl()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    channels[0].catchupSupported = true;
    channels[0].catchupWindowHours = 72;
    channels[0].catchupMode = QStringLiteral("append");
    channels[0].catchupSourceTemplate = QStringLiteral("utc={utc}&lutc={lutc}");
    channels[0].streamUrl = QStringLiteral("http://provider/live.m3u8?token=secret_token");
    harness.channelListModel->setChannels(channels, {
        { QStringLiteral("News"), QStringLiteral("News"), 0 },
        { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
    });
    harness.guideStateModel->setChannels(channels);

    const auto channelVariant = toVariantMap(channels.first());
    const QVariantMap programVariant {
        { QStringLiteral("channelId"), channels.first().tvgId },
        { QStringLiteral("title"), QStringLiteral("Past Show") },
        { QStringLiteral("start"), QStringLiteral("2026-05-18T09:45:00+02:00") },
        { QStringLiteral("stop"), QStringLiteral("2026-05-18T10:45:00+02:00") }
    };

    harness.appController->playCatchup(channelVariant, programVariant);

    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("catchup"));
    QCOMPARE(
        harness.playerController->currentPlaybackUrl(),
        QStringLiteral("http://provider/live.m3u8?token=secret_token&utc=1779090300&lutc=1779093900"));
}

void AppModelTests::appControllerPlayCatchupXtreamPreResolvesRedirectUrl()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const auto port = server.serverPort();
    QVERIFY(QObject::connect(&server, &QTcpServer::newConnection, &server, [&server]() {
        while (server.hasPendingConnections()) {
            QTcpSocket *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
                const auto request = socket->readAll();
                if (request.startsWith("HEAD ")) {
                    const QByteArray response(
                        "HTTP/1.1 302 Found\r\n"
                        "Location: /archive/final.m3u8\r\n"
                        "Connection: close\r\n\r\n");
                    socket->write(response);
                } else {
                    const QByteArray response(
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n");
                    socket->write(response);
                }
                socket->disconnectFromHost();
            });
        }
    }));

    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    auto &profile = harness.settings->current().profiles.first();
    profile.type = ProfileType::Xtream;
    profile.xtreamBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    profile.xtreamUsername = QStringLiteral("user");
    profile.xtreamPassword = QStringLiteral("pass");
    harness.settings->save();

    auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    channels[0].source = ChannelSource::Xtream;
    channels[0].id = 952;
    channels[0].catchupSupported = true;
    channels[0].catchupWindowHours = 72;
    channels[0].streamUrl = QStringLiteral("http://127.0.0.1:%1/live/stream").arg(port);
    harness.channelListModel->setChannels(channels, {
        { QStringLiteral("News"), QStringLiteral("News"), 0 },
        { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
    });

    const auto programStart = QDateTime::currentDateTimeUtc().addSecs(-3600);
    const auto programStop = programStart.addSecs(1800);
    const auto channelVariant = toVariantMap(channels.first());
    const QVariantMap programVariant {
        { QStringLiteral("channelId"), channels.first().tvgId },
        { QStringLiteral("title"), QStringLiteral("Past Show") },
        { QStringLiteral("start"), programStart.toString(Qt::ISODateWithMs) },
        { QStringLiteral("stop"), programStop.toString(Qt::ISODateWithMs) }
    };

    harness.appController->playCatchup(channelVariant, programVariant);

    QTRY_COMPARE_WITH_TIMEOUT(harness.playerController->playbackMode(), QStringLiteral("catchup"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(
        harness.playerController->currentPlaybackUrl(),
        QStringLiteral("http://127.0.0.1:%1/archive/final.m3u8").arg(port),
        5000);
}

void AppModelTests::appControllerPlayCatchupXtreamRedirectResolutionDoesNotBlockUiThread()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const auto port = server.serverPort();
    QVERIFY(QObject::connect(&server, &QTcpServer::newConnection, &server, [&server]() {
        while (server.hasPendingConnections()) {
            QTcpSocket *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
                const auto request = socket->readAll();
                if (!request.startsWith("HEAD ")) {
                    const QByteArray response(
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n");
                    socket->write(response);
                    socket->disconnectFromHost();
                    return;
                }

                QTimer::singleShot(1200, socket, [socket]() {
                    const QByteArray response(
                        "HTTP/1.1 302 Found\r\n"
                        "Location: /archive/delayed-final.m3u8\r\n"
                        "Connection: close\r\n\r\n");
                    socket->write(response);
                    socket->disconnectFromHost();
                });
            });
        }
    }));

    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    auto &profile = harness.settings->current().profiles.first();
    profile.type = ProfileType::Xtream;
    profile.xtreamBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    profile.xtreamUsername = QStringLiteral("user");
    profile.xtreamPassword = QStringLiteral("pass");
    harness.settings->save();

    auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    channels[0].source = ChannelSource::Xtream;
    channels[0].id = 1952;
    channels[0].catchupSupported = true;
    channels[0].catchupWindowHours = 72;
    channels[0].streamUrl = QStringLiteral("http://127.0.0.1:%1/live/stream").arg(port);
    harness.channelListModel->setChannels(channels, {
        { QStringLiteral("News"), QStringLiteral("News"), 0 },
        { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
    });

    const auto programStart = QDateTime::currentDateTimeUtc().addSecs(-3600);
    const auto programStop = programStart.addSecs(1800);
    const auto channelVariant = toVariantMap(channels.first());
    const QVariantMap programVariant {
        { QStringLiteral("channelId"), channels.first().tvgId },
        { QStringLiteral("title"), QStringLiteral("Past Show") },
        { QStringLiteral("start"), programStart.toString(Qt::ISODateWithMs) },
        { QStringLiteral("stop"), programStop.toString(Qt::ISODateWithMs) }
    };

    QElapsedTimer elapsed;
    elapsed.start();
    harness.appController->playCatchup(channelVariant, programVariant);
    QVERIFY(elapsed.elapsed() < 400);

    QTRY_COMPARE_WITH_TIMEOUT(harness.playerController->playbackMode(), QStringLiteral("catchup"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(
        harness.playerController->currentPlaybackUrl(),
        QStringLiteral("http://127.0.0.1:%1/archive/delayed-final.m3u8").arg(port),
        5000);
}

void AppModelTests::appControllerPlayCatchupXtreamRedirectFailureFallsBackToOriginalUrl()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    auto &profile = harness.settings->current().profiles.first();
    profile.type = ProfileType::Xtream;
    profile.xtreamBaseUrl = QStringLiteral("http://127.0.0.1:9");
    profile.xtreamUsername = QStringLiteral("user");
    profile.xtreamPassword = QStringLiteral("pass");
    harness.settings->save();

    auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    channels[0].source = ChannelSource::Xtream;
    channels[0].id = 953;
    channels[0].catchupSupported = true;
    channels[0].catchupWindowHours = 72;
    harness.channelListModel->setChannels(channels, {
        { QStringLiteral("News"), QStringLiteral("News"), 0 },
        { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
    });

    const auto programStart = QDateTime::currentDateTimeUtc().addSecs(-3600);
    const auto programStop = programStart.addSecs(1800);
    const auto channelVariant = toVariantMap(channels.first());
    const QVariantMap programVariant {
        { QStringLiteral("channelId"), channels.first().tvgId },
        { QStringLiteral("title"), QStringLiteral("Past Show") },
        { QStringLiteral("start"), programStart.toString(Qt::ISODateWithMs) },
        { QStringLiteral("stop"), programStop.toString(Qt::ISODateWithMs) }
    };

    CatchupUrlResolver resolver(profile);
    QString reason;
    const auto target = resolver.resolve(channels.first(), EpgEntry {
        channels.first().tvgId,
        QStringLiteral("Past Show"),
        QString(),
        QString(),
        programStart,
        programStop,
        QString()
    }, &reason);
    QVERIFY(target.has_value());

    harness.appController->playCatchup(channelVariant, programVariant);
    QTRY_COMPARE_WITH_TIMEOUT(harness.playerController->playbackMode(), QStringLiteral("catchup"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.playerController->currentPlaybackUrl(), target->url, 5000);
}

void AppModelTests::appControllerPlayCatchupRejectsUnresolvedTemplate()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    channels[0].catchupSupported = true;
    channels[0].catchupWindowHours = 72;
    channels[0].catchupMode = QStringLiteral("append");
    channels[0].catchupSourceTemplate = QStringLiteral("utc={utc}&lutc={unknown}");
    channels[0].streamUrl = QStringLiteral("http://provider/live.m3u8?token=secret_token");
    harness.channelListModel->setChannels(channels, {
        { QStringLiteral("News"), QStringLiteral("News"), 0 },
        { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
    });
    harness.guideStateModel->setChannels(channels);

    const auto programStart = QDateTime::fromString(QStringLiteral("2026-05-18T07:45:00Z"), Qt::ISODate);
    const auto programStop = QDateTime::fromString(QStringLiteral("2026-05-18T08:45:00Z"), Qt::ISODate);
    QVERIFY(programStart.isValid());
    QVERIFY(programStop.isValid());

    harness.playerController->playChannel(channels[0]);

    const auto channelVariant = toVariantMap(channels.first());
    const QVariantMap programVariant {
        { QStringLiteral("channelId"), channels.first().tvgId },
        { QStringLiteral("title"), QStringLiteral("Past Show") },
        { QStringLiteral("start"), programStart.toString(Qt::ISODateWithMs) },
        { QStringLiteral("stop"), programStop.toString(Qt::ISODateWithMs) }
    };

    harness.appController->playCatchup(channelVariant, programVariant);

    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("live"));
    QVERIFY(harness.appController->statusText().contains(QStringLiteral("unresolved placeholders")));
}

void AppModelTests::xtreamProfileRefreshKeepsStoredTimezoneWhenResponseMissingTimezone()
{
    auto network = std::make_shared<MockNetworkAccess>();
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt, network));

    auto &profile = harness.settings->current().profiles.first();
    profile.type = ProfileType::Xtream;
    profile.name = QStringLiteral("Xtream TZ");
    profile.xtreamBaseUrl = QStringLiteral("https://xtream.example");
    profile.xtreamUsername = QStringLiteral("alice");
    profile.xtreamPassword = QStringLiteral("secret");
    profile.xtreamServerTimezone = QStringLiteral("UTC");
    profile.m3uFilePath.clear();
    profile.m3uUrl.clear();
    harness.settings->save();
    harness.profilesModel->reload();

    const auto profileId = guidToString(profile.id);
    const QUrl authUrl(QStringLiteral("https://xtream.example/player_api.php?username=alice&password=secret"));
    const QUrl categoriesUrl(QStringLiteral("https://xtream.example/player_api.php?username=alice&password=secret&action=get_live_categories"));
    const QUrl streamsUrl(QStringLiteral("https://xtream.example/player_api.php?username=alice&password=secret&action=get_live_streams"));

    network->setResponse(
        authUrl,
        { QByteArrayLiteral(R"json({
            "user_info": { "auth": 1 },
            "server_info": { "timezone": "Asia/Dubai" }
        })json"), {}, 0 });
    network->setResponse(
        categoriesUrl,
        { QByteArrayLiteral(R"json([
            { "category_id": "12", "category_name": "News", "parent_id": 0 }
        ])json"), {}, 0 });
    network->setResponse(
        streamsUrl,
        { QByteArrayLiteral(R"json([
            {
                "stream_id": 55,
                "name": "Archive News",
                "epg_channel_id": "archive.news",
                "category_id": "12",
                "stream_icon": "http://logo.png",
                "num": "9",
                "container_extension": "ts",
                "tv_archive": 1,
                "tv_archive_duration": 7
            }
        ])json"), {}, 0 });

    QSignalSpy profileLoadSpy(harness.appController.get(), &AppController::profileLoadFinished);
    harness.appController->loadProfile(profileId);
    QTRY_VERIFY_WITH_TIMEOUT(profileLoadSpy.count() > 0, 8000);
    QCOMPARE(harness.settings->current().profiles.first().xtreamServerTimezone, QStringLiteral("Asia/Dubai"));

    network->setResponse(
        authUrl,
        { QByteArrayLiteral(R"json({
            "user_info": { "auth": 1 },
            "server_info": { }
        })json"), {}, 0 });
    profileLoadSpy.clear();
    harness.appController->loadProfile(profileId);
    QTRY_VERIFY_WITH_TIMEOUT(profileLoadSpy.count() > 0, 8000);
    QCOMPARE(harness.settings->current().profiles.first().xtreamServerTimezone, QStringLiteral("Asia/Dubai"));
}

void AppModelTests::scheduledSourceAutoRefreshTriggersAtExactIntervalBoundary()
{
    auto network = std::make_shared<MockNetworkAccess>();
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt, network));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto url = QUrl(QStringLiteral("https://example.test/auto-refresh.m3u"));
    network->setResponse(
        url,
        { QByteArrayLiteral(
              "#EXTM3U\n"
              "#EXTINF:-1 tvg-id=\"auto.one\" group-title=\"News\",Auto One\n"
              "http://stream/auto-one\n"),
          {},
          0 });

    auto &profile = harness.settings->current().profiles.first();
    profile.type = ProfileType::M3UUrl;
    profile.m3uUrl = url.toString();
    profile.m3uFilePath.clear();
    profile.autoRefreshIntervalHours = 1;
    profile.lastRefreshed = QDateTime::currentDateTimeUtc().addSecs(-(60 * 60) + 1);
    harness.settings->save();

    harness.appController->triggerScheduledSourceAutoRefresh();
    QTest::qWait(100);
    QCOMPARE(network->callCount(url), 0);

    profile.lastRefreshed = QDateTime::currentDateTimeUtc().addSecs(-(60 * 60));
    harness.settings->save();
    harness.appController->triggerScheduledSourceAutoRefresh();

    QTRY_COMPARE_WITH_TIMEOUT(network->callCount(url), 1, 5000);
}

void AppModelTests::sourceRefreshFailureWithCachedFallbackKeepsPreviousLastRefreshed()
{
    auto network = std::make_shared<MockNetworkAccess>();
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt, network));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto url = QUrl(QStringLiteral("https://example.test/source-failure.m3u"));
    network->setResponse(url, { {}, QStringLiteral("simulated refresh failure"), 0 });

    auto &profile = harness.settings->current().profiles.first();
    const auto previousRefresh = QDateTime::fromString(QStringLiteral("2026-05-19T10:00:00Z"), Qt::ISODate);
    QVERIFY(previousRefresh.isValid());
    profile.type = ProfileType::M3UUrl;
    profile.m3uUrl = url.toString();
    profile.m3uFilePath.clear();
    profile.lastRefreshed = previousRefresh;
    harness.settings->save();
    harness.profilesModel->reload();

    const auto profileId = guidToString(profile.id);
    QSignalSpy profileLoadSpy(harness.appController.get(), &AppController::profileLoadFinished);
    harness.appController->loadProfile(profileId);
    QTRY_VERIFY_WITH_TIMEOUT(profileLoadSpy.count() > 0, 8000);

    QCOMPARE(harness.settings->current().profiles.first().lastRefreshed, previousRefresh);
    QVERIFY(harness.appController->statusText().contains(QStringLiteral("Using cached channels after refresh failure")));
}

void AppModelTests::profileRefreshPrunesRemovedChannelsFromDatabase()
{
    auto network = std::make_shared<MockNetworkAccess>();
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt, network));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto url = QUrl(QStringLiteral("https://example.test/prune.m3u"));
    auto &profile = harness.settings->current().profiles.first();
    profile.type = ProfileType::M3UUrl;
    profile.m3uUrl = url.toString();
    profile.m3uFilePath.clear();
    harness.settings->save();
    harness.profilesModel->reload();

    network->setResponse(
        url,
        { QByteArrayLiteral(
              "#EXTM3U\n"
              "#EXTINF:-1 tvg-id=\"prune.one\" group-title=\"News\",Prune One\n"
              "http://stream/prune-one\n"
              "#EXTINF:-1 tvg-id=\"prune.two\" group-title=\"News\",Prune Two\n"
              "http://stream/prune-two\n"
              "#EXTINF:-1 tvg-id=\"prune.three\" group-title=\"News\",Prune Three\n"
              "http://stream/prune-three\n"),
          {},
          0 });

    const auto profileId = guidToString(profile.id);
    QSignalSpy profileLoadSpy(harness.appController.get(), &AppController::profileLoadFinished);
    harness.appController->loadProfile(profileId);
    QTRY_VERIFY_WITH_TIMEOUT(profileLoadSpy.count() > 0, 8000);
    QCOMPARE(harness.database->loadChannels(profile.id).size(), 3);

    network->setResponse(
        url,
        { QByteArrayLiteral(
              "#EXTM3U\n"
              "#EXTINF:-1 tvg-id=\"prune.one\" group-title=\"News\",Prune One Updated\n"
              "http://stream/prune-one-updated\n"),
          {},
          0 });

    profileLoadSpy.clear();
    harness.appController->loadProfile(profileId);
    QTRY_VERIFY_WITH_TIMEOUT(profileLoadSpy.count() > 0, 8000);

    const auto loaded = harness.database->loadChannels(profile.id);
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.first().id, 0);
    QCOMPARE(loaded.first().name, QStringLiteral("Prune One Updated"));
}

void AppModelTests::multiviewExitPromotesFocusedSecondaryToPrimary()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    QVERIFY(harness.multiViewController->togglePictureInPicture(channels.last().id));
    harness.multiViewController->focusNextTile();

    harness.multiViewController->exitMultiView();

    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("off"));
    QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), channels.last().id);
}

void AppModelTests::multiviewGridToggleExitWithoutRetainStillPerformsFullCleanup()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    harness.settingsController->setMultiviewRetainSelectionOnPromotion(false);
    harness.settingsController->setMultiviewMaxTiles(4);
    harness.settingsController->save();

    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    QVERIFY(harness.multiViewController->toggleGrid());
    harness.multiViewController->focusTile(1);
    QVERIFY(harness.channelListModel->activateById(channels.last().id));

    harness.multiViewController->focusTile(1);
    QVERIFY(harness.multiViewController->toggleGrid());

    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("off"));
    QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), channels.last().id);
    QVERIFY(!harness.multiViewController->retainedSelectionActive());
    QCOMPARE(static_cast<int>(harness.multiViewController->m_secondarySlots.size()), 0);
}

void AppModelTests::multiviewGridToggleWithRetainSoftPromotesAndKeepsSecondaryStreams()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    harness.settingsController->setMultiviewRetainSelectionOnPromotion(true);
    harness.settingsController->setMultiviewMaxTiles(4);
    harness.settingsController->save();

    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    QVERIFY(harness.multiViewController->toggleGrid());
    harness.multiViewController->focusTile(1);
    QVERIFY(harness.channelListModel->activateById(channels.last().id));

    harness.multiViewController->focusTile(1);
    QVERIFY(harness.multiViewController->toggleGrid());

    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("off"));
    QVERIFY(harness.multiViewController->retainedSelectionActive());
    QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), channels.last().id);
    QVERIFY(harness.playerController->usingSharedPlayback());

    bool foundOriginalPrimary = false;
    for (const auto &slot : harness.multiViewController->m_secondarySlots) {
        if (slot.channel.has_value() && slot.channel->id == channels.first().id) {
            foundOriginalPrimary = true;
            break;
        }
    }
    QVERIFY(foundOriginalPrimary);
    QVERIFY(harness.multiViewController->m_retiredPlayers.empty());
}

void AppModelTests::multiviewGridStopFocusedSecondaryKeepsFocusWithoutRetain()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    harness.settingsController->setMultiviewRetainSelectionOnPromotion(false);
    harness.settingsController->setMultiviewMaxTiles(4);
    harness.settingsController->save();

    QVERIFY(harness.channelListModel->activateById(channels.at(0).id));
    QVERIFY(harness.multiViewController->toggleGrid());
    harness.multiViewController->focusTile(1);
    QVERIFY(harness.channelListModel->activateById(channels.at(1).id));

    harness.multiViewController->focusTile(1);
    QVERIFY(harness.multiViewController->stopRetainedPromotedAndRestoreGrid());

    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("grid2x2"));
    QCOMPARE(harness.multiViewController->focusedTileIndex(), 1);
    const auto tilesAfterStop = harness.multiViewController->tiles();
    QCOMPARE(tilesAfterStop.at(0).toMap().value(QStringLiteral("channelId")).toInt(), channels.at(0).id);
    QCOMPARE(tilesAfterStop.at(1).toMap().value(QStringLiteral("isEmpty")).toBool(), true);
    QCOMPARE(tilesAfterStop.at(1).toMap().value(QStringLiteral("channelId")).toInt(), -1);
    QVERIFY(tilesAfterStop.at(1).toMap().value(QStringLiteral("isFocused")).toBool());
}

void AppModelTests::multiviewGridStopFocusedSecondaryKeepsFocusWithRetain()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    harness.settingsController->setMultiviewRetainSelectionOnPromotion(true);
    harness.settingsController->setMultiviewMaxTiles(4);
    harness.settingsController->save();

    QVERIFY(harness.channelListModel->activateById(channels.at(0).id));
    QVERIFY(harness.multiViewController->toggleGrid());
    harness.multiViewController->focusTile(1);
    QVERIFY(harness.channelListModel->activateById(channels.at(1).id));

    harness.multiViewController->focusTile(1);
    QVERIFY(harness.multiViewController->stopRetainedPromotedAndRestoreGrid());

    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("grid2x2"));
    QCOMPARE(harness.multiViewController->focusedTileIndex(), 1);
    const auto tilesAfterStop = harness.multiViewController->tiles();
    QCOMPARE(tilesAfterStop.at(0).toMap().value(QStringLiteral("channelId")).toInt(), channels.at(0).id);
    QCOMPARE(tilesAfterStop.at(1).toMap().value(QStringLiteral("isEmpty")).toBool(), true);
    QCOMPARE(tilesAfterStop.at(1).toMap().value(QStringLiteral("channelId")).toInt(), -1);
    QVERIFY(tilesAfterStop.at(1).toMap().value(QStringLiteral("isFocused")).toBool());
}

void AppModelTests::multiviewGridStopLastRemainingTileReturnsToDefaultPlaybackState()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    harness.settingsController->setMultiviewRetainSelectionOnPromotion(true);
    harness.settingsController->setMultiviewMaxTiles(4);
    harness.settingsController->save();

    QVERIFY(harness.channelListModel->activateById(channels.at(0).id));
    QVERIFY(harness.multiViewController->toggleGrid());
    harness.multiViewController->focusTile(1);
    QVERIFY(harness.channelListModel->activateById(channels.at(1).id));

    harness.multiViewController->focusTile(0);
    QVERIFY(harness.multiViewController->stopRetainedPromotedAndRestoreGrid());
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("grid2x2"));
    QCOMPARE(harness.multiViewController->focusedTileIndex(), 0);
    QCOMPARE(harness.multiViewController->tiles().at(0).toMap().value(QStringLiteral("isEmpty")).toBool(), true);
    QCOMPARE(harness.multiViewController->tiles().at(1).toMap().value(QStringLiteral("channelId")).toInt(), channels.at(1).id);
    QVERIFY(!harness.playerController->currentChannelValue().has_value());

    harness.multiViewController->focusTile(1);
    QVERIFY(harness.multiViewController->stopRetainedPromotedAndRestoreGrid());
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("off"));
    QVERIFY(!harness.playerController->currentChannelValue().has_value());
    QVERIFY(!harness.multiViewController->retainedSelectionActive());
}

void AppModelTests::multiviewRetainedSelectionReopenRestoresWarmSecondarySlots()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    harness.settingsController->setMultiviewRetainSelectionOnPromotion(true);
    harness.settingsController->setMultiviewMaxTiles(4);
    harness.settingsController->save();

    QVERIFY(harness.channelListModel->activateById(channels.at(0).id));
    QVERIFY(harness.multiViewController->toggleGrid());
    harness.multiViewController->focusTile(1);
    QVERIFY(harness.channelListModel->activateById(channels.at(1).id));

    const auto tilesBeforeClose = harness.multiViewController->tiles();
    auto *slotOnePlayerBeforeClose =
        tilesBeforeClose.at(1).toMap().value(QStringLiteral("playerObject")).value<QObject *>();
    QVERIFY(slotOnePlayerBeforeClose != nullptr);

    harness.multiViewController->focusTile(1);
    QVERIFY(harness.multiViewController->toggleGrid());
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("off"));
    QVERIFY(harness.multiViewController->retainedSelectionActive());
    QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), channels.at(1).id);

    QVERIFY(harness.multiViewController->toggleGrid());
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("grid2x2"));
    QVERIFY(!harness.multiViewController->retainedSelectionActive());
    QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), channels.at(0).id);
    QCOMPARE(harness.multiViewController->focusedTileIndex(), 1);

    const auto tilesAfterReopen = harness.multiViewController->tiles();
    QCOMPARE(tilesAfterReopen.at(0).toMap().value(QStringLiteral("channelId")).toInt(), channels.at(0).id);
    QCOMPARE(tilesAfterReopen.at(1).toMap().value(QStringLiteral("channelId")).toInt(), channels.at(1).id);
    QVERIFY(tilesAfterReopen.at(1).toMap().value(QStringLiteral("isFocused")).toBool());
    QCOMPARE(
        tilesAfterReopen.at(1).toMap().value(QStringLiteral("playerObject")).value<QObject *>(),
        slotOnePlayerBeforeClose);
}

void AppModelTests::multiviewRetainedSelectionStopReopensGridAndStopsPromotedTile()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    harness.settingsController->setMultiviewRetainSelectionOnPromotion(true);
    harness.settingsController->setMultiviewMaxTiles(4);
    harness.settingsController->save();

    QVERIFY(harness.channelListModel->activateById(channels.at(0).id));
    QVERIFY(harness.multiViewController->toggleGrid());
    harness.multiViewController->focusTile(1);
    QVERIFY(harness.channelListModel->activateById(channels.at(1).id));

    harness.multiViewController->focusTile(1);
    QVERIFY(harness.multiViewController->toggleGrid());
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("off"));
    QVERIFY(harness.multiViewController->retainedSelectionActive());
    QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), channels.at(1).id);

    QVERIFY(harness.multiViewController->stopRetainedPromotedAndRestoreGrid());
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("grid2x2"));
    QVERIFY(!harness.multiViewController->retainedSelectionActive());
    QCOMPARE(harness.multiViewController->focusedTileIndex(), 0);

    const auto tilesAfterReopen = harness.multiViewController->tiles();
    QCOMPARE(tilesAfterReopen.at(0).toMap().value(QStringLiteral("isEmpty")).toBool(), false);
    QCOMPARE(tilesAfterReopen.at(0).toMap().value(QStringLiteral("channelId")).toInt(), channels.at(0).id);
    QVERIFY(tilesAfterReopen.at(0).toMap().value(QStringLiteral("isFocused")).toBool());
    QCOMPARE(tilesAfterReopen.at(1).toMap().value(QStringLiteral("isEmpty")).toBool(), true);
    QCOMPARE(tilesAfterReopen.at(1).toMap().value(QStringLiteral("channelId")).toInt(), -1);
    QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), channels.at(0).id);
}

void AppModelTests::multiviewRetainedSelectionKeepsHiddenTilesAudioWarmAndSchedulesDeferredRefresh()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    harness.settingsController->setMultiviewRetainSelectionOnPromotion(true);
    harness.settingsController->setMultiviewMaxTiles(4);
    harness.settingsController->save();

    QVERIFY(harness.channelListModel->activateById(channels.at(0).id));
    QVERIFY(harness.multiViewController->toggleGrid());
    harness.multiViewController->focusTile(1);
    QVERIFY(harness.channelListModel->activateById(channels.at(1).id));

    harness.multiViewController->focusTile(1);
    QVERIFY(harness.multiViewController->toggleGrid());
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("off"));
    QVERIFY(harness.multiViewController->retainedSelectionActive());

    bool foundWarmHiddenAudio = false;
    for (const auto &slot : harness.multiViewController->m_secondarySlots) {
        auto *slotPlayer = slot.playbackPlayer();
        if (!slot.channel.has_value() || slotPlayer == nullptr) {
            continue;
        }
        QCOMPARE(slotPlayer->requestedVolume(), 0);
        QVERIFY(slotPlayer->audioEnabledRequested());
        foundWarmHiddenAudio = true;
    }
    QVERIFY(foundWarmHiddenAudio);

    QTRY_VERIFY_WITH_TIMEOUT(!harness.multiViewController->m_audioOwnershipRefreshTimer.isActive(), 1000);
    QSignalSpy audioRefreshSpy(
        &harness.multiViewController->m_audioOwnershipRefreshTimer,
        &QTimer::timeout);

    QVERIFY(harness.multiViewController->toggleGrid());
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("grid2x2"));
    QVERIFY(!harness.multiViewController->retainedSelectionActive());
    QVERIFY(
        harness.multiViewController->m_audioOwnershipRefreshTimer.isActive()
        || audioRefreshSpy.count() > 0);
    QTRY_VERIFY_WITH_TIMEOUT(audioRefreshSpy.count() > 0, 1000);
}

void AppModelTests::multiviewRetainedSelectionClearsOnFullPromoteShortcut()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    harness.settingsController->setMultiviewRetainSelectionOnPromotion(true);
    harness.settingsController->setMultiviewMaxTiles(4);
    harness.settingsController->save();

    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    QVERIFY(harness.multiViewController->toggleGrid());
    harness.multiViewController->focusTile(1);
    QVERIFY(harness.channelListModel->activateById(channels.last().id));

    harness.multiViewController->focusTile(1);
    QVERIFY(harness.multiViewController->fullPromoteAndExit());
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("off"));
    QVERIFY(!harness.multiViewController->retainedSelectionActive());
    QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), channels.last().id);
    QCOMPARE(static_cast<int>(harness.multiViewController->m_secondarySlots.size()), 0);

    // Selection takes precedence over any last-focused secondary tracking:
    // with primary selected, explicit full promotion must keep primary.
    QVERIFY(harness.multiViewController->toggleGrid());
    harness.multiViewController->focusTile(1);
    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    harness.multiViewController->focusTile(0);
    QVERIFY(harness.multiViewController->fullPromoteAndExit());
    QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), channels.last().id);

    QVERIFY(harness.multiViewController->toggleGrid());
    harness.multiViewController->focusTile(1);
    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    harness.multiViewController->focusTile(1);
    QVERIFY(harness.multiViewController->toggleGrid());
    QVERIFY(harness.multiViewController->retainedSelectionActive());

    QVERIFY(harness.multiViewController->fullPromoteAndExit());
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("off"));
    QVERIFY(!harness.multiViewController->retainedSelectionActive());
    QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), channels.first().id);
    QCOMPARE(static_cast<int>(harness.multiViewController->m_secondarySlots.size()), 0);
}

void AppModelTests::multiviewRetainedSelectionClearsOnDegradeToOff()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    harness.settingsController->setMultiviewRetainSelectionOnPromotion(true);
    harness.settingsController->setMultiviewMaxTiles(4);
    harness.settingsController->save();

    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    QVERIFY(harness.multiViewController->toggleGrid());
    harness.multiViewController->focusTile(1);
    QVERIFY(harness.channelListModel->activateById(channels.last().id));
    harness.multiViewController->focusTile(1);

    harness.multiViewController->m_degradePromptVisible = true;
    harness.multiViewController->m_pendingDegradeLayout = QStringLiteral("off");
    harness.multiViewController->acceptPendingDegrade();

    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("off"));
    QVERIFY(!harness.multiViewController->retainedSelectionActive());
    QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), channels.first().id);
    QCOMPARE(static_cast<int>(harness.multiViewController->m_secondarySlots.size()), 0);
}

void AppModelTests::multiviewRetainedSelectionClearsOnProfileChange()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    harness.settingsController->setMultiviewRetainSelectionOnPromotion(true);
    harness.settingsController->setMultiviewMaxTiles(4);
    harness.settingsController->save();

    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    QVERIFY(harness.multiViewController->toggleGrid());
    harness.multiViewController->focusTile(1);
    QVERIFY(harness.channelListModel->activateById(channels.last().id));
    harness.multiViewController->focusTile(1);
    QVERIFY(harness.multiViewController->toggleGrid());
    QVERIFY(harness.multiViewController->retainedSelectionActive());

    auto profileSwitchedChannel = channels.first();
    profileSwitchedChannel.id = 999;
    profileSwitchedChannel.name = QStringLiteral("Profile Switched Channel");
    profileSwitchedChannel.profileId = QUuid::createUuid();
    harness.playerController->playChannel(profileSwitchedChannel);

    QTRY_VERIFY_WITH_TIMEOUT(!harness.multiViewController->retainedSelectionActive(), 3000);
    QCOMPARE(static_cast<int>(harness.multiViewController->m_secondarySlots.size()), 0);
}

void AppModelTests::multiviewExitAfterSwapKeepsPromotedPrimaryWithoutRetune()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    QVERIFY(harness.multiViewController->togglePictureInPicture(channels.last().id));
    QVERIFY(harness.multiViewController->swapPrimaryWithPictureInPicture());
    QVERIFY(harness.playerController->usingSharedPlayback());
    auto *promotedPrimaryPlayer = harness.playerController->player();
    QVERIFY(promotedPrimaryPlayer != nullptr);

    QSignalSpy activationSpy(harness.playerController.get(), &PlayerController::playbackChannelActivated);
    QSignalSpy currentChannelSpy(harness.playerController.get(), &PlayerController::currentChannelChanged);

    harness.multiViewController->exitMultiView();

    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("off"));
    QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), channels.last().id);
    QVERIFY(harness.playerController->usingSharedPlayback());
    QCOMPARE(harness.playerController->player(), promotedPrimaryPlayer);
    QCOMPARE(activationSpy.count(), 0);
    QCOMPARE(currentChannelSpy.count(), 0);
}

void AppModelTests::multiviewExitAfterSwapDefersDetachedPlayerCleanup()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    QVERIFY(harness.multiViewController->togglePictureInPicture(channels.last().id));
    QVERIFY(harness.multiViewController->swapPrimaryWithPictureInPicture());
    QVERIFY(harness.playerController->usingSharedPlayback());

    const auto swappedTiles = harness.multiViewController->tiles();
    auto *swappedPrimaryPlayer =
        swappedTiles.at(0).toMap().value(QStringLiteral("playerObject")).value<QObject *>();
    auto *swappedSecondaryPlayer =
        swappedTiles.at(1).toMap().value(QStringLiteral("playerObject")).value<QObject *>();
    QVERIFY(swappedPrimaryPlayer != nullptr);
    QVERIFY(swappedSecondaryPlayer != nullptr);
    QVERIFY(swappedPrimaryPlayer != swappedSecondaryPlayer);

    harness.multiViewController->exitMultiView();

    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("off"));
    QCOMPARE(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), channels.last().id);
    QVERIFY(harness.playerController->usingSharedPlayback());
    QCOMPARE(harness.playerController->player(), swappedPrimaryPlayer);
    QTRY_VERIFY_WITH_TIMEOUT(harness.multiViewController->m_retiredPlayers.empty(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!harness.multiViewController->m_retiredPlayerCleanupTimer.isActive(), 1000);
}

void AppModelTests::multiviewRetiredSecondarySignalsDoNotCorruptReusedSlot()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    QVERIFY(harness.multiViewController->togglePictureInPicture(channels.last().id));

    const auto firstOpenTiles = harness.multiViewController->tiles();
    auto *initialSecondaryPlayer =
        firstOpenTiles.at(1).toMap().value(QStringLiteral("playerObject")).value<QObject *>();
    QVERIFY(initialSecondaryPlayer != nullptr);

    QVERIFY(harness.multiViewController->togglePictureInPicture(-1));
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("off"));
    QVERIFY(!harness.multiViewController->m_retiredPlayers.empty());

    auto *retiredSecondaryPlayer = harness.multiViewController->m_retiredPlayers.back().get();
    QVERIFY(retiredSecondaryPlayer != nullptr);
    QCOMPARE(static_cast<QObject *>(retiredSecondaryPlayer), initialSecondaryPlayer);

    QVERIFY(harness.multiViewController->togglePictureInPicture(channels.last().id));
    QCOMPARE(harness.multiViewController->layoutMode(), QStringLiteral("pip"));

    const auto reopenedTiles = harness.multiViewController->tiles();
    auto *reopenedSecondaryPlayer =
        reopenedTiles.at(1).toMap().value(QStringLiteral("playerObject")).value<QObject *>();
    QVERIFY(reopenedSecondaryPlayer != nullptr);
    QVERIFY(reopenedSecondaryPlayer != initialSecondaryPlayer);
    QCOMPARE(harness.multiViewController->m_secondarySlots.at(0).playbackPlayer(), reopenedSecondaryPlayer);

    auto &slot = harness.multiViewController->m_secondarySlots.at(0);
    slot.playerState = QStringLiteral("loading");
    slot.hasError = false;
    slot.errorText.clear();

    QVERIFY(QMetaObject::invokeMethod(
        retiredSecondaryPlayer,
        "errorOccurred",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("stale retired error"))));
    QVERIFY(QMetaObject::invokeMethod(retiredSecondaryPlayer, "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        retiredSecondaryPlayer,
        "bufferingStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, true)));

    QCOMPARE(slot.playerState, QStringLiteral("loading"));
    QCOMPARE(slot.hasError, false);
    QVERIFY(slot.errorText.isEmpty());
}

void AppModelTests::multiviewPrimaryAssignmentRetunesSharedPrimaryInPlaceAfterSwap()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    ChannelListModel channelListModel(&settings);
    PlayerController playerController;
    MultiViewController multiViewController(&settings, &channelListModel, &playerController);

    const auto profileId = QUuid::createUuid();
    QList<Channel> channels;
    for (int index = 0; index < 3; ++index) {
        Channel channel;
        channel.id = index + 1;
        channel.name = QStringLiteral("Channel %1").arg(index + 1);
        channel.streamUrl = QStringLiteral("http://127.0.0.1/channel-%1").arg(index + 1);
        channel.categoryId = QStringLiteral("group-%1").arg(index + 1);
        channel.categoryName = QStringLiteral("Group %1").arg(index + 1);
        channel.profileId = profileId;
        channels.push_back(channel);
    }

    QList<ChannelCategory> categories;
    for (int index = 0; index < channels.size(); ++index) {
        ChannelCategory category;
        category.id = channels.at(index).categoryId;
        category.name = channels.at(index).categoryName;
        categories.push_back(category);
    }
    channelListModel.setChannels(channels, categories);

    QObject::connect(
        &multiViewController,
        &MultiViewController::primaryTileAssignmentRequested,
        &playerController,
        [&channelListModel, &playerController](const int channelId) {
            const auto channel = channelListModel.channelById(channelId);
            if (channel.has_value()) {
                playerController.playChannel(channel.value());
            }
        });

    playerController.playChannel(channels.at(0));
    QVERIFY(multiViewController.togglePictureInPicture(channels.at(1).id));
    QVERIFY(multiViewController.swapPrimaryWithPictureInPicture());

    const auto swappedTiles = multiViewController.tiles();
    auto *expectedPrimaryPlayer =
        swappedTiles.at(0).toMap().value(QStringLiteral("playerObject")).value<QObject *>();
    auto *expectedSecondaryPlayer =
        swappedTiles.at(1).toMap().value(QStringLiteral("playerObject")).value<QObject *>();
    QVERIFY(expectedPrimaryPlayer != nullptr);
    QVERIFY(expectedSecondaryPlayer != nullptr);
    QVERIFY(expectedPrimaryPlayer != expectedSecondaryPlayer);

    QVERIFY(multiViewController.assignResolvedChannel(channels.at(2)));
    QCOMPARE(playerController.currentChannel().value(QStringLiteral("id")).toInt(), channels.at(2).id);

    const auto retunedTiles = multiViewController.tiles();
    QCOMPARE(retunedTiles.at(0).toMap().value(QStringLiteral("channelId")).toInt(), channels.at(2).id);
    QCOMPARE(retunedTiles.at(1).toMap().value(QStringLiteral("channelId")).toInt(), channels.at(0).id);
    QCOMPARE(retunedTiles.at(0).toMap().value(QStringLiteral("playerObject")).value<QObject *>(), expectedPrimaryPlayer);
    QCOMPARE(retunedTiles.at(1).toMap().value(QStringLiteral("playerObject")).value<QObject *>(), expectedSecondaryPlayer);
}

void AppModelTests::dvrControllerToggleScheduleAndExitGuard()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    settings.current().dvrStartOffsetMinutes = 0;
    settings.current().dvrEndOffsetMinutes = 0;
    settings.save();

    PlayerController playerController;
    DvrController dvrController(&settings, &playerController);

    const auto now = QDateTime::currentDateTimeUtc();
    const auto start = now.addSecs(5 * 60);
    const auto stop = start.addSecs(15 * 60);

    const QVariantMap channel {
        { QStringLiteral("id"), 101 },
        { QStringLiteral("name"), QStringLiteral("DVR Channel") },
        { QStringLiteral("streamUrl"), QStringLiteral("http://127.0.0.1/dvr") },
        { QStringLiteral("profileId"), QStringLiteral("profile-one") },
        { QStringLiteral("tvgId"), QStringLiteral("channel.dvr") }
    };
    const QVariantMap program {
        { QStringLiteral("title"), QStringLiteral("Scheduled Show") },
        { QStringLiteral("start"), start.toString(Qt::ISODateWithMs) },
        { QStringLiteral("stop"), stop.toString(Qt::ISODateWithMs) }
    };

    QCOMPARE(dvrController.scheduledCount(), 0);
    QVERIFY(!dvrController.exitConfirmationRequired());

    QVERIFY(dvrController.toggleProgramSchedule(channel, program));
    QCOMPARE(dvrController.scheduledCount(), 1);
    QVERIFY(dvrController.isProgramScheduled(channel, program));
    QVERIFY(dvrController.exitConfirmationRequired());

    QVERIFY(dvrController.toggleProgramSchedule(channel, program));
    QCOMPARE(dvrController.scheduledCount(), 0);
    QVERIFY(!dvrController.isProgramScheduled(channel, program));
    QVERIFY(!dvrController.exitConfirmationRequired());
}

void AppModelTests::dvrControllerFinalizeActiveWindowSchedulesRestart()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    settings.current().dvrStartOffsetMinutes = 0;
    settings.current().dvrEndOffsetMinutes = 0;

    DvrScheduleEntry entry;
    entry.profileId = QStringLiteral("profile-one");
    entry.channelId = 601;
    entry.channelName = QStringLiteral("DVR Active Window");
    entry.streamUrl = QStringLiteral("http://127.0.0.1/dvr-active");
    entry.title = QStringLiteral("Active Programme");
    entry.start = QDateTime::currentDateTimeUtc().addSecs(-60);
    entry.stop = QDateTime::currentDateTimeUtc().addSecs(15 * 60);
    entry.id = DvrController::makeScheduleId(entry.profileId, entry.channelId, entry.start, entry.stop, entry.title);
    settings.current().dvrSchedules = { entry };
    settings.save();

    PlayerController playerController;
    DvrController dvrController(&settings, &playerController);
    const auto windows = dvrController.mergedWindows();
    QVERIFY(!windows.isEmpty());
    const auto window = windows.first();
    QVERIFY(window.startAt <= QDateTime::currentDateTimeUtc());
    QVERIFY(QDateTime::currentDateTimeUtc() < window.stopAt);

    auto session = std::make_unique<DvrController::Session>();
    session->window = window;
    session->state = DvrController::SessionState::Stopping;
    session->stopRequested = true;
    session->stopReason = QStringLiteral("process-error");
    session->recordingStarted = true;
    session->tapUrl = QStringLiteral("udp://127.0.0.1:50001");
    session->finishedSignaled = true;
    session->lastExitCode = 1;
    session->lastExitStatus = QProcess::CrashExit;
    session->ingestProcess = std::make_unique<QProcess>();
    dvrController.m_sessions.insert_or_assign(window.id, std::move(session));

    dvrController.finalizeStopSession(window.id, 1, QProcess::CrashExit);
    QVERIFY(dvrController.m_sessions.find(window.id) == dvrController.m_sessions.end());
    QVERIFY(dvrController.m_restartNotBeforeByWindowId.contains(window.id));
    QVERIFY(dvrController.m_restartNotBeforeByWindowId.value(window.id) > QDateTime::currentDateTimeUtc());
}

void AppModelTests::dvrControllerRestartStateMaintainedAndClearedByWindowState()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    settings.current().dvrStartOffsetMinutes = 0;
    settings.current().dvrEndOffsetMinutes = 0;

    DvrScheduleEntry entry;
    entry.profileId = QStringLiteral("profile-one");
    entry.channelId = 602;
    entry.channelName = QStringLiteral("DVR Restart Window");
    entry.streamUrl = QStringLiteral("http://127.0.0.1/dvr-restart");
    entry.title = QStringLiteral("Restart Programme");
    entry.start = QDateTime::currentDateTimeUtc().addSecs(-60);
    entry.stop = QDateTime::currentDateTimeUtc().addSecs(15 * 60);
    entry.id = DvrController::makeScheduleId(entry.profileId, entry.channelId, entry.start, entry.stop, entry.title);
    settings.current().dvrSchedules = { entry };
    settings.save();

    PlayerController playerController;
    DvrController dvrController(&settings, &playerController);
    const auto windows = dvrController.mergedWindows();
    QVERIFY(!windows.isEmpty());
    const auto windowId = windows.first().id;

    dvrController.scheduleRestartForWindow(windowId, QStringLiteral("unit-test-initial"));
    const auto firstRetryAt = dvrController.m_restartNotBeforeByWindowId.value(windowId);
    QTest::qWait(5);
    dvrController.scheduleRestartForWindow(windowId, QStringLiteral("unit-test-retry"));
    const auto secondRetryAt = dvrController.m_restartNotBeforeByWindowId.value(windowId);
    QVERIFY(secondRetryAt >= firstRetryAt);

    dvrController.tick();
    QVERIFY(dvrController.m_restartNotBeforeByWindowId.contains(windowId));

    dvrController.m_schedules.clear();
    dvrController.tick();
    QVERIFY(!dvrController.m_restartNotBeforeByWindowId.contains(windowId));
}

void AppModelTests::dvrControllerWindowsCrashWithoutFinishedDefersFinalize()
{
#if !defined(Q_OS_WIN)
    QSKIP("Windows-specific DVR orphan-process reconciliation path.");
#else
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    PlayerController playerController;
    DvrController dvrController(&settings, &playerController);

    auto session = std::make_unique<DvrController::Session>();
    session->window.id = QStringLiteral("win-crash-no-finished");
    session->window.profileId = QStringLiteral("profile-one");
    session->window.channelId = 501;
    session->state = DvrController::SessionState::Stopping;
    session->stopRequested = true;
    session->recordingStarted = false;
    session->finishedSignaled = false;
    session->orphanSweepRetriesRemaining = 1;
    session->ingestProcess = std::make_unique<QProcess>();

    const auto sessionId = session->window.id;
    dvrController.m_sessions.insert_or_assign(sessionId, std::move(session));

    QSignalSpy stateSpy(&dvrController, &DvrController::stateChanged);

    dvrController.reconcileStoppingSession(sessionId, QStringLiteral("unit-test"));
    QVERIFY2(
        dvrController.m_sessions.find(sessionId) != dvrController.m_sessions.end(),
        "session finalized too early; expected deferred finalize until orphan sweep pass");

    QTRY_VERIFY_WITH_TIMEOUT(dvrController.m_sessions.find(sessionId) == dvrController.m_sessions.end(), 3000);
    QVERIFY(stateSpy.count() > 0);
#endif
}

void AppModelTests::dvrControllerRemuxDeletesTempWhenDurationMatchesRegardlessOfExitCode()
{
#if defined(Q_OS_WIN)
    QSKIP("POSIX shell-based fake ffmpeg/ffprobe wrappers are used by this test.");
#else
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const auto toolsDir = tempDir.filePath(QStringLiteral("tools"));
    QVERIFY(QDir().mkpath(toolsDir));

    const auto ffmpegPath = QDir(toolsDir).filePath(QStringLiteral("ffmpeg"));
    const auto ffprobePath = QDir(toolsDir).filePath(QStringLiteral("ffprobe"));

    const QString ffmpegScript = QStringLiteral(
        "#!/usr/bin/env bash\n"
        "input=\"\"\n"
        "for ((i=1; i<= $#; ++i)); do\n"
        "  arg=\"${!i}\"\n"
        "  if [[ \"$arg\" == \"-i\" ]]; then\n"
        "    j=$((i+1))\n"
        "    input=\"${!j}\"\n"
        "  fi\n"
        "done\n"
        "output=\"${@: -1}\"\n"
        "cp \"$input\" \"$output\"\n"
        "exit 23\n");
    const QString ffprobeScript = QStringLiteral(
        "#!/usr/bin/env bash\n"
        "target=\"${@: -1}\"\n"
        "if [[ \"$target\" == *.ts ]]; then\n"
        "  echo \"150.000\"\n"
        "else\n"
        "  echo \"150.000\"\n"
        "fi\n"
        "exit 0\n");
    QVERIFY(writeExecutableTextFile(ffmpegPath, ffmpegScript));
    QVERIFY(writeExecutableTextFile(ffprobePath, ffprobeScript));
    ScopedPathOverride scopedPath(toolsDir);

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    PlayerController playerController;
    DvrController dvrController(&settings, &playerController);

    const auto tempPath = tempDir.filePath(QStringLiteral("record.ts"));
    const auto finalPath = tempDir.filePath(QStringLiteral("record.mkv"));
    QFile tempFile(tempPath);
    QVERIFY(tempFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    tempFile.write("sample-dvr-ts-payload");
    tempFile.close();

    DvrController::Session session;
    session.remuxToMkv = true;
    session.recordTempPath = tempPath;
    session.recordFinalPath = finalPath;

    dvrController.maybeStartRemux(session);

    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(finalPath), 5000);
    QVERIFY(QFileInfo(finalPath).size() > 0);
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(tempPath), 5000);
#endif
}

void AppModelTests::dvrControllerRemuxKeepsTempWhenDurationMismatched()
{
#if defined(Q_OS_WIN)
    QSKIP("POSIX shell-based fake ffmpeg/ffprobe wrappers are used by this test.");
#else
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const auto toolsDir = tempDir.filePath(QStringLiteral("tools"));
    QVERIFY(QDir().mkpath(toolsDir));

    const auto ffmpegPath = QDir(toolsDir).filePath(QStringLiteral("ffmpeg"));
    const auto ffprobePath = QDir(toolsDir).filePath(QStringLiteral("ffprobe"));

    const QString ffmpegScript = QStringLiteral(
        "#!/usr/bin/env bash\n"
        "input=\"\"\n"
        "for ((i=1; i<= $#; ++i)); do\n"
        "  arg=\"${!i}\"\n"
        "  if [[ \"$arg\" == \"-i\" ]]; then\n"
        "    j=$((i+1))\n"
        "    input=\"${!j}\"\n"
        "  fi\n"
        "done\n"
        "output=\"${@: -1}\"\n"
        "cp \"$input\" \"$output\"\n"
        "exit 0\n");
    const QString ffprobeScript = QStringLiteral(
        "#!/usr/bin/env bash\n"
        "target=\"${@: -1}\"\n"
        "if [[ \"$target\" == *.ts ]]; then\n"
        "  echo \"150.000\"\n"
        "else\n"
        "  echo \"90.000\"\n"
        "fi\n"
        "exit 0\n");
    QVERIFY(writeExecutableTextFile(ffmpegPath, ffmpegScript));
    QVERIFY(writeExecutableTextFile(ffprobePath, ffprobeScript));
    ScopedPathOverride scopedPath(toolsDir);

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    PlayerController playerController;
    DvrController dvrController(&settings, &playerController);

    const auto tempPath = tempDir.filePath(QStringLiteral("record.ts"));
    const auto finalPath = tempDir.filePath(QStringLiteral("record.mkv"));
    QFile tempFile(tempPath);
    QVERIFY(tempFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    tempFile.write("sample-dvr-ts-payload");
    tempFile.close();

    DvrController::Session session;
    session.remuxToMkv = true;
    session.recordTempPath = tempPath;
    session.recordFinalPath = finalPath;

    dvrController.maybeStartRemux(session);

    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(finalPath), 5000);
    QVERIFY(QFileInfo::exists(tempPath));
#endif
}

void AppModelTests::portableRuntimeControllerTracksPortableOverrideWithoutDirtyingSettings()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    ScopedRuntimeContext runtimeContext;
    qputenv("OKILTV_SKIP_PORTABLE_RESTART", QByteArrayLiteral("1"));

    RuntimeContext context;
    context.launchMode = LaunchMode::Portable;
    context.portableBootstrapPath = harness.tempDir.filePath(QStringLiteral("OKILTV-portable.json"));
    AppDataPaths::initializeRuntime(context);

    PortableRuntimeController controller;
    QVERIFY(controller.portableModeEnabled());
    QCOMPARE(controller.customDataRoot(), QString());
    QVERIFY(!controller.restartRequired());

    controller.setCustomDataRoot(QStringLiteral("relative/path"));
    QVERIFY(controller.dataRootStatus().contains(QStringLiteral("absolute path")));

    const auto customRoot = harness.tempDir.filePath(QStringLiteral("portable-data"));
    controller.setCustomDataRoot(customRoot);
    controller.applyCustomDataRootAndRestart();

    QVERIFY(controller.restartRequired());
    QCOMPARE(PortableBootstrap::load(context.portableBootstrapPath).dataRootOverride, QDir::cleanPath(customRoot));
    QVERIFY(!harness.settingsController->dirty());

    qunsetenv("OKILTV_SKIP_PORTABLE_RESTART");
}

void AppModelTests::channelListModelSupportsAutoFavouritesAndGroupPrefs()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    ChannelListModel model(&settings);
    model.setActiveProfileId(QStringLiteral("profile-a"));

    Channel news;
    news.id = 1;
    news.name = QStringLiteral("BBC One");
    news.categoryId = QStringLiteral("News");
    news.sortOrder = 20;

    Channel sports;
    sports.id = 2;
    sports.name = QStringLiteral("Sky Sports");
    sports.categoryId = QStringLiteral("Sports");
    sports.sortOrder = 5;

    model.setChannels(
        { news, sports },
        {
            ChannelCategory { QStringLiteral("News"), QStringLiteral("News"), 0 },
            ChannelCategory { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
        });

    QCOMPARE(model.totalCount(), 2);
    QCOMPARE(model.filteredCount(), 2);
    QCOMPARE(model.index(0, 0).data(ChannelListModel::IdRole).toInt(), 2);

    model.setWatchSeconds({
        { 1, 7 * 60 * 60 },
        { 2, 12 * 60 }
    });
    QSignalSpy modelResetSpy(&model, &QAbstractItemModel::modelReset);
    QVERIFY(model.isFavorite(1));
    QVERIFY(!model.isFavorite(2));
    QVERIFY(model.toggleFavorite(2));
    QCOMPARE(modelResetSpy.count(), 0);
    QVERIFY(model.isFavorite(2));
    QCOMPARE(
        settings.current().favoriteChannelIdsByProfile.value(QStringLiteral("profile-a")),
        QList<int> { 2 });

    model.setSelectedCategoryId(QStringLiteral("__favourites__"));
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.filteredCount(), 2);
    QCOMPARE(model.index(0, 0).data(ChannelListModel::IdRole).toInt(), 1);
    QCOMPARE(model.index(1, 0).data(ChannelListModel::IdRole).toInt(), 2);

    QVERIFY(model.toggleFavorite(2));
    QVERIFY(!model.isFavorite(2));
    QCOMPARE(model.rowCount(), 1);

    QVERIFY(model.setCategoryHidden(QStringLiteral("Sports"), true));
    const auto hiddenCategories = model.categories();
    QCOMPARE(hiddenCategories.size(), 2);
    QCOMPARE(hiddenCategories.at(0).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("__favourites__"));
    QCOMPARE(hiddenCategories.at(1).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("News"));

    QVERIFY(model.setCategoryHidden(QStringLiteral("Sports"), false));
    QVERIFY(model.moveCategory(QStringLiteral("Sports"), 0));
    const auto orderedCategories = model.categories();
    QCOMPARE(orderedCategories.at(0).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("Sports"));
}

void AppModelTests::channelListModelExposesCurrentProgramRoles()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    ChannelListModel model(&settings);
    model.setActiveProfileId(QStringLiteral("profile-a"));

    Channel news;
    news.id = 1;
    news.name = QStringLiteral("BBC One");
    news.categoryId = QStringLiteral("News");

    model.setChannels(
        { news },
        { ChannelCategory { QStringLiteral("News"), QStringLiteral("News"), 0 } });

    model.setCurrentProgramInfo({
        { 1,
          QVariantMap {
              { QStringLiteral("title"), QStringLiteral("Morning News") },
              { QStringLiteral("timeRange"), QStringLiteral("08:00 - 09:00") }
          } }
    });

    const auto row = model.index(0, 0);
    QCOMPARE(row.data(ChannelListModel::CurrentProgramTitleRole).toString(), QStringLiteral("Morning News"));
    QCOMPARE(row.data(ChannelListModel::CurrentProgramTimeRangeRole).toString(), QStringLiteral("08:00 - 09:00"));
}

void AppModelTests::channelListModelExposesDvrRecordingRole()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    ChannelListModel model(&settings);
    model.setActiveProfileId(QStringLiteral("profile-a"));

    Channel news;
    news.id = 11;
    news.name = QStringLiteral("News");
    news.categoryId = QStringLiteral("News");

    Channel sports;
    sports.id = 12;
    sports.name = QStringLiteral("Sports");
    sports.categoryId = QStringLiteral("Sports");

    model.setChannels(
        { news, sports },
        {
            ChannelCategory { QStringLiteral("News"), QStringLiteral("News"), 0 },
            ChannelCategory { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
        });

    QCOMPARE(model.index(0, 0).data(ChannelListModel::IsDvrRecordingRole).toBool(), false);
    QCOMPARE(model.index(1, 0).data(ChannelListModel::IsDvrRecordingRole).toBool(), false);

    model.setDvrRecordingChannelsForProfile(QStringLiteral("profile-a"), { 12 });
    QCOMPARE(model.index(0, 0).data(ChannelListModel::IsDvrRecordingRole).toBool(), false);
    QCOMPARE(model.index(1, 0).data(ChannelListModel::IsDvrRecordingRole).toBool(), true);

    model.setDvrRecordingChannelsForProfile(QStringLiteral("profile-a"), {});
    QCOMPARE(model.index(1, 0).data(ChannelListModel::IsDvrRecordingRole).toBool(), false);
}

void AppModelTests::channelListModelHidesDeselectedGroupsUntilExplicitGroupIsChosen()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    ChannelListModel model(&settings);
    model.setActiveProfileId(QStringLiteral("profile-a"));

    Channel news;
    news.id = 1;
    news.name = QStringLiteral("BBC One");
    news.categoryId = QStringLiteral("News");

    Channel sports;
    sports.id = 2;
    sports.name = QStringLiteral("Sky Sports");
    sports.categoryId = QStringLiteral("Sports");

    settings.current().hiddenGroupsByProfile[QStringLiteral("profile-a")] = { QStringLiteral("Sports") };

    model.setChannels(
        { news, sports },
        {
            ChannelCategory { QStringLiteral("News"), QStringLiteral("News"), 0 },
            ChannelCategory { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
        });

    QCOMPARE(model.filteredCount(), 1);
    QCOMPARE(model.index(0, 0).data(ChannelListModel::IdRole).toInt(), 1);

    model.setSelectedCategoryId(QStringLiteral("Sports"));
    QCOMPARE(model.filteredCount(), 1);
    QCOMPARE(model.index(0, 0).data(ChannelListModel::IdRole).toInt(), 2);

    model.setSelectedCategoryId(QString {});
    QCOMPARE(model.filteredCount(), 1);

    settings.current().hiddenGroupsByProfile[QStringLiteral("profile-a")].clear();
    model.refreshFilter();
    QCOMPARE(model.filteredCount(), 2);
}

void AppModelTests::sourceGroupsModelAppliesSelectionThresholdAndPersistsReorder()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    DatabaseService database(tempDir.filePath(QStringLiteral("channels.db")));
    database.ensureSchema();

    ServerProfile profileA;
    profileA.name = QStringLiteral("Small Group Set");
    ServerProfile profileB;
    profileB.name = QStringLiteral("Large Group Set");
    settings.current().profiles = { profileA, profileB };
    settings.save();

    Channel news;
    news.id = 1;
    news.profileId = profileA.id;
    news.name = QStringLiteral("News One");
    news.categoryId = QStringLiteral("News");
    news.streamUrl = QStringLiteral("http://127.0.0.1/news");
    news.tvgId = QStringLiteral("news.one");
    news.tvgName = news.name;

    Channel sports;
    sports.id = 2;
    sports.profileId = profileA.id;
    sports.name = QStringLiteral("Sports One");
    sports.categoryId = QStringLiteral("Sports");
    sports.streamUrl = QStringLiteral("http://127.0.0.1/sports");
    sports.tvgId = QStringLiteral("sports.one");
    sports.tvgName = sports.name;

    Channel ungrouped;
    ungrouped.id = 3;
    ungrouped.profileId = profileA.id;
    ungrouped.name = QStringLiteral("Ungrouped One");
    ungrouped.categoryId = QString {};
    ungrouped.streamUrl = QStringLiteral("http://127.0.0.1/ungrouped");
    ungrouped.tvgId = QStringLiteral("ungrouped.one");
    ungrouped.tvgName = ungrouped.name;

    QList<Channel> channels { news, sports, ungrouped };
    for (int index = 0; index < 21; ++index) {
        Channel groupChannel;
        groupChannel.id = 100 + index;
        groupChannel.profileId = profileB.id;
        groupChannel.name = QStringLiteral("Group Channel %1").arg(index + 1);
        groupChannel.categoryId = QStringLiteral("Group %1").arg(index + 1);
        groupChannel.streamUrl = QStringLiteral("http://127.0.0.1/group-%1").arg(index + 1);
        groupChannel.tvgId = QStringLiteral("group.%1").arg(index + 1);
        groupChannel.tvgName = groupChannel.name;
        channels.push_back(groupChannel);
    }

    const auto profileAKey = guidToString(profileA.id);
    const auto profileBKey = guidToString(profileB.id);
    database.upsertChannels(channels);
    database.incrementWatchSeconds(profileA.id, news.id, (5 * 60 * 60) + (59 * 60));
    settings.current().favoriteChannelIdsByProfile[profileAKey] = { sports.id };
    settings.save();

    SourceGroupsModel model(&settings, &database);
    const auto favouritesCount = [&model]() {
        for (auto row = 0; row < model.rowCount(); ++row) {
            if (model.get(row).value(QStringLiteral("id")).toString() == QStringLiteral("__favourites__")) {
                return model.get(row).value(QStringLiteral("count")).toInt();
            }
        }
        return -1;
    };

    model.setProfileId(profileAKey);

    QTRY_COMPARE_WITH_TIMEOUT(model.totalCount(), 4, 3000);
    QCOMPARE(model.get(0).value(QStringLiteral("id")).toString(), QStringLiteral("__favourites__"));
    QCOMPARE(model.get(0).value(QStringLiteral("count")).toInt(), 1);
    QCOMPARE(favouritesCount(), 1);
    QCOMPARE(model.selectedCount(), 4);
    QCOMPARE(model.get(0).value(QStringLiteral("selected")).toBool(), true);
    QCOMPARE(settings.current().hiddenGroupsByProfile.value(profileAKey).size(), 0);
    QCOMPARE(model.hideUnchecked(), false);

    model.setHideUnchecked(true);
    QCOMPARE(model.hideUnchecked(), true);
    QCOMPARE(settings.current().hideUncheckedGroupsByProfile.value(profileAKey, false), true);

    QVERIFY(model.moveGroup(QStringLiteral("Sports"), 0));
    QCOMPARE(model.get(0).value(QStringLiteral("id")).toString(), QStringLiteral("Sports"));

    model.reload();
    QTRY_COMPARE_WITH_TIMEOUT(model.loading(), false, 3000);
    QCOMPARE(model.get(0).value(QStringLiteral("id")).toString(), QStringLiteral("Sports"));
    QCOMPARE(favouritesCount(), 1);
    QCOMPARE(
        settings.current().groupOrderByProfile.value(profileAKey).value(0),
        QStringLiteral("Sports"));
    QCOMPARE(model.selectedCount(), 4);
    QCOMPARE(model.hideUnchecked(), true);

    auto foundUngrouped = false;
    for (int row = 0; row < model.rowCount(); ++row) {
        if (model.get(row).value(QStringLiteral("id")).toString() == ungroupedCategoryId()) {
            QCOMPARE(model.get(row).value(QStringLiteral("name")).toString(), QStringLiteral("Ungrouped"));
            foundUngrouped = true;
            break;
        }
    }
    QVERIFY(foundUngrouped);

    QVERIFY(model.setGroupsSelected({ QStringLiteral("Sports") }, false));
    QCOMPARE(model.selectedCount(), 3);

    database.incrementWatchSeconds(profileA.id, news.id, 60);
    model.reload();
    QTRY_COMPARE_WITH_TIMEOUT(model.loading(), false, 3000);
    QCOMPARE(favouritesCount(), 2);

    model.setProfileId(profileBKey);
    QTRY_COMPARE_WITH_TIMEOUT(model.totalCount(), 22, 3000);
    QCOMPARE(model.get(0).value(QStringLiteral("id")).toString(), QStringLiteral("__favourites__"));
    QCOMPARE(model.selectedCount(), 1);
    QCOMPARE(settings.current().hiddenGroupsByProfile.value(profileBKey).size(), 21);
    QCOMPARE(model.hideUnchecked(), false);
    QCOMPARE(settings.current().hideUncheckedGroupsByProfile.value(profileBKey, false), false);

    model.setProfileId(profileAKey);
    QTRY_COMPARE_WITH_TIMEOUT(model.hideUnchecked(), true, 3000);

    SettingsManager reloaded(tempDir.filePath(QStringLiteral("settings.json")));
    reloaded.load();
    QCOMPARE(reloaded.current().hideUncheckedGroupsByProfile.value(profileAKey, false), true);
    QCOMPARE(reloaded.current().hideUncheckedGroupsByProfile.value(profileBKey, false), false);
}

void AppModelTests::sourceGroupsModelReorderVisibleGroupsAppendsHiddenInRelativeOrder()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    DatabaseService database(tempDir.filePath(QStringLiteral("channels.sqlite")));
    database.ensureSchema();

    ServerProfile profile;
    profile.name = QStringLiteral("Visible reorder");
    settings.current().profiles = { profile };
    settings.save();

    const auto profileKey = guidToString(profile.id);
    settings.current().groupOrderByProfile[profileKey] = {
        QStringLiteral("__favourites__"),
        QStringLiteral("News"),
        QStringLiteral("Sports"),
        QStringLiteral("Movies"),
        QStringLiteral("Kids")
    };
    settings.current().hiddenGroupsByProfile[profileKey] = {
        QStringLiteral("Sports"),
        QStringLiteral("Kids")
    };
    settings.save();

    auto makeChannel = [&](const int id, const QString &name, const QString &category) {
        Channel channel;
        channel.id = id;
        channel.profileId = profile.id;
        channel.name = name;
        channel.categoryId = category;
        channel.categoryName = category;
        channel.streamUrl = QStringLiteral("http://127.0.0.1/%1").arg(name.toLower());
        channel.tvgId = QStringLiteral("%1.id").arg(name.toLower());
        channel.tvgName = name;
        return channel;
    };

    database.upsertChannels({
        makeChannel(1, QStringLiteral("News One"), QStringLiteral("News")),
        makeChannel(2, QStringLiteral("Sports One"), QStringLiteral("Sports")),
        makeChannel(3, QStringLiteral("Movies One"), QStringLiteral("Movies")),
        makeChannel(4, QStringLiteral("Kids One"), QStringLiteral("Kids"))
    });

    SourceGroupsModel model(&settings, &database);
    model.setAutoPersist(false);
    model.setProfileId(profileKey);
    QTRY_COMPARE_WITH_TIMEOUT(model.loading(), false, 3000);
    QVERIFY(model.totalCount() >= 5);

    QVERIFY(model.reorderVisibleGroups({
        QStringLiteral("Movies"),
        QStringLiteral("__favourites__"),
        QStringLiteral("News")
    }));

    QCOMPARE(model.get(0).value(QStringLiteral("id")).toString(), QStringLiteral("Movies"));
    QCOMPARE(model.get(1).value(QStringLiteral("id")).toString(), QStringLiteral("__favourites__"));
    QCOMPARE(model.get(2).value(QStringLiteral("id")).toString(), QStringLiteral("News"));
    QCOMPARE(model.get(3).value(QStringLiteral("id")).toString(), QStringLiteral("Sports"));
    QCOMPARE(model.get(4).value(QStringLiteral("id")).toString(), QStringLiteral("Kids"));

    QVERIFY(model.dirty());
    model.saveDraftChanges();
    QVERIFY(!model.dirty());

    const auto persistedOrder = settings.current().groupOrderByProfile.value(profileKey);
    QCOMPARE(persistedOrder.value(0), QStringLiteral("Movies"));
    QCOMPARE(persistedOrder.value(1), QStringLiteral("__favourites__"));
    QCOMPARE(persistedOrder.value(2), QStringLiteral("News"));
    QCOMPARE(persistedOrder.value(3), QStringLiteral("Sports"));
    QCOMPARE(persistedOrder.value(4), QStringLiteral("Kids"));
}

void AppModelTests::sourceGroupsModelClearsStaleRowsForInvalidProfile()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    DatabaseService database(tempDir.filePath(QStringLiteral("channels.sqlite")));

    ServerProfile profile;
    profile.id = QUuid::createUuid();
    profile.name = QStringLiteral("Source");
    profile.type = ProfileType::M3UFile;
    settings.current().profiles = { profile };
    settings.save();

    Channel channel;
    channel.id = 1;
    channel.profileId = profile.id;
    channel.name = QStringLiteral("News One");
    channel.categoryId = QStringLiteral("News");
    channel.categoryName = QStringLiteral("News");
    channel.streamUrl = QStringLiteral("http://127.0.0.1/news");
    channel.tvgId = QStringLiteral("news.one");
    channel.tvgName = channel.name;
    database.upsertChannels({ channel });

    SourceGroupsModel model(&settings, &database);
    model.setProfileId(guidToString(profile.id));
    QTRY_VERIFY_WITH_TIMEOUT(model.totalCount() > 0, 3000);
    QVERIFY(model.hasGroups());

    model.setProfileId(QStringLiteral("not-a-profile-id"));

    QCOMPARE(model.totalCount(), 0);
    QVERIFY(!model.hasGroups());
    QCOMPARE(model.selectedCount(), 0);
    QCOMPARE(model.hideUnchecked(), false);
}

void AppModelTests::channelListModelKeyboardSelectionHelpersWrapAndJump()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    ChannelListModel model(&settings);
    model.setActiveProfileId(QStringLiteral("profile-a"));

    Channel one;
    one.id = 1;
    one.name = QStringLiteral("Alpha");
    one.categoryId = QStringLiteral("News");
    one.sortOrder = 10;

    Channel two;
    two.id = 2;
    two.name = QStringLiteral("Beta");
    two.categoryId = QStringLiteral("News");
    two.sortOrder = 20;

    Channel three;
    three.id = 3;
    three.name = QStringLiteral("Gamma");
    three.categoryId = QStringLiteral("News");
    three.sortOrder = 30;

    model.setChannels(
        { three, one, two },
        { ChannelCategory { QStringLiteral("News"), QStringLiteral("News"), 0 } });

    QVERIFY(model.selectAt(0));
    QCOMPARE(model.selectedChannelId(), 1);

    QVERIFY(model.selectRelativeWrapped(-1));
    QCOMPARE(model.selectedChannelId(), 3);

    QVERIFY(model.selectRelativeWrapped(1));
    QCOMPARE(model.selectedChannelId(), 1);

    QVERIFY(model.selectAt(1));
    QCOMPARE(model.selectedChannelId(), 2);
    QCOMPARE(model.rowForChannelId(2), 1);
    QCOMPARE(model.rowForChannelId(99), -1);

    QVERIFY(model.selectRelativeWrapped(2));
    QCOMPARE(model.selectedChannelId(), 1);
    QCOMPARE(model.rowForChannelId(1), 0);
}

void AppModelTests::channelListModelSelectByIdNoOpWhenUnchanged()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    ChannelListModel model(&settings);
    model.setActiveProfileId(QStringLiteral("profile-a"));

    Channel one;
    one.id = 1;
    one.name = QStringLiteral("Alpha");
    one.categoryId = QStringLiteral("News");

    Channel two;
    two.id = 2;
    two.name = QStringLiteral("Beta");
    two.categoryId = QStringLiteral("News");

    model.setChannels(
        { one, two },
        { ChannelCategory { QStringLiteral("News"), QStringLiteral("News"), 0 } });

    QSignalSpy selectionSpy(&model, &ChannelListModel::selectedChannelIdChanged);

    QVERIFY(model.selectById(one.id));
    QCOMPARE(model.selectedChannelId(), one.id);
    QCOMPARE(selectionSpy.count(), 1);

    QVERIFY(model.selectById(one.id));
    QCOMPARE(model.selectedChannelId(), one.id);
    QCOMPARE(selectionSpy.count(), 1);
}

void AppModelTests::channelListModelActivatesByDisplayNumber()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    ChannelListModel model(&settings);
    model.setActiveProfileId(QStringLiteral("profile-a"));

    Channel m3uWithoutSort;
    m3uWithoutSort.id = 11;
    m3uWithoutSort.name = QStringLiteral("M3U Zero");
    m3uWithoutSort.categoryId = QStringLiteral("News");
    m3uWithoutSort.source = ChannelSource::M3U;
    m3uWithoutSort.sortOrder = 0;

    Channel m3uExplicitOne;
    m3uExplicitOne.id = 44;
    m3uExplicitOne.name = QStringLiteral("M3U One");
    m3uExplicitOne.categoryId = QStringLiteral("News");
    m3uExplicitOne.source = ChannelSource::M3U;
    m3uExplicitOne.sortOrder = 1;

    Channel xtreamWithoutSort;
    xtreamWithoutSort.id = 22;
    xtreamWithoutSort.name = QStringLiteral("Xtream Zero");
    xtreamWithoutSort.categoryId = QStringLiteral("News");
    xtreamWithoutSort.source = ChannelSource::Xtream;
    xtreamWithoutSort.sortOrder = 0;

    Channel xtreamWithSort;
    xtreamWithSort.id = 33;
    xtreamWithSort.name = QStringLiteral("Xtream Sorted");
    xtreamWithSort.categoryId = QStringLiteral("News");
    xtreamWithSort.source = ChannelSource::Xtream;
    xtreamWithSort.sortOrder = 77;

    model.setChannels(
        { xtreamWithoutSort, xtreamWithSort, m3uExplicitOne, m3uWithoutSort },
        { ChannelCategory { QStringLiteral("News"), QStringLiteral("News"), 0 } });

    QSignalSpy activationSpy(&model, &ChannelListModel::channelActivated);

    QVERIFY(model.activateByDisplayNumber(1));
    QCOMPARE(model.selectedChannelId(), 11);
    QCOMPARE(activationSpy.count(), 1);
    QCOMPARE(activationSpy.at(0).at(0).toInt(), 11);

    QVERIFY(model.activateByDisplayNumber(2));
    QCOMPARE(model.selectedChannelId(), 22);
    QCOMPARE(activationSpy.count(), 2);
    QCOMPARE(activationSpy.at(1).at(0).toInt(), 22);

    QVERIFY(model.activateByDisplayNumber(77));
    QCOMPARE(model.selectedChannelId(), 33);
    QCOMPARE(activationSpy.count(), 3);
    QCOMPARE(activationSpy.at(2).at(0).toInt(), 33);

    const auto selectedBeforeMissing = model.selectedChannelId();
    const auto activationCountBeforeMissing = activationSpy.count();
    QVERIFY(!model.activateByDisplayNumber(999));
    QCOMPARE(model.selectedChannelId(), selectedBeforeMissing);
    QCOMPARE(activationSpy.count(), activationCountBeforeMissing);
}

void AppModelTests::epgGridModelInitializesTimeWindow()
{
    EpgService epg;
    EpgGridModel model(&epg);

    const auto timeSlots = model.timeSlots();
    QVERIFY(!timeSlots.isEmpty());
    QCOMPARE(timeSlots.first().toMap().value(QStringLiteral("offsetMinutes")).toInt(), 0);
    QCOMPARE(model.windowSpanMinutes(), 1800);
    QCOMPARE(timeSlots.size(), model.windowSpanMinutes() / 60 + 1);
    QVERIFY(!model.windowStartLabel().isEmpty());
    QVERIFY(!model.windowEndLabel().isEmpty());

    Channel one;
    one.id = 11;
    one.name = QStringLiteral("BBC One");
    one.tvgId = QStringLiteral("bbc.one");

    Channel two;
    two.id = 22;
    two.name = QStringLiteral("BBC Two");
    two.tvgId = QStringLiteral("bbc.two");

    const auto start = QDateTime::currentDateTimeUtc().addSecs(-15 * 60);
    epg.loadFromEntries({
        EpgEntry { one.tvgId, QStringLiteral("Morning"), QStringLiteral("Show"), QString {}, start, start.addSecs(3600) },
        EpgEntry { two.tvgId, QStringLiteral("Noon"), QStringLiteral("Show"), QString {}, start, start.addSecs(3600) }
    });
    model.rebuild({ one, two }, 6, 6);

    QCOMPARE(model.rowIndexForChannelId(one.id), 0);
    QCOMPARE(model.rowIndexForChannelId(two.id), 1);
    QCOMPARE(model.channelIdAt(0), one.id);
    QCOMPARE(model.channelIdAt(1), two.id);
    QCOMPARE(model.channelIdAt(9), -1);
}

void AppModelTests::epgGridModelSelectionUpdatesOnlyAffectedRows()
{
    EpgService epg;
    EpgGridModel model(&epg);

    Channel one;
    one.id = 11;
    one.name = QStringLiteral("BBC One");
    one.tvgId = QStringLiteral("bbc.one");

    Channel two;
    two.id = 22;
    two.name = QStringLiteral("BBC Two");
    two.tvgId = QStringLiteral("bbc.two");

    const auto start = QDateTime::currentDateTimeUtc().addSecs(-15 * 60);
    epg.loadFromEntries({
        EpgEntry { one.tvgId, QStringLiteral("Morning"), QStringLiteral("Show"), QString {}, start, start.addSecs(3600) },
        EpgEntry { two.tvgId, QStringLiteral("Noon"), QStringLiteral("Show"), QString {}, start, start.addSecs(3600) }
    });
    model.rebuild({ one, two }, 6, 6);

    QSignalSpy dataSpy(&model, &EpgGridModel::dataChanged);

    model.setSelectedChannelId(one.id);
    QCOMPARE(dataSpy.count(), 1);
    {
        const auto args = dataSpy.takeFirst();
        const auto topLeft = qvariant_cast<QModelIndex>(args.at(0));
        const auto bottomRight = qvariant_cast<QModelIndex>(args.at(1));
        QCOMPARE(topLeft.row(), 0);
        QCOMPARE(bottomRight.row(), 0);
    }

    model.setSelectedProgramStart(start.toUTC().toString(Qt::ISODateWithMs));
    QCOMPARE(dataSpy.count(), 1);
    {
        const auto args = dataSpy.takeFirst();
        const auto topLeft = qvariant_cast<QModelIndex>(args.at(0));
        const auto bottomRight = qvariant_cast<QModelIndex>(args.at(1));
        QCOMPARE(topLeft.row(), 0);
        QCOMPARE(bottomRight.row(), 0);
    }

    model.setSelectedChannelId(two.id);
    QCOMPARE(dataSpy.count(), 2);
    {
        const auto first = dataSpy.takeFirst();
        const auto second = dataSpy.takeFirst();
        const auto firstRow = qvariant_cast<QModelIndex>(first.at(0)).row();
        const auto secondRow = qvariant_cast<QModelIndex>(second.at(0)).row();
        QVERIFY((firstRow == 0 && secondRow == 1) || (firstRow == 1 && secondRow == 0));
    }
}

void AppModelTests::epgGridModelNavigationHelpersFollowTimeAndBounds()
{
    EpgService epg;
    EpgGridModel model(&epg);

    Channel one;
    one.id = 11;
    one.name = QStringLiteral("BBC One");
    one.tvgId = QStringLiteral("bbc.one");

    Channel two;
    two.id = 22;
    two.name = QStringLiteral("BBC Two");
    two.tvgId = QStringLiteral("bbc.two");

    auto windowStart = QDateTime::currentDateTimeUtc();
    windowStart.setTime(QTime(windowStart.time().hour(), 0, 0, 0));

    const auto oneFirstStart = windowStart.addSecs(0);
    const auto oneSecondStart = windowStart.addSecs(30 * 60);
    const auto twoFirstStart = windowStart.addSecs(10 * 60);
    const auto twoSecondStart = windowStart.addSecs(40 * 60);

    epg.loadFromEntries({
        EpgEntry { one.tvgId, QStringLiteral("One A"), QStringLiteral("Show"), QString {}, oneFirstStart, oneFirstStart.addSecs(30 * 60) },
        EpgEntry { one.tvgId, QStringLiteral("One B"), QStringLiteral("Show"), QString {}, oneSecondStart, oneSecondStart.addSecs(30 * 60) },
        EpgEntry { two.tvgId, QStringLiteral("Two A"), QStringLiteral("Show"), QString {}, twoFirstStart, twoFirstStart.addSecs(30 * 60) },
        EpgEntry { two.tvgId, QStringLiteral("Two B"), QStringLiteral("Show"), QString {}, twoSecondStart, twoSecondStart.addSecs(30 * 60) }
    });

    model.rebuild({ one, two }, 6, 6);

    QCOMPARE(model.adjacentChannelId(-1, 1), -1);
    QCOMPARE(model.adjacentChannelId(one.id, 1), two.id);
    QCOMPARE(model.adjacentChannelId(two.id, 1), two.id);
    QCOMPARE(model.adjacentChannelId(one.id, -1), one.id);

    const auto nextProgram = model.adjacentProgram(one.id, oneFirstStart.toUTC().toString(Qt::ISODateWithMs), 1);
    QCOMPARE(nextProgram.value(QStringLiteral("title")).toString(), QStringLiteral("One B"));

    const auto previousProgram = model.adjacentProgram(one.id, oneFirstStart.toUTC().toString(Qt::ISODateWithMs), -1);
    QCOMPARE(previousProgram.value(QStringLiteral("title")).toString(), QStringLiteral("One A"));

    const auto midpoint = oneSecondStart.addSecs(15 * 60).toUTC().toString(Qt::ISODateWithMs);
    const auto sameTimeSlot = model.programForChannelAtTimestamp(two.id, midpoint);
    QCOMPARE(sameTimeSlot.value(QStringLiteral("title")).toString(), QStringLiteral("Two B"));
}

void AppModelTests::epgGridModelUsesConfiguredPastAndFutureWindow()
{
    EpgService epg;
    EpgGridModel model(&epg);

    Channel one;
    one.id = 11;
    one.name = QStringLiteral("BBC One");
    one.tvgId = QStringLiteral("bbc.one");

    Channel two;
    two.id = 22;
    two.name = QStringLiteral("BBC Two");
    two.tvgId = QStringLiteral("bbc.two");

    auto currentHour = QDateTime::currentDateTimeUtc();
    currentHour.setTime(QTime(currentHour.time().hour(), 0, 0, 0));

    const auto pastStart = currentHour.addSecs(-2 * 60 * 60);
    const auto currentStart = currentHour.addSecs(15 * 60);
    const auto futureStart = currentHour.addSecs(60 * 60);

    epg.loadFromEntries({
        EpgEntry { one.tvgId, QStringLiteral("Past"), QStringLiteral("Show"), QString {}, pastStart, pastStart.addSecs(60 * 60) },
        EpgEntry { one.tvgId, QStringLiteral("Current"), QStringLiteral("Show"), QString {}, currentStart, currentStart.addSecs(30 * 60) },
        EpgEntry { one.tvgId, QStringLiteral("Future"), QStringLiteral("Show"), QString {}, futureStart, futureStart.addSecs(30 * 60) },
        EpgEntry { two.tvgId, QStringLiteral("Past Only"), QStringLiteral("Show"), QString {}, pastStart, pastStart.addSecs(60 * 60) }
    });

    model.rebuild({ one, two }, 3, 5);

    QCOMPARE(model.windowSpanMinutes(), (3 + 5) * 60);
    QVERIFY(model.currentTimeOffsetMinutes() >= 180.0);
    QVERIFY(model.currentTimeOffsetMinutes() < 240.0);
    QCOMPARE(model.rowIndexForChannelId(two.id), 1);

    const auto pastProgram = model.programForChannelAtTimestamp(
        two.id,
        pastStart.addSecs(20 * 60).toUTC().toString(Qt::ISODateWithMs));
    QCOMPARE(pastProgram.value(QStringLiteral("title")).toString(), QStringLiteral("Past Only"));

    const auto movedPast = model.adjacentProgram(one.id, currentStart.toUTC().toString(Qt::ISODateWithMs), -1);
    QCOMPARE(movedPast.value(QStringLiteral("title")).toString(), QStringLiteral("Past"));
}

void AppModelTests::epgGridModelUsesConfiguredLookAheadWindow()
{
    EpgService epg;
    EpgGridModel model(&epg);

    Channel channel;
    channel.id = 11;
    channel.name = QStringLiteral("BBC One");
    channel.tvgId = QStringLiteral("bbc.one");

    auto windowStart = QDateTime::currentDateTimeUtc();
    windowStart.setTime(QTime(windowStart.time().hour(), 0, 0, 0));
    const auto farFutureStart = windowStart.addSecs(8 * 60 * 60);
    const auto farFutureStop = farFutureStart.addSecs(30 * 60);

    epg.loadFromEntries({
        EpgEntry { channel.tvgId, QStringLiteral("Current"), QStringLiteral("Show"), QString {}, windowStart, windowStart.addSecs(30 * 60) },
        EpgEntry { channel.tvgId, QStringLiteral("Far Future"), QStringLiteral("Show"), QString {}, farFutureStart, farFutureStop }
    });

    model.rebuild({ channel }, 6, 2);

    QCOMPARE(model.windowSpanMinutes(), 480);
    QCOMPARE(model.timeSlots().size(), model.windowSpanMinutes() / 60 + 1);
}

void AppModelTests::epgGridModelStreamsProgramsForViewport()
{
    EpgService epg;
    EpgGridModel model(&epg);

    Channel channel;
    channel.id = 11;
    channel.name = QStringLiteral("BBC One");
    channel.tvgId = QStringLiteral("bbc.one");

    auto start = QDateTime::currentDateTimeUtc();
    start.setTime(QTime(start.time().hour(), 0, 0, 0));

    QList<EpgEntry> entries;
    for (int hour = -6; hour < 12; ++hour) {
        const auto entryStart = start.addSecs(hour * 60 * 60);
        entries.push_back(EpgEntry {
            channel.tvgId,
            QStringLiteral("Slot %1").arg(hour),
            QStringLiteral("Show"),
            QString {},
            entryStart,
            entryStart.addSecs(60 * 60)
        });
    }

    epg.loadFromEntries(entries);
    model.rebuild({ channel }, 6, 12);
    model.setVisibleRowRange(0, 0);
    model.setRenderViewport(5 * 60, 90);

    const auto modelIndex = model.index(0, 0);
    const auto initialPrograms = model.data(modelIndex, EpgGridModel::ProgramsRole).toList();
    QVERIFY(!initialPrograms.isEmpty());
    QVERIFY(initialPrograms.size() < entries.size());

    auto hasTitle = [](const QVariantList &programs, const QString &title) {
        return std::any_of(programs.cbegin(), programs.cend(), [&title](const QVariant &value) {
            return value.toMap().value(QStringLiteral("title")).toString() == title;
        });
    };

    QVERIFY(hasTitle(initialPrograms, QStringLiteral("Slot -2")));
    QVERIFY(hasTitle(initialPrograms, QStringLiteral("Slot 0")));
    QVERIFY(!hasTitle(initialPrograms, QStringLiteral("Slot 8")));

    model.setRenderViewport(0, 90);
    const auto leftEdgePrograms = model.data(modelIndex, EpgGridModel::ProgramsRole).toList();
    QVERIFY(hasTitle(leftEdgePrograms, QStringLiteral("Slot -6")));
    QVERIFY(!hasTitle(leftEdgePrograms, QStringLiteral("Slot 6")));

    model.setRenderViewport(13 * 60, 90);
    const auto shiftedPrograms = model.data(modelIndex, EpgGridModel::ProgramsRole).toList();
    QVERIFY(!shiftedPrograms.isEmpty());
    QVERIFY(hasTitle(shiftedPrograms, QStringLiteral("Slot 7")));
    QVERIFY(!hasTitle(shiftedPrograms, QStringLiteral("Slot -6")));
}

void AppModelTests::appControllerGuideRebuildUsesConfiguredPastAndFutureRanges()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));

    harness.settings->current().guidePastHours = 4;
    harness.settings->current().epgLookAheadHours = 10;

    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    QCOMPARE(harness.epgGridModel->guidePastHours(), 4);
    QCOMPARE(harness.epgGridModel->lookAheadHours(), 10);
    QCOMPARE(harness.epgGridModel->windowSpanMinutes(), (4 + 10) * 60);

    harness.settingsController->setGuidePastHours(8);
    harness.settingsController->setEpgLookAheadHours(12);
    harness.settingsController->save();

    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->guidePastHours(), 8, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->lookAheadHours(), 12, 5000);
    QCOMPARE(harness.epgGridModel->windowSpanMinutes(), (8 + 12) * 60);
}

void AppModelTests::guideGridFilteringStaysIndependentFromLiveSearch()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));

    harness.appController->initialize();

    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    const auto channels = harness.channelListModel->allChannels();
    QCOMPARE(channels.size(), 2);

    QList<EpgEntry> entries;
    const auto now = QDateTime::currentDateTimeUtc().addSecs(-10 * 60);
    for (const auto &channel : channels) {
        entries.push_back(EpgEntry {
            channel.tvgId,
            QStringLiteral("Programme %1").arg(channel.name),
            QStringLiteral("Description %1").arg(channel.name),
            QString {},
            now,
            now.addSecs(3600)
        });
    }
    harness.epgService->loadFromEntries(entries);

    const auto newsChannel = std::find_if(channels.cbegin(), channels.cend(), [](const Channel &channel) {
        return channel.categoryId == QStringLiteral("News");
    });
    const auto sportsChannel = std::find_if(channels.cbegin(), channels.cend(), [](const Channel &channel) {
        return channel.categoryId == QStringLiteral("Sports");
    });
    QVERIFY(newsChannel != channels.cend());
    QVERIFY(sportsChannel != channels.cend());

    harness.settings->current().hiddenGroupsByProfile[guidToString(harness.activeProfileId())].clear();
    harness.channelListModel->refreshFilter();
    QCOMPARE(harness.channelListModel->filteredCount(), 2);

    harness.guideStateModel->setSelectedGroupId(QStringLiteral("Sports"));
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->rowCount(), 1, 2000);
    QCOMPARE(harness.epgGridModel->channelIdAt(0), sportsChannel->id);

    harness.guideStateModel->setSelectedGroupId(QStringLiteral("News"));
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->rowCount(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->channelIdAt(0), newsChannel->id, 2000);

    harness.channelListModel->setSearchText(QStringLiteral("Channel Two"));
    QCOMPARE(harness.channelListModel->filteredCount(), 1);
    QCOMPARE(harness.epgGridModel->rowCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->channelIdAt(0), newsChannel->id, 2000);

    harness.channelListModel->setWatchSeconds({
        { sportsChannel->id, 6 * 60 * 60 }
    });
    harness.guideStateModel->setSelectedGroupId(QStringLiteral("__favourites__"));
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->channelIdAt(0), sportsChannel->id, 2000);
}

void AppModelTests::startupResumeLastWatchedChannel()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(1));

    harness.appController->initialize();

    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.channelListModel->selectedChannelId(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.playerController->currentChannel().value(QStringLiteral("id")).toInt(), 1, 5000);
}

void AppModelTests::startupWithoutSavedChannelStaysBlack()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));

    harness.appController->initialize();

    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.channelListModel->selectedChannelId(), 0, 5000);
    QVERIFY(harness.playerController->currentChannel().isEmpty());
}

void AppModelTests::startupWithMissingSavedChannelStaysBlack()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(99));

    harness.appController->initialize();

    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.channelListModel->selectedChannelId(), 0, 5000);
    QVERIFY(harness.playerController->currentChannel().isEmpty());
}

void AppModelTests::nowNextModelRefreshIsAsyncAndDeduplicatesUpcoming()
{
    EpgService epg;
    const auto now = QDateTime::currentDateTimeUtc();
    epg.loadFromEntries({
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Current"),
            QString {},
            QString {},
            now.addSecs(-20 * 60),
            now.addSecs(20 * 60)
        },
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Next"),
            QString {},
            QString {},
            now.addSecs(20 * 60),
            now.addSecs(50 * 60)
        },
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Future 1"),
            QString {},
            QString {},
            now.addSecs(50 * 60),
            now.addSecs(80 * 60)
        },
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Future 2"),
            QString {},
            QString {},
            now.addSecs(80 * 60),
            now.addSecs(110 * 60)
        }
    });

    Channel channel;
    channel.id = 1;
    channel.name = QStringLiteral("Channel One");
    channel.tvgId = QStringLiteral("channel.one");

    NowNextModel model(&epg);
    QSignalSpy dataSpy(&model, &NowNextModel::dataChanged);
    model.setChannel(channel);

    QVERIFY(model.loading());
    QVERIFY(dataSpy.count() >= 1);
    QTRY_VERIFY_WITH_TIMEOUT(dataSpy.count() > 0, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 3000);
    QCOMPARE(model.currentProgram().value(QStringLiteral("title")).toString(), QStringLiteral("Current"));
    QCOMPARE(model.nextProgram().value(QStringLiteral("title")).toString(), QStringLiteral("Next"));

    const auto upcoming = model.upcomingPrograms();
    QCOMPARE(upcoming.size(), 2);
    QCOMPARE(upcoming.at(0).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Future 1"));
    QCOMPARE(upcoming.at(1).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Future 2"));
}

void AppModelTests::nowNextModelCoalescesRefreshRequestsToLatestSelection()
{
    EpgService epg;
    const auto now = QDateTime::currentDateTimeUtc();
    epg.loadFromEntries({
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Current One"),
            QString {},
            QString {},
            now.addSecs(-20 * 60),
            now.addSecs(20 * 60)
        },
        EpgEntry {
            QStringLiteral("channel.two"),
            QStringLiteral("Current Two"),
            QString {},
            QString {},
            now.addSecs(-20 * 60),
            now.addSecs(20 * 60)
        },
        EpgEntry {
            QStringLiteral("channel.three"),
            QStringLiteral("Current Three"),
            QString {},
            QString {},
            now.addSecs(-20 * 60),
            now.addSecs(20 * 60)
        }
    });

    Channel channelOne;
    channelOne.id = 1;
    channelOne.name = QStringLiteral("Channel One");
    channelOne.tvgId = QStringLiteral("channel.one");

    Channel channelTwo;
    channelTwo.id = 2;
    channelTwo.name = QStringLiteral("Channel Two");
    channelTwo.tvgId = QStringLiteral("channel.two");

    Channel channelThree;
    channelThree.id = 3;
    channelThree.name = QStringLiteral("Channel Three");
    channelThree.tvgId = QStringLiteral("channel.three");

    NowNextModel model(&epg);
    model.m_refreshInFlight = true;

    model.setChannel(channelOne);
    model.setChannel(channelTwo);
    model.setChannel(channelThree);

    QVERIFY(model.loading());
    QVERIFY(model.m_refreshQueued);
    QVERIFY(model.m_queuedChannel.has_value());
    QCOMPARE(model.m_queuedChannel->tvgId, QStringLiteral("channel.three"));

    NowNextModel::RefreshResult staleResult;
    model.applyRefreshResult(1, std::move(staleResult));

    QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 3000);
    QCOMPARE(model.channelName(), QStringLiteral("Channel Three"));
    QCOMPARE(model.currentProgram().value(QStringLiteral("title")).toString(), QStringLiteral("Current Three"));
    QVERIFY(!model.m_refreshQueued);
    QVERIFY(!model.m_queuedChannel.has_value());
}

void AppModelTests::nowNextModelExposesLoadingStateDuringRefresh()
{
    EpgService epg;
    const auto now = QDateTime::currentDateTimeUtc();
    epg.loadFromEntries({
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Current"),
            QString {},
            QString {},
            now.addSecs(-20 * 60),
            now.addSecs(20 * 60)
        }
    });

    Channel channel;
    channel.id = 1;
    channel.name = QStringLiteral("Channel One");
    channel.tvgId = QStringLiteral("channel.one");

    NowNextModel model(&epg);
    QSignalSpy dataSpy(&model, &NowNextModel::dataChanged);

    model.setChannel(channel);

    QVERIFY(model.loading());
    QTRY_VERIFY_WITH_TIMEOUT(dataSpy.count() > 0, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 3000);
    QCOMPARE(model.currentProgram().value(QStringLiteral("title")).toString(), QStringLiteral("Current"));
}

void AppModelTests::guideStateModelSelectChannelLoadsProgramsAsync()
{
    EpgService epg;
    const auto now = QDateTime::currentDateTimeUtc();
    epg.loadFromEntries({
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Current"),
            QString {},
            QString {},
            now.addSecs(-20 * 60),
            now.addSecs(20 * 60)
        },
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Future"),
            QString {},
            QString {},
            now.addSecs(20 * 60),
            now.addSecs(50 * 60)
        }
    });

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    GuideStateModel model(&epg, &settings);
    Channel channel;
    channel.id = 1;
    channel.name = QStringLiteral("Channel One");
    channel.tvgId = QStringLiteral("channel.one");
    model.setChannels({ channel });

    QSignalSpy selectedChannelSpy(&model, &GuideStateModel::selectedChannelIdChanged);
    QSignalSpy programsSpy(&model, &GuideStateModel::channelProgramsChanged);

    model.selectChannel(channel.id);

    QCOMPARE(selectedChannelSpy.count(), 1);
    QCOMPARE(programsSpy.count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(programsSpy.count() > 0, 3000);
    QVERIFY(!model.channelPrograms().isEmpty());
    QVERIFY(!model.selectedProgram().isEmpty());
}

void AppModelTests::guideStateModelSelectChannelNoOpWhenUnchanged()
{
    EpgService epg;
    const auto now = QDateTime::currentDateTimeUtc();
    epg.loadFromEntries({
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Current"),
            QString {},
            QString {},
            now.addSecs(-20 * 60),
            now.addSecs(20 * 60)
        }
    });

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    GuideStateModel model(&epg, &settings);
    Channel channel;
    channel.id = 1;
    channel.name = QStringLiteral("Channel One");
    channel.tvgId = QStringLiteral("channel.one");
    model.setChannels({ channel });

    QSignalSpy selectedChannelSpy(&model, &GuideStateModel::selectedChannelIdChanged);
    QSignalSpy programsSpy(&model, &GuideStateModel::channelProgramsChanged);

    model.selectChannel(channel.id);
    QTRY_VERIFY_WITH_TIMEOUT(programsSpy.count() > 0, 3000);
    const auto baselineProgramsCount = programsSpy.count();
    QCOMPARE(selectedChannelSpy.count(), 1);

    model.selectChannel(channel.id);
    QTest::qWait(120);

    QCOMPARE(selectedChannelSpy.count(), 1);
    QCOMPARE(programsSpy.count(), baselineProgramsCount);
}

void AppModelTests::guideStateModelRefreshesProgramsWhenSelectedChannelTvgIdChanges()
{
    EpgService epg;
    const auto now = QDateTime::currentDateTimeUtc();
    epg.loadFromEntries({
        EpgEntry {
            QStringLiteral("channel.old"),
            QStringLiteral("Old Guide"),
            QString {},
            QString {},
            now.addSecs(-20 * 60),
            now.addSecs(20 * 60)
        },
        EpgEntry {
            QStringLiteral("channel.new"),
            QStringLiteral("New Guide"),
            QString {},
            QString {},
            now.addSecs(-20 * 60),
            now.addSecs(20 * 60)
        }
    });

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    GuideStateModel model(&epg, &settings);
    Channel channel;
    channel.id = 1;
    channel.name = QStringLiteral("Channel One");
    channel.tvgId = QStringLiteral("channel.old");
    model.setChannels({ channel });
    model.selectChannel(channel.id);

    QTRY_COMPARE_WITH_TIMEOUT(
        model.selectedProgram().value(QStringLiteral("title")).toString(),
        QStringLiteral("Old Guide"),
        3000);

    channel.tvgId = QStringLiteral("channel.new");
    model.setChannels({ channel });

    QTRY_COMPARE_WITH_TIMEOUT(
        model.selectedProgram().value(QStringLiteral("title")).toString(),
        QStringLiteral("New Guide"),
        3000);
}

void AppModelTests::guideStateModelPreferredProgramStartSurvivesAsyncReload()
{
    EpgService epg;
    const auto now = QDateTime::currentDateTimeUtc();
    const auto currentStart = now.addSecs(-20 * 60);
    const auto futureStart = now.addSecs(20 * 60);
    epg.loadFromEntries({
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Current"),
            QString {},
            QString {},
            currentStart,
            now.addSecs(20 * 60)
        },
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Future"),
            QString {},
            QString {},
            futureStart,
            now.addSecs(50 * 60)
        }
    });

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    GuideStateModel model(&epg, &settings);
    Channel channel;
    channel.id = 1;
    channel.name = QStringLiteral("Channel One");
    channel.tvgId = QStringLiteral("channel.one");
    model.setChannels({ channel });

    model.selectChannel(channel.id);
    model.selectProgramByStart(futureStart.toUTC().toString(Qt::ISODateWithMs));

    QTRY_VERIFY_WITH_TIMEOUT(!model.channelPrograms().isEmpty(), 3000);
    QCOMPARE(model.selectedProgram().value(QStringLiteral("title")).toString(), QStringLiteral("Future"));
}

void AppModelTests::epgMissingCacheFetchesFromSource()
{
    const auto url = QUrl(QStringLiteral("https://example.com/guide.xml"));
    auto network = std::make_shared<MockNetworkAccess>();
    network->setResponse(url, { xmltvPayload(QStringLiteral("Fresh Only")), {}, 0 });

    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt, network, url.toString()));

    harness.appController->initialize();

    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgService->totalEntries(), 1, 5000);
    QCOMPARE(harness.epgService->allEntries().first().title, QStringLiteral("Fresh Only"));
    QCOMPARE(network->callCount(url), 1);
    QVERIFY(QFile::exists(AppDataPaths::epgCacheFile(harness.activeProfileId())));
}

void AppModelTests::epgFreshCacheSkipsNetworkUntilDue()
{
    const auto url = QUrl(QStringLiteral("https://example.com/guide.xml"));
    auto network = std::make_shared<MockNetworkAccess>();
    network->setResponse(url, { xmltvPayload(QStringLiteral("Fresh Network")), {}, 0 });

    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt, network, url.toString()));

    EpgCacheService cache;
    ServerProfile profile = harness.settings->current().profiles.first();
    EpgCacheService::CacheData data;
    data.profileId = profile.id;
    data.sourceFingerprint = EpgCacheService::sourceFingerprint(profile);
    data.fetchedAt = QDateTime::currentDateTimeUtc();
    data.snapshot = EpgService::buildSnapshot({
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Cached Fresh"),
            QStringLiteral("Cached description"),
            QString {},
            QDateTime::fromString(QStringLiteral("2026-03-17T18:00:00Z"), Qt::ISODate),
            QDateTime::fromString(QStringLiteral("2026-03-17T19:00:00Z"), Qt::ISODate)
        }
    });
    cache.save(data);

    harness.appController->initialize();

    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgService->totalEntries(), 1, 5000);
    QCOMPARE(harness.epgService->allEntries().first().title, QStringLiteral("Cached Fresh"));
    QTest::qWait(200);
    QCOMPARE(network->callCount(url), 0);
}

void AppModelTests::manualEpgRefreshBypassesFreshCache()
{
    const auto url = QUrl(QStringLiteral("https://example.com/guide.xml"));
    auto network = std::make_shared<MockNetworkAccess>();
    network->setResponse(url, { xmltvPayload(QStringLiteral("Manual Refresh")), {}, 150 });

    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt, network, url.toString()));

    EpgCacheService cache;
    ServerProfile profile = harness.settings->current().profiles.first();
    EpgCacheService::CacheData data;
    data.profileId = profile.id;
    data.sourceFingerprint = EpgCacheService::sourceFingerprint(profile);
    data.fetchedAt = QDateTime::currentDateTimeUtc();
    data.snapshot = EpgService::buildSnapshot({
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Cached Fresh"),
            QStringLiteral("Cached description"),
            QString {},
            QDateTime::fromString(QStringLiteral("2026-03-17T18:00:00Z"), Qt::ISODate),
            QDateTime::fromString(QStringLiteral("2026-03-17T19:00:00Z"), Qt::ISODate)
        }
    });
    cache.save(data);

    harness.appController->initialize();

    const QRegularExpression refreshStampPattern(QStringLiteral("^\\d{2}-\\d{2}-\\d{4} \\d{2}:\\d{2}$"));
    QTRY_VERIFY_WITH_TIMEOUT(refreshStampPattern.match(harness.appController->epgLastRefreshText()).hasMatch(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgService->totalEntries(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgService->allEntries().first().title, QStringLiteral("Cached Fresh"), 5000);
    QCOMPARE(network->callCount(url), 0);

    harness.appController->refreshActiveEpg();

    QTRY_VERIFY_WITH_TIMEOUT(harness.appController->epgRefreshInProgress(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(network->callCount(url), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgService->totalEntries(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgService->allEntries().first().title, QStringLiteral("Manual Refresh"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->epgRefreshInProgress(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(harness.appController->statusText().startsWith(QStringLiteral("EPG refreshed at ")), 5000);
}

void AppModelTests::epgStaleCacheLoadsThenRefreshesInBackground()
{
    const auto url = QUrl(QStringLiteral("https://example.com/guide.xml"));
    auto network = std::make_shared<MockNetworkAccess>();
    network->setResponse(url, { xmltvPayload(QStringLiteral("Fresh Network")), {}, 250 });

    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt, network, url.toString()));

    EpgCacheService cache;
    ServerProfile profile = harness.settings->current().profiles.first();
    EpgCacheService::CacheData data;
    data.profileId = profile.id;
    data.sourceFingerprint = EpgCacheService::sourceFingerprint(profile);
    data.fetchedAt = QDateTime::currentDateTimeUtc().addSecs(-720 * 60);
    data.snapshot = EpgService::buildSnapshot({
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Cached Stale"),
            QStringLiteral("Cached description"),
            QString {},
            QDateTime::fromString(QStringLiteral("2026-03-17T18:00:00Z"), Qt::ISODate),
            QDateTime::fromString(QStringLiteral("2026-03-17T19:00:00Z"), Qt::ISODate)
        }
    });
    cache.save(data);

    harness.appController->initialize();

    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgService->totalEntries(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgService->allEntries().first().title, QStringLiteral("Cached Stale"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgService->allEntries().first().title, QStringLiteral("Fresh Network"), 5000);
    QCOMPARE(network->callCount(url), 1);
}

void AppModelTests::epgRefreshFailureKeepsStaleCacheLoaded()
{
    const auto url = QUrl(QStringLiteral("https://example.com/guide.xml"));
    auto network = std::make_shared<MockNetworkAccess>();
    network->setResponse(url, { {}, QStringLiteral("simulated network failure"), 50 });

    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt, network, url.toString()));

    EpgCacheService cache;
    ServerProfile profile = harness.settings->current().profiles.first();
    EpgCacheService::CacheData data;
    data.profileId = profile.id;
    data.sourceFingerprint = EpgCacheService::sourceFingerprint(profile);
    data.fetchedAt = QDateTime::currentDateTimeUtc().addSecs(-720 * 60);
    data.snapshot = EpgService::buildSnapshot({
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Cached Stale"),
            QStringLiteral("Cached description"),
            QString {},
            QDateTime::fromString(QStringLiteral("2026-03-17T18:00:00Z"), Qt::ISODate),
            QDateTime::fromString(QStringLiteral("2026-03-17T19:00:00Z"), Qt::ISODate)
        }
    });
    cache.save(data);

    harness.appController->initialize();

    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgService->totalEntries(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgService->allEntries().first().title, QStringLiteral("Cached Stale"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(network->callCount(url), 1, 5000);
    QTest::qWait(100);
    QVERIFY(harness.appController->debugSummary().contains(QStringLiteral("simulated network failure")));
    QCOMPARE(harness.epgService->allEntries().first().title, QStringLiteral("Cached Stale"));
    QCOMPARE(network->callCount(url), 1);
}

QTEST_MAIN(AppModelTests)

#include "tst_app_models.moc"
