#include "../src/core/secretprotection.h"
#include <sstream>

#define private public
#include "../src/app/appcontroller.h"
#undef private
#include "../src/app/channellistmodel.h"
#include "../src/app/catchupdownloadcontroller.h"
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
#include "../src/core/trackpreferences.h"
#define private public
#include "../src/player/mpvplayer.h"
#undef private
#define private public
#include "../src/player/mpvvideoitem.h"
#undef private

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QThreadPool>
#include <QDataStream>
#include <QDir>
#include <QImage>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTcpSocket>
#include <QThread>
#include <QTimeZone>
#include <QtTest>
#include <QtConcurrentRun>

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
        if (!settings->addProfile(profile)) {
            return false;
        }
        auto activeId = profile.id;
        if (activeId.isNull()) {
            const auto summaries = settings->sourceSummaries();
            if (summaries.isEmpty()) {
                return false;
            }
            activeId = summaries.first().id;
        }
        settings->setActiveProfileId(activeId);
        if (lastWatchedChannelId.has_value()) {
            settings->current().lastWatchedChannelId.insert(guidToString(activeId), lastWatchedChannelId.value());
        }
        settings->save();

        database = std::make_unique<DatabaseService>();
        network = customNetwork ? std::move(customNetwork) : makeDefaultNetworkAccess();
        epgService = std::make_unique<EpgService>();
        profilesModel = std::make_unique<ProfilesModel>(settings.get());
        channelListModel = std::make_unique<ChannelListModel>(settings.get());
        nowNextModel = std::make_unique<NowNextModel>(epgService.get(), settings.get());
        playbackNowNextModel = std::make_unique<NowNextModel>(epgService.get(), settings.get());
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
        return settings->current().activeProfileId.value_or(QUuid {});
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
    void initTestCase() { OKILTV::Core::useIsolatedSecretKeyForTests(); }
    void catchupDownloadValidation();
    void trackPreferencesRejectStaleSnapshots();
    void removingProfileClearsTrackPreferences();
    void playerRemembersTracksAcrossRestartAndFallsBack();
    void profilesModelGetTracksActiveSource();
    void shellControllerRestoreLastViewClearsOverlayState();
    void shellControllerOpenOverlayPreservesOverlayStateExclusive();
    void appControllerKeepsSettingsOverlayOpenDuringProfileLoad();
    void appControllerKeepsGuideOpenedDuringProfileLoad();
    void appControllerLoadProfileDoesNotAutoActivateInactiveProfile();
    void playerControllerMuteToggleRestoresPreviousVolume();
    void appControllerRestoresVolumeAfterExit_data();
    void appControllerRestoresVolumeAfterExit();
    void multiviewVolumeChangesPreserveSelectedAudioTrack();
    void mpvPlayerEmitsObservedPauseChanges();
    void mpvPlayerLoadsPerFileOptions();
    void mpvPlayerReopensAudioOutputOnStreamReplacement_data();
    void mpvPlayerReopensAudioOutputOnStreamReplacement();
    void playerControllerPlaysAudioOnly_data();
    void playerControllerPlaysAudioOnly();
    void mpvPlayerStartsAudioAcrossMpegTsTimestampWrap_data();
    void mpvPlayerStartsAudioAcrossMpegTsTimestampWrap();
    void mpegTsTimestampNormalizerPreservesSourceClocks();
    void ownedStreamTimesOutWhenProviderStopsSending();
    void playerControllerIsPlayingTracksBackendPauseState();
    void playerControllerLiveTuneUsesFastStartupPolicy();
    void liveBufferTunerLearnsOnlyRepeatedDemandGaps();
    void liveBufferTunerIgnoresPauseSeekAndOutage();
    void liveBufferTunerBoundsAndRelaxesCapacity();
    void mpvLiveRefillPolicyResetsOnRetune();
    void mpvLiveContinuouslyRefillsBeyondReserve();
    void mpvImageSmoothingAppliesWithoutRetune();
    void mpvPicturePresetsPreserveUserShaders();
    void liveReservePreservesManualPauseAndStopsCleanly();
    void liveReservePausesOnlyAtDepletionRegardlessOfAdaptation();
    void liveReserveHandlesMissingTelemetryEofAndOutage();
    void liveReservePreservesDeliveryLearning();
    void playerControllerShowsLoadingIndicatorAfterTuneDelay_data();
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
    void playerControllerCatchupReconnectRestoresStreamPositionAfterFileLoaded();
    void playerControllerCatchupRollbackGuardCorrectsLargeWarmupRegression();
    void playerControllerCatchupRollbackGuardDefersCorrectionUntilSeekableOnInitialLoad();
    void playerControllerCatchupRollbackGuardDeferredCorrectionExpiresOnInitialLoad();
    void catchupOwnedStreamSessionClosesProviderWhileBufferedBytesRemainReadable();
    void catchupOwnedStreamSessionPreservesConfiguredRequestHeaders();
    void catchupOwnedStreamSessionCloseWithBackpressure_data();
    void catchupOwnedStreamSessionCloseWithBackpressure();
    void playerControllerCatchupDebugSnapshotUsesEffectiveBufferMetric();
    void playerControllerCatchupReconnectStabilizationIgnoresCacheDurationOutliers();
    void playerControllerXtreamCatchupSeekRegeneratesUrlTransparently();
    void playerControllerXtreamCatchupSeekWaitsForStopAckBeforeReload();
    void playerControllerXtreamCatchupSeekTimeoutReloadsWithoutStopAck();
    void playerControllerXtreamCatchupSeekCoalescesRapidClicksLastWins();
    void playerControllerCatchupRegeneratedSeekStopsReconnectWithoutStrictStartupRestore();
    void playerControllerCatchupDegradationWatchdogTriggersSilentRecovery();
    void playerControllerCatchupDegradationWatchdogNearEndSuppressesRecovery();
    void playerControllerCatchupDegradationWatchdogEscalatesToHardRestore();
    void playerControllerCatchupDegradationWatchdogPrefersSeamlessCutoverWhenStandbyReady();
    void playerControllerCatchupDegradationWatchdogFallsBackToHardRestoreWhenStandbyNotReady();
    void playerControllerCatchupProgrammeBoundaryBypassesSeamlessExtension();
    void playerControllerCatchupProgrammeBoundaryDetectionRequiresStopEdgeAndEndPosition();
    void playerControllerCatchupPastProgrammeDemuxerEofStopsLikeExplicitStop();
    void playerControllerCatchupSeamlessStandbyRetriesWithStopFirstBackoff();
    void playerControllerCatchupSeamlessStandbyRetryRefreshesXtreamUrl();
    void playerControllerCatchupSeamlessCutoverRequiresStandbyVideoReady();
    void playerControllerCatchupSeamlessSecondCycleTracksBaseStandbySignals();
    void playerControllerCatchupSeamlessStandbyFailureArmsFastRetry();
    void playerControllerCatchupDegradationWatchdogDefersHardRestoreDuringFastRetryWindow();
    void playerControllerCatchupEofCloseArmsSeamlessRolloverRegardlessOfCacheLevel();
    void playerControllerCatchupEofCloseWaitsBeforeStandbyWarmup();
    void playerControllerCatchupSeamlessCutoverDoesNotForceCloseNewActiveSessionFromStaleEofTick_data();
    void playerControllerCatchupSeamlessCutoverDoesNotForceCloseNewActiveSessionFromStaleEofTick();
    void playerControllerCatchupSeamlessRejectsDeadStandbySession_data();
    void playerControllerCatchupSeamlessRejectsDeadStandbySession();
    void playerControllerCatchupReconnectWaitsForTransportSettleBeforeAttempt();
    void playerControllerCatchupReconnectAttemptLimitEscalatesToHardRestore();
    void playerControllerCatchupTimelineShowsEpgEndAndReturnsLiveFromFuture();
    void playerControllerCatchupRefillsWithoutReplacingTransport();
    void playerControllerCatchupRefillTimeoutRequiresPlayableReserve();
    void playerControllerRecoversFailedContinuousAtWatchedPosition_data();
    void playerControllerRecoversFailedContinuousAtWatchedPosition();
    void playerControllerStandbyTimeoutLeavesWaitLoop();
    void playerControllerPlaysAcrossMpegTsConfigurationChange_data();
    void playerControllerPlaysAcrossMpegTsConfigurationChange();
    void playerControllerSkipsRetriedArchiveGap_data();
    void playerControllerSkipsRetriedArchiveGap();
    void playerControllerRecoveryWaitsForCachedResumePoint();
    void playerControllerEndlessCatchupChangesEpgWithoutRetuning();
    void appControllerArchivesContinueAcrossProgrammes_data();
    void appControllerArchivesContinueAcrossProgrammes();
    void playerControllerContinuousCatchupWaitsAndBoundsRecovery();
    void playerControllerArchiveCrossesProgrammesWithOneRequest_data();
    void playerControllerArchiveCrossesProgrammesWithOneRequest();
    void playerControllerCatchupCurrentProgramFollowsPlayback();
    void playerControllerSharedPrimarySignalsAndRetuneStayOnActivePlayer();
    void mpvPlayerDemuxerMaxBytesMapping();
    void mpvPlayerCacheWindowSecondsMapping();
    void mpvPlayerSteadyStateCacheBandMapping();
    void mpvPlayerRenderCallbackUpdatesHeartbeatTimestamp();
    void mpvPlayerInitializationErrorAllowsStateQueries();
    void mpvPlayerDefersIdleBackendInitialization();
    void playerControllerStartupBufferFallbackTimeoutMapping();
    void playerControllerAdaptiveSteadyStateMaxBytesMapping();
    void playerControllerCatchupCacheBudgetsAreBounded_data();
    void playerControllerCatchupCacheBudgetsAreBounded();
    void playerControllerLiveReconnectAllowsFullStabilizationWindow();
    void playerControllerReconnectRebuildsReserveOnSameConnection();
    void playerControllerReconnectReserveHonorsStopAndFailure();
    void playerControllerReconnectReserveTimesOutWithoutProgress();
    void playerControllerLiveWatchdogRespectsTuneGrace();
    void timeshiftControllerPreparingUntilPlaybackAttached();
    void timeshiftControllerProbeKeepsMetadataWithStderr();
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
    void dateTimeFormatsApplyOnlyOnSaveAndRefreshCachedPrograms();
    void settingsControllerTracksDirtyStateForRegularSettings();
    void settingsControllerPreviewsUiTransparency();
    void settingsControllerDisablesFfmpegDependentOptionsWhenToolsUnavailable();
    void settingsControllerAllowsFfmpegDependentOptionsWhenToolsAvailable();
    void timeshiftControllerServesPlaybackOverLocalHttp();
    void timeshiftControllerStartupCleanupOnlyRemovesManagedSessionDirectories();
    void timeshiftControllerCurrentPlaybackEpochStaysOnAttachedStreamWhileDelayedLoadIsPending();
    void timeshiftUserStopRequestKillsIngestImmediately();
    void timeshiftUserChannelSwitchRequestKillsIngestImmediately();
    void multiviewPictureInPictureEmptyOpenAssignsFocusedSecondaryAndClosesOnToggle();
    void catchupPipPreservesIndependentSessionsOnSwapAndClose();
    void catchupPipGuideStartsSecondArchiveAndTracksBothBookmarks();
    void catchupPipConvertsExistingLivePip();
    void catchupPipPendingRedirectIsCancelledWhenClosed();
    void multiviewControllerAllowsCatchupPipButBlocksGrid();
    void multiviewControllerOpensPictureInPictureGridAndSwapsChannels();
    void multiviewPromotedPipCanPauseAfterRepeatedSwaps();
    void multiviewPrimaryTileReflectsPlaybackPlayerObjectChanges();
    void mpvVideoItemSharedPlayerDetachDoesNotClearOtherRenderTarget();
    void appControllerRoutesActivationToFocusedMultiviewTile();
    void appControllerActivatingPipChannelSwapsWithoutRetune();
    void appControllerSameChannelActivationSkipsRetuneWhileActiveOrInFlight();
    void appControllerSameChannelActivationRetunesLiveWhenCatchupActive();
    void appControllerSourceActivationStopsCatchupBeforeCrossProfileLoad();
    void appControllerPlayCatchupResolvesLegacyM3uTimeshiftTemplate();
    void appControllerCatchupResumePersistsAndHonorsExplicitStart_data();
    void appControllerCatchupResumePersistsAndHonorsExplicitStart();
    void appControllerCatchupProgressFollowsEndlessProgramme();
    void playerControllerCatchupProgressIgnoresUnconfirmedTransport();
    void playerControllerQueuedProgrammeStopCannotStopNewTune();
    void playerControllerRecoveryRespectsPauseAndPendingReload();
    void playerControllerCatchupResumeReportsRealPlayback_data();
    void playerControllerCatchupResumeReportsRealPlayback();
    void appControllerPlayCatchupGuideUtcPayloadResolvesExpectedEpochUrl();
    void appControllerPlayCatchupGuideOffsetPayloadResolvesExpectedEpochUrl();
    void appControllerPlayCatchupXtreamPreResolvesRedirectUrl();
    void appControllerPlayCatchupXtreamRedirectResolutionDoesNotBlockUiThread();
    void appControllerPlayCatchupXtreamRedirectFailureFallsBackToOriginalUrl();
    void appControllerPlayCatchupAtOffsetXtreamLiveProgramUsesOriginDurationDelta();
    void appControllerPlayCatchupRejectsUnresolvedTemplate();
    void appControllerPlayCatchupRejectsFutureProgram();
    void appControllerPlayCatchupRejectsRunningProgramBeforeSourceMargin_data();
    void appControllerPlayCatchupRejectsRunningProgramBeforeSourceMargin();
    void appControllerPlayCatchupAllowsRunningProgramAfterSourceMargin_data();
    void appControllerPlayCatchupAllowsRunningProgramAfterSourceMargin();
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
    void multiviewStartupCleanupPreservesPlaybackAndRunsOnce();
    void multiviewRetiredSecondarySignalsDoNotCorruptReusedSlot();
    void multiviewPrimaryAssignmentRetunesSharedPrimaryInPlaceAfterSwap();
    void dvrControllerToggleScheduleAndExitGuard();
    void dvrControllerFinalizeActiveWindowSchedulesRestart();
    void dvrControllerRestartStateMaintainedAndClearedByWindowState();
    void dvrControllerWindowsStoppedJobWithoutFinishedFinalizes();
    void dvrControllerWindowsWaitsForDescendants();
    void dvrControllerRemuxDeletesTempWhenDurationMatchesRegardlessOfExitCode();
    void dvrControllerRemuxKeepsTempWhenDurationMismatched();
    void portableRuntimeControllerTracksPortableOverrideWithoutDirtyingSettings();
    void channelListModelRestoresSavedGroup();
    void channelListModelSupportsAutoFavouritesAndGroupPrefs();
    void channelListModelWatchUpdatesPreserveFavouriteRows();
    void channelListModelReplacementIsConsistentDuringNotifications_data();
    void channelListModelReplacementIsConsistentDuringNotifications();
    void channelListModelHidesDeselectedGroupsUntilExplicitGroupIsChosen();
    void channelListModelKeyboardSelectionHelpersWrapAndJump();
    void channelListModelSelectByIdNoOpWhenUnchanged();
    void channelListModelActivatesByDisplayNumber();
    void channelListModelExposesCurrentProgramRoles();
    void channelListModelExposesDvrRecordingRole();
    void sourceGroupsModelAppliesSelectionThresholdAndPersistsReorder();
    void sourceGroupsThreshold_data();
    void startupGroupSyncDoesNotDecryptChannelUrls();
    void sourceGroupsThreshold();
    void sourceGroupsRefreshPreservesDrafts();
    void sourceGroupImportNotices();
    void sourceGroupsModelReorderVisibleGroupsAppendsHiddenInRelativeOrder();
    void sourceGroupsModelClearsStaleRowsForInvalidProfile();
    void epgGridModelRefreshPreservesViewport();
    void epgGridModelInitializesTimeWindow();
    void epgGridModelSelectionUpdatesOnlyAffectedRows();
    void epgGridModelNavigationHelpersFollowTimeAndBounds();
    void epgGridModelUsesConfiguredPastAndFutureWindow();
    void epgGridModelUsesConfiguredLookAheadWindow();
    void epgGridModelStreamsProgramsForViewport();
    void appControllerGuideRebuildUsesConfiguredPastAndFutureRanges();
    void guideGridFilteringStaysIndependentFromLiveSearch();
    void guideGridInvalidatesQueuedResultsBeforeReplacement();
    void startupResumeLastWatchedChannel();
    void startupRestoresCatchup_data();
    void startupRestoresCatchup();
    void startupRestoresEndlessXtreamCatchup();
    void startupWithoutSavedChannelStaysBlack();
    void startupWithMissingSavedChannelStaysBlack();
    void nowNextModelRefreshIsAsyncAndDeduplicatesUpcoming();
    void nowNextModelUsesConfiguredLookAhead();
    void nowNextModelArchiveHistory();
    void nowNextModelHistoryDoesNotLeakAcrossChannels();
    void appControllerRefreshesNowNextWhenLookAheadIsSaved();
    void nowNextModelCoalescesRefreshRequestsToLatestSelection();
    void nowNextModelExposesLoadingStateDuringRefresh();
    void guideStateModelSelectChannelLoadsProgramsAsync();
    void guideStateModelSelectChannelNoOpWhenUnchanged();
    void guideStateModelRefreshesProgramsWhenSelectedChannelTvgIdChanges();
    void guideStateModelPreferredProgramStartSurvivesAsyncReload();
    void guideStateModelRefreshPreservesBrowsedProgram_data();
    void guideStateModelRefreshPreservesBrowsedProgram();
    void epgMissingCacheFetchesFromSource();
    void m3uDiscoversEpgAndRespectsOverride();
    void m3uSourcesSaveArchiveSafetyMargin();
    void m3uRefreshRetainsChannelIdentity();
    void epgFreshCacheSkipsNetworkUntilDue();
    void manualEpgRefreshBypassesFreshCache();
    void epgStaleCacheLoadsThenRefreshesInBackground();
    void epgRefreshFailureKeepsStaleCacheLoaded();
    void xtreamProfileRefreshKeepsStoredTimezoneWhenResponseMissingTimezone();
    void scheduledSourceAutoRefreshTriggersAtExactIntervalBoundary();
    void sourceRefreshFailureWithCachedFallbackKeepsPreviousLastRefreshed();
    void profileRefreshPreservesSettingsDraft_data();
    void profileRefreshPreservesSettingsDraft();
    void invalidPlaylistRefreshKeepsCachedChannels_data();
    void invalidPlaylistRefreshKeepsCachedChannels();
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

void AppModelTests::appControllerKeepsGuideOpenedDuringProfileLoad()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));

    QSignalSpy profileLoadSpy(harness.appController.get(), &AppController::profileLoadFinished);
    harness.appController->loadProfile(guidToString(harness.activeProfileId()));
    harness.shellController->openOverlay(QStringLiteral("guide"));
    QSignalSpy overlaySpy(harness.shellController.get(), &ShellController::activeOverlayChanged);
    QTRY_VERIFY_WITH_TIMEOUT(!profileLoadSpy.isEmpty(), 8000);

    QVERIFY(profileLoadSpy.last().at(1).toBool());
    QCOMPARE(harness.shellController->activeOverlay(), QStringLiteral("guide"));
    QVERIFY(harness.shellController->overlaysVisible());
    QCOMPARE(overlaySpy.count(), 0);
}

void AppModelTests::removingProfileClearsTrackPreferences()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    const auto profileId = harness.activeProfileId();
    const auto profile = guidToString(profileId);
    harness.settings->setChannelTrackPreference(profile, QStringLiteral("xtream:1"), QStringLiteral("sub"),
        makeTrackPreference({}, QStringLiteral("sub"), 0));
    QVERIFY(harness.settings->current().channelTrackPreferences.contains(profile));
    QVERIFY(harness.settings->removeProfile(profileId));
    QVERIFY(!harness.settings->current().channelTrackPreferences.contains(profile));
    SettingsManager reloaded(harness.settings->settingsFilePath());
    reloaded.load();
    QVERIFY(!reloaded.current().channelTrackPreferences.contains(profile));
}

void AppModelTests::trackPreferencesRejectStaleSnapshots()
{
    OKILTV::Player::MpvPlayer player;
    const QString audio = QStringLiteral("audio");
    const QString sub = QStringLiteral("sub");
    const QVariantList original {
        QVariantMap { {QStringLiteral("type"), audio}, {QStringLiteral("id"), 2}, {QStringLiteral("lang"), QStringLiteral("eng")} }
    };
    const QJsonObject preferences { {audio, makeTrackPreference(original, audio, 2)},
        {sub, makeTrackPreference({}, sub, 0)} };
    const auto profile = guidToString(QUuid::createUuid());
    player.configureTrackPreferences(profile, QStringLiteral("xtream:1"), preferences);
    QSignalSpy changed(&player, &OKILTV::Player::MpvPlayer::trackPreferenceChanged);
    player.beginTrackLoad(QStringLiteral("current"));
    const auto oldGeneration = player.m_trackGeneration.load();
    player.beginTrackLoad(QStringLiteral("current"));
    const auto generation = player.m_trackGeneration.load();
    const QVariantList missing {
        QVariantMap { {QStringLiteral("type"), audio}, {QStringLiteral("id"), 1},
            {QStringLiteral("lang"), QStringLiteral("pol")}, {QStringLiteral("selected"), true} }
    };
    player.acceptTrackSnapshot(generation, QStringLiteral("current"), missing); // Not loaded yet.
    player.m_tracksLoadedGeneration.store(generation);
    player.acceptTrackSnapshot(oldGeneration, QStringLiteral("current"), missing);
    player.acceptTrackSnapshot(generation, QStringLiteral("previous"), missing);
    player.acceptTrackSnapshot(generation, QStringLiteral("current"), {});
    QCOMPARE(changed.count(), 0);
    player.configureTrackPreferences(profile, QStringLiteral("xtream:1"), preferences, false);
    player.acceptTrackSnapshot(generation, QStringLiteral("current"), missing);
    QCOMPARE(changed.count(), 0); // A local remux may omit a provider track.
    player.configureTrackPreferences(profile, QStringLiteral("xtream:1"), preferences, true);
    player.acceptTrackSnapshot(generation, QStringLiteral("current"), missing);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.front().at(2).toString(), audio);
    QVERIFY(changed.front().at(3).toJsonObject().isEmpty());
    QCOMPARE(player.m_trackPreferences.value(sub), preferences.value(sub));
    player.beginTrackLoad({});
    player.acceptTrackSnapshot(generation, QStringLiteral("current"), missing);
    QCOMPARE(changed.count(), 1);
    QVERIFY(!player.prepareRememberedTrack(audio, 1));
}

void AppModelTests::playerRemembersTracksAcrossRestartAndFallsBack()
{
    const auto ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) { QSKIP("ffmpeg required for the multiple-track fixture."); }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto subtitlePath = dir.filePath(QStringLiteral("captions.srt"));
    QFile subtitles(subtitlePath);
    QVERIFY(subtitles.open(QIODevice::WriteOnly));
    subtitles.write("1\n00:00:00,000 --> 00:01:00,000\nFixture subtitles\n");
    subtitles.close();
    const auto media = dir.filePath(QStringLiteral("tracks.mkv"));
    QProcess generator;
    generator.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=frequency=440:duration=60"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=frequency=880:duration=60"),
        QStringLiteral("-i"), subtitlePath,
        QStringLiteral("-map"), QStringLiteral("0:a"), QStringLiteral("-map"), QStringLiteral("1:a"),
        QStringLiteral("-map"), QStringLiteral("2:s"), QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"),
        QStringLiteral("-c:s"), QStringLiteral("srt"),
        QStringLiteral("-metadata:s:a:0"), QStringLiteral("language=pol"),
        QStringLiteral("-metadata:s:a:1"), QStringLiteral("language=eng"),
        QStringLiteral("-disposition:a:0"), QStringLiteral("default"),
        QStringLiteral("-disposition:a:1"), QStringLiteral("0"), media});
    QVERIFY(generator.waitForFinished(30000));
    QCOMPARE(generator.exitCode(), 0);
    const auto single = dir.filePath(QStringLiteral("single.mkv"));
    generator.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), media,
        QStringLiteral("-map"), QStringLiteral("0:a:0"), QStringLiteral("-c"), QStringLiteral("copy"), single});
    QVERIFY(generator.waitForFinished(30000));
    QCOMPARE(generator.exitCode(), 0);

    const auto headless = qgetenv("OKILTV_HEADLESS_TEST");
    const auto restore = qScopeGuard([&]() {
        if (headless.isNull()) { qunsetenv("OKILTV_HEADLESS_TEST"); }
        else { qputenv("OKILTV_HEADLESS_TEST", headless); }
    });
    qputenv("OKILTV_HEADLESS_TEST", "0");
    Channel channel;
    channel.id = 37;
    channel.source = ChannelSource::Xtream;
    channel.profileId = QUuid::createUuid();
    channel.name = QStringLiteral("Track fixture");
    channel.streamUrl = QUrl::fromLocalFile(media).toString();
    const auto profile = guidToString(channel.profileId);
    const auto key = trackPreferenceChannelKey(channel);
    const auto settingsPath = dir.filePath(QStringLiteral("settings.json"));
    const QMap<QString, QString> options {
        {QStringLiteral("vo"), QStringLiteral("null")}, {QStringLiteral("ao"), QStringLiteral("null")},
        {QStringLiteral("hwdec"), QStringLiteral("no")}, {QStringLiteral("sid"), QStringLiteral("no")}
    };
    {
        SettingsManager settings(settingsPath);
        PlayerController controller;
        controller.setTrackPreferenceSettings(&settings);
        controller.applySettings({}, options, 5.0, false, 1.0, QStringLiteral("test"));
        controller.playChannel(channel);
        auto *backend = controller.player();
        QTRY_VERIFY2_WITH_TIMEOUT(backend->m_readyTrackGeneration != 0,
            qPrintable(QStringLiteral("expected=%1 actual=%2 generation=%3 loaded=%4 tracks=%5 diagnostics=%6")
                .arg(backend->m_trackExpectedPath, backend->propertyString("path").value_or(QString {}))
                .arg(backend->m_trackGeneration.load()).arg(backend->m_tracksLoadedGeneration.load())
                .arg(backend->trackList().size()).arg(backend->diagnostics())), 8000);
        QCOMPARE(selectedTrackId(backend->m_readyTracks, QStringLiteral("audio")), 1);
        QCOMPARE(selectedTrackId(backend->m_readyTracks, QStringLiteral("sub")), 0);
        controller.selectAudioTrack(2);
        controller.selectSubtitleTrack(1);
        QTRY_COMPARE_WITH_TIMEOUT(settings.channelTrackPreferences(profile, key).size(), 2, 5000);
        controller.stop();
    }
    SettingsManager settings(settingsPath);
    settings.load();
    PlayerController controller;
    controller.setTrackPreferenceSettings(&settings);
    controller.applySettings({}, options, 5.0, false, 1.0, QStringLiteral("test"));
    controller.playChannel(channel);
    auto *backend = controller.player();
    QTRY_COMPARE_WITH_TIMEOUT(selectedTrackId(backend->m_readyTracks, QStringLiteral("audio")), 2, 8000);
    QTRY_COMPARE_WITH_TIMEOUT(selectedTrackId(backend->m_readyTracks, QStringLiteral("sub")), 1, 5000);
    controller.selectSubtitleTrack(0);
    QTRY_COMPARE_WITH_TIMEOUT(settings.channelTrackPreferences(profile, key).value(QStringLiteral("sub"))
        .toObject().value(QStringLiteral("mode")).toString(), QStringLiteral("off"), 5000);
    backend->setPaused(true);
    QTRY_VERIFY_WITH_TIMEOUT(backend->pauseState().value_or(false), 3000);
    controller.selectAudioTrack(1);
    QTRY_VERIFY_WITH_TIMEOUT(!settings.channelTrackPreferences(profile, key).contains(QStringLiteral("audio")), 5000);
    controller.selectAudioTrack(2);
    QTRY_VERIFY_WITH_TIMEOUT(settings.channelTrackPreferences(profile, key).contains(QStringLiteral("audio")), 5000);
    controller.stop();
    controller.playChannel(channel);
    QTRY_COMPARE_WITH_TIMEOUT(selectedTrackId(backend->m_readyTracks, QStringLiteral("audio")), 2, 8000);
    QCOMPARE(selectedTrackId(backend->m_readyTracks, QStringLiteral("sub")), 0);

    auto other = channel;
    other.profileId = QUuid::createUuid();
    controller.playChannel(other);
    QTRY_VERIFY_WITH_TIMEOUT(backend->m_readyTrackGeneration != 0, 8000);
    QCOMPARE(selectedTrackId(backend->m_readyTracks, QStringLiteral("audio")), 1);
    other.profileId = channel.profileId;
    other.id = 38;
    controller.playChannel(other);
    QTRY_VERIFY_WITH_TIMEOUT(backend->m_readyTrackGeneration != 0, 8000);
    QCOMPARE(selectedTrackId(backend->m_readyTracks, QStringLiteral("audio")), 1);
    controller.playChannel(channel);
    QTRY_COMPARE_WITH_TIMEOUT(selectedTrackId(backend->m_readyTracks, QStringLiteral("audio")), 2, 8000);
    channel.streamUrl = QUrl::fromLocalFile(single).toString();
    controller.playChannel(channel);
    QTRY_VERIFY_WITH_TIMEOUT(backend->m_readyTrackGeneration != 0, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(!settings.channelTrackPreferences(profile, key).contains(QStringLiteral("audio")), 5000);
    QCOMPARE(selectedTrackId(backend->m_readyTracks, QStringLiteral("audio")), 1);
    QCOMPARE(settings.channelTrackPreferences(profile, key).value(QStringLiteral("sub"))
        .toObject().value(QStringLiteral("mode")).toString(), QStringLiteral("off"));
    channel.streamUrl = QUrl::fromLocalFile(media).toString();
    controller.playChannel(channel);
    QTRY_VERIFY_WITH_TIMEOUT(backend->m_readyTrackGeneration != 0, 8000);
    QCOMPARE(selectedTrackId(backend->m_readyTracks, QStringLiteral("audio")), 1);
    controller.selectSubtitleTrack(1);
    QTRY_COMPARE_WITH_TIMEOUT(settings.channelTrackPreferences(profile, key).value(QStringLiteral("sub"))
        .toObject().value(QStringLiteral("mode")).toString(), QStringLiteral("track"), 5000);
    channel.streamUrl = QUrl::fromLocalFile(single).toString();
    controller.playChannel(channel);
    QTRY_VERIFY_WITH_TIMEOUT(backend->m_readyTrackGeneration != 0, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(settings.channelTrackPreferences(profile, key).isEmpty(), 5000);
    controller.stop();
}

void AppModelTests::multiviewVolumeChangesPreserveSelectedAudioTrack()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    auto *player = harness.playerController->player();
    QVERIFY2(player->ensureInitialized(), qPrintable(player->diagnostics()));

    player->setAudioEnabled(true);
    player->selectAudioTrack(2);
    QTRY_COMPARE_WITH_TIMEOUT(player->propertyInt("aid").value_or(-1), 2, 2000);

    harness.playerController->toggleMute();
    QTRY_COMPARE_WITH_TIMEOUT(player->propertyDouble("volume").value_or(-1.0), 0.0, 2000);
    QCOMPARE(player->propertyInt("aid").value_or(-1), 2);
    harness.playerController->toggleMute();
    QTRY_COMPARE_WITH_TIMEOUT(
        player->propertyDouble("volume").value_or(-1.0), harness.playerController->volume(), 2000);
    QCOMPARE(player->propertyInt("aid").value_or(-1), 2);

    player->setAudioEnabled(false);
    QTRY_COMPARE_WITH_TIMEOUT(player->propertyString("aid").value_or(QString()), QStringLiteral("no"), 2000);
    player->setAudioEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(player->propertyString("aid").value_or(QString()), QStringLiteral("auto"), 2000);
    player->selectAudioTrack(3);
    QTRY_COMPARE_WITH_TIMEOUT(player->propertyInt("aid").value_or(-1), 3, 2000);
    harness.playerController->setVolume(35);
    QTRY_COMPARE_WITH_TIMEOUT(player->propertyDouble("volume").value_or(-1.0), 35.0, 2000);
    QCOMPARE(player->propertyInt("aid").value_or(-1), 3);

    player->selectSubtitleTrack(1);
    QTRY_COMPARE_WITH_TIMEOUT(player->propertyInt("sid").value_or(-1), 1, 2000);
    player->selectSubtitleTrack(0);
    QTRY_COMPARE_WITH_TIMEOUT(player->propertyString("sid").value_or(QString()), QStringLiteral("no"), 2000);
}

void AppModelTests::mpvPlayerEmitsObservedPauseChanges()
{
    const auto previousHeadless = qgetenv("OKILTV_HEADLESS_TEST");
    const auto restore = qScopeGuard([&]() {
        if (previousHeadless.isNull()) { qunsetenv("OKILTV_HEADLESS_TEST"); }
        else { qputenv("OKILTV_HEADLESS_TEST", previousHeadless); }
    });
    qputenv("OKILTV_HEADLESS_TEST", "0");
    OKILTV::Player::MpvPlayer player;
    player.configureOptions({{QStringLiteral("vo"), QStringLiteral("null")},
                             {QStringLiteral("ao"), QStringLiteral("null")}});
    QSignalSpy pauseSpy(&player, &OKILTV::Player::MpvPlayer::pauseStateChanged);
    QVERIFY(player.ensureInitialized());
    QTRY_VERIFY_WITH_TIMEOUT(!pauseSpy.isEmpty(), 3000);
    for (const bool paused : {true, false, true, false}) {
        pauseSpy.clear();
        player.setPaused(paused);
        QTRY_VERIFY_WITH_TIMEOUT(!pauseSpy.isEmpty(), 3000);
        QCOMPARE(pauseSpy.last().first().toBool(), paused);
        QCOMPARE(player.pauseState(), std::optional<bool>(paused));
    }
}

void AppModelTests::mpvPlayerLoadsPerFileOptions()
{
    const auto previousHeadless = qgetenv("OKILTV_HEADLESS_TEST");
    const auto restore = qScopeGuard([&]() {
        if (previousHeadless.isNull()) { qunsetenv("OKILTV_HEADLESS_TEST"); }
        else { qputenv("OKILTV_HEADLESS_TEST", previousHeadless); }
    });
    qputenv("OKILTV_HEADLESS_TEST", "0");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("frame with spaces,comma.png"));
    QImage frame(16, 16, QImage::Format_RGB32);
    frame.fill(Qt::red);
    QVERIFY(frame.save(path));

    OKILTV::Player::MpvPlayer player;
    player.configureOptions({ {QStringLiteral("vo"), QStringLiteral("null")},
                              {QStringLiteral("ao"), QStringLiteral("null")} });
    QSignalSpy loaded(&player, &OKILTV::Player::MpvPlayer::fileLoaded);
    QSignalSpy errors(&player, &OKILTV::Player::MpvPlayer::errorOccurred);
    // mpv 0.37 rejects the positional '-1' introduced in 0.38 as an options
    // string. Verify actual loading and per-file options on either version.
    player.play(path, QStringLiteral("pause=yes,image-display-duration=30"));
    QVERIFY2(errors.isEmpty(), qPrintable(player.diagnostics()));
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty() || !errors.isEmpty(), 5000);
    QVERIFY2(errors.isEmpty(), qPrintable(player.diagnostics()));
    QVERIFY(!loaded.isEmpty());
    QCOMPARE(player.pauseState(), std::optional<bool>(true));
    QCOMPARE(player.propertyDouble("image-display-duration"), std::optional<double>(30.0));
}

void AppModelTests::mpvPlayerReopensAudioOutputOnStreamReplacement_data()
{
    QTest::addColumn<bool>("overrideGapless");
    QTest::newRow("independent-streams") << false;
    QTest::newRow("explicit-gapless-override") << true;
}

void AppModelTests::mpvPlayerReopensAudioOutputOnStreamReplacement()
{
    QFETCH(bool, overrideGapless);
    const auto previousHeadless = qgetenv("OKILTV_HEADLESS_TEST");
    const auto previousTrace = qgetenv("OKILTV_TRACE_MPV");
    const auto restoreEnvironment = qScopeGuard([&]() {
        if (previousHeadless.isNull()) {
            qunsetenv("OKILTV_HEADLESS_TEST");
        } else {
            qputenv("OKILTV_HEADLESS_TEST", previousHeadless);
        }
        if (previousTrace.isNull()) {
            qunsetenv("OKILTV_TRACE_MPV");
        } else {
            qputenv("OKILTV_TRACE_MPV", previousTrace);
        }
    });
    // Exercise real loadfile/event processing, using null outputs for CI.
    qputenv("OKILTV_HEADLESS_TEST", "0");
    qputenv("OKILTV_TRACE_MPV", "1");

    QTemporaryDir mediaDirectory;
    QVERIFY(mediaDirectory.isValid());
    const auto firstPath = mediaDirectory.filePath(QStringLiteral("channel-a.wav"));
    const auto secondPath = mediaDirectory.filePath(QStringLiteral("channel-b.wav"));
    QFile media(firstPath);
    QVERIFY(media.open(QIODevice::WriteOnly));
    constexpr quint32 sampleRate = 48000;
    constexpr quint32 sampleCount = sampleRate * 10;
    constexpr quint32 dataBytes = sampleCount * 2;
    QDataStream wave(&media);
    wave.setByteOrder(QDataStream::LittleEndian);
    wave.writeRawData("RIFF", 4);
    wave << quint32(36 + dataBytes);
    wave.writeRawData("WAVEfmt ", 8);
    wave << quint32(16) << quint16(1) << quint16(1) << sampleRate;
    wave << quint32(sampleRate * 2) << quint16(2) << quint16(16);
    wave.writeRawData("data", 4);
    wave << dataBytes;
    for (quint32 sample = 0; sample < sampleCount; ++sample) {
        wave << qint16(sample % 100 < 50 ? 1000 : -1000);
    }
    QCOMPARE(wave.status(), QDataStream::Ok);
    media.close();
    QVERIFY(QFile::copy(firstPath, secondPath));

    OKILTV::Player::MpvPlayer player;
    QMap<QString, QString> options {
        { QStringLiteral("vo"), QStringLiteral("null") },
        { QStringLiteral("ao"), QStringLiteral("null") },
        { QStringLiteral("load-scripts"), QStringLiteral("no") },
    };
    if (overrideGapless) {
        options.insert(QStringLiteral("gapless-audio"), QStringLiteral("weak"));
    }
    player.configureOptions(options);
    QSignalSpy restarted(&player, &OKILTV::Player::MpvPlayer::playbackRestarted);
    QSignalSpy errors(&player, &OKILTV::Player::MpvPlayer::errorOccurred);
    auto &logger = DebugLogger::instance();
    const auto cursor = logger.latestCursor();
    const auto audioOutputOpenCount = [&]() {
        const auto entries = logger.entriesSince(cursor);
        return std::count_if(entries.cbegin(), entries.cend(), [](const auto &entry) {
            return entry.category == QStringLiteral("mpv-log")
                && entry.message.startsWith(QStringLiteral("[cplayer] AO: [null]"));
        });
    };

    // Matching audio formats are intentional: mpv's default weak gapless mode
    // reuses the output here, unlike a full Stop -> Play or a format change.
    const QStringList channels { firstPath, secondPath, firstPath };
    for (int index = 0; index < channels.size(); ++index) {
        player.play(channels.at(index));
        QTRY_COMPARE_WITH_TIMEOUT(restarted.count(), index + 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(audioOutputOpenCount(), overrideGapless ? 1 : index + 1, 2000);
        QCOMPARE(errors.count(), 0);
    }

    // Closing PiP now queues stop without waiting for decoder teardown. Verify
    // completion, and that a rapid Stop -> Play cannot stop the replacement.
    QSignalSpy stopped(&player, &OKILTV::Player::MpvPlayer::playbackStopped);
    player.stop();
    QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
    for (const auto &channel : channels) {
        const auto restartCount = restarted.count();
        player.play(channel);
        QTRY_COMPARE_WITH_TIMEOUT(restarted.count(), restartCount + 1, 5000);
        player.stop();
        player.play(channel);
        QTRY_COMPARE_WITH_TIMEOUT(restarted.count(), restartCount + 2, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(player.position() > 0.2, 5000);
        QCOMPARE(errors.count(), 0);
    }
}

void AppModelTests::mpegTsTimestampNormalizerPreservesSourceClocks()
{
    constexpr quint64 mask = (quint64 { 1 } << 33) - 1;
    constexpr quint64 safeStart = quint64 { 1 } << 32;
    const auto pes = [](quint64 pts, std::optional<quint64> dts) {
        QByteArray result(dts.has_value() ? 19 : 14, '\0');
        result[2] = 1;
        result[3] = static_cast<char>(dts.has_value() ? 0xe0 : 0xc0);
        result[6] = static_cast<char>(0x80);
        result[7] = static_cast<char>(dts.has_value() ? 0xc0 : 0x80);
        result[8] = static_cast<char>(dts.has_value() ? 10 : 5);
        const auto write = [&](int offset, quint64 clock, unsigned tag) {
            result[offset] = static_cast<char>(tag | (((clock >> 30) & 7U) << 1) | 1U);
            result[offset + 1] = static_cast<char>(clock >> 22);
            result[offset + 2] = static_cast<char>((((clock >> 15) & 0x7fU) << 1) | 1U);
            result[offset + 3] = static_cast<char>(clock >> 7);
            result[offset + 4] = static_cast<char>(((clock & 0x7fU) << 1) | 1U);
        };
        write(9, pts, dts.has_value() ? 0x30U : 0x20U);
        if (dts.has_value()) {
            write(14, *dts, 0x10U);
        }
        return result;
    };
    const auto packet = [&](int pid, const QByteArray &payload, bool start, std::optional<quint64> pcr) {
        QByteArray result(188, static_cast<char>(0xff));
        result[0] = 0x47;
        result[1] = static_cast<char>((pid >> 8) | (start ? 0x40 : 0));
        result[2] = static_cast<char>(pid);
        result[3] = 0x30;
        result[4] = static_cast<char>(183 - payload.size());
        result[5] = static_cast<char>(pcr.has_value() ? 0x18 : 0);
        if (pcr.has_value()) {
            for (int index = 0; index < 2; ++index) {
                const auto clock = (*pcr + static_cast<quint64>(index) * 45000) & mask;
                const auto offset = 6 + index * 6;
                result[offset] = static_cast<char>(clock >> 25);
                result[offset + 1] = static_cast<char>(clock >> 17);
                result[offset + 2] = static_cast<char>(clock >> 9);
                result[offset + 3] = static_cast<char>(clock >> 1);
                // Preserve a nonzero 27 MHz extension and all reserved bits.
                result[offset + 4] = static_cast<char>(((clock & 1U) << 7) | 0x7fU);
                result[offset + 5] = 0x2b;
            }
        }
        result.replace(188 - payload.size(), payload.size(), payload);
        return result;
    };
    const auto source = [&](quint64 origin, bool split, bool pcr) {
        const auto video = pes((origin + 9000) & mask, origin);
        const auto audio = pes((origin - 180000) & mask, std::nullopt);
        auto result = packet(256, split ? video.left(10) : video, true,
            pcr ? std::optional<quint64>(origin) : std::nullopt);
        result += packet(257, audio, true, std::nullopt);
        if (split) {
            result += packet(256, video.mid(10), false, std::nullopt);
        }
        result += packet(8191, QByteArrayLiteral("unchanged payload"), false, std::nullopt);
        return result;
    };
    const QList<quint64> origins { 0, 123456789, mask - 45000 };
    for (const auto origin : origins) {
        for (const auto split : { false, true }) {
            for (const auto pcr : { false, true }) {
                const auto input = source(origin, split, pcr);
                const auto expected = source(safeStart, split, pcr);
                for (const auto chunkSize : { 1, 7, 187, 188, 193, 4096 }) {
                    OKILTV::Player::MpegTsTimestampNormalizer normalizer;
                    QByteArray output;
                    for (qsizetype offset = 0; offset < input.size(); offset += chunkSize) {
                        output += normalizer.push(input.mid(offset, chunkSize));
                    }
                    output += normalizer.finish();
                    QVERIFY(normalizer.active());
                    QCOMPARE(output, expected);
                }
            }
        }
    }
    for (const auto &input : { QByteArrayLiteral("#EXTM3U\n"), QByteArray(1024, 'x'), QByteArray() }) {
        OKILTV::Player::MpegTsTimestampNormalizer normalizer;
        auto output = normalizer.push(input);
        output += normalizer.finish();
        QCOMPARE(output, input);
        QVERIFY(!normalizer.active());
    }
}

void AppModelTests::playerControllerPlaysAudioOnly_data()
{
    QTest::addColumn<QString>("format");
    QTest::addColumn<bool>("catchup");
    QTest::newRow("mpeg-ts-radio") << QStringLiteral("ts") << false;
    QTest::newRow("mp3-radio") << QStringLiteral("mp3") << false;
    QTest::newRow("aac-adts-radio") << QStringLiteral("aac") << false;
    QTest::newRow("mpeg-ts-radio-catchup") << QStringLiteral("ts") << true;
}

void AppModelTests::playerControllerPlaysAudioOnly()
{
    QFETCH(QString, format);
    QFETCH(bool, catchup);
    const auto ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) { QSKIP("ffmpeg needed to generate radio fixture."); }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("radio.") + format);
    QProcess generator;
    generator.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("sine=frequency=440:sample_rate=48000:duration=30"),
        QStringLiteral("-c:a"), format == QStringLiteral("mp3") ? QStringLiteral("libmp3lame") : QStringLiteral("aac"), path});
    QVERIFY(generator.waitForFinished(15000));
    QVERIFY2(generator.exitCode() == 0, generator.readAllStandardError().constData());
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto source = file.readAll();
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    int requests = 0;
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (auto *socket = server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
                const auto request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n") || socket->property("sent").toBool()) { return; }
                socket->setProperty("sent", true);
                ++requests;
                // Keep the live connection open after sending enough audio for the test.
                socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + source);
            });
        }
    });
    const auto previousHeadless = qgetenv("OKILTV_HEADLESS_TEST");
    const auto restore = qScopeGuard([&]() {
        if (previousHeadless.isNull()) { qunsetenv("OKILTV_HEADLESS_TEST"); }
        else { qputenv("OKILTV_HEADLESS_TEST", previousHeadless); }
    });
    qputenv("OKILTV_HEADLESS_TEST", "0");
    PlayerController controller;
    controller.applySettings({}, {{QStringLiteral("vo"), QStringLiteral("null")},
        {QStringLiteral("ao"), QStringLiteral("null")}, {QStringLiteral("hwdec"), QStringLiteral("no")}},
        5.0, false, 3.0, {});
    Channel channel;
    channel.id = 456;
    channel.name = QStringLiteral("Radio");
    channel.streamUrl = QStringLiteral("http://127.0.0.1:%1/timeshift/test/test/10/2026-09-20:00-00/456.%2").arg(server.serverPort()).arg(format);
    auto *player = controller.playbackPlayer();
    QSignalSpy errors(player, &OKILTV::Player::MpvPlayer::errorOccurred);
    if (catchup) {
        const auto start = QDateTime::currentDateTimeUtc().addSecs(-1200);
        controller.playCatchupChannel(channel, channel.streamUrl, QStringLiteral("Radio programme"),
            start, start.addSecs(600), channel.streamUrl);
    } else {
        controller.playChannel(channel);
    }
    const auto stop = qScopeGuard([&]() { controller.stop(); });
    QTRY_VERIFY_WITH_TIMEOUT(player->propertyDouble("audio-pts").value_or(0.0) > 0.2, 8000);
    QTRY_VERIFY(controller.isPlaying());
    QTRY_VERIFY(!controller.isLoading());
    QTRY_VERIFY(!controller.channelSwitchInProgress());
    QVERIFY(!player->videoCodec().has_value());
    QVERIFY(player->audioCodec().has_value());
    controller.togglePause();
    QTRY_VERIFY(!controller.isPlaying());
    controller.togglePause();
    QTRY_VERIFY(controller.isPlaying());
    const auto position = player->position();
    QTRY_VERIFY_WITH_TIMEOUT(player->position() > position + 1.0, 3000);
    QVERIFY(!controller.channelLoadFailed());
    QVERIFY(!controller.m_hwdecFallbackApplied);
    QCOMPARE(requests, 1);
    QCOMPARE(errors.count(), 0);
}

void AppModelTests::mpvPlayerStartsAudioAcrossMpegTsTimestampWrap_data()
{
    QTest::addColumn<bool>("disableNormalization");
    QTest::addColumn<QString>("userAgent");
    QTest::newRow("normalized-audio-starts-in-sync") << false << QStringLiteral("OKILTV-regression");
    QTest::newRow("original-timestamps-reproduce-silence") << true << QStringLiteral("OKILTV-regression");
    QTest::newRow("empty-agent-uses-okiltv") << false << QString();
    QTest::newRow("blank-agent-direct-mpv-uses-okiltv") << true << QStringLiteral("   ");
}

void AppModelTests::mpvPlayerStartsAudioAcrossMpegTsTimestampWrap()
{
    QFETCH(bool, disableNormalization);
    QFETCH(QString, userAgent);
    const auto expectedUserAgent = userAgent.trimmed().isEmpty()
        ? OKILTV::Core::defaultPlayerUserAgent() : userAgent.trimmed();
    QVERIFY(OKILTV::Player::CatchupStreamSession::requestHeadersFromOptions(userAgent, {})
                .contains(qMakePair(QByteArrayLiteral("User-Agent"), expectedUserAgent.toUtf8())));
    const auto ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        QSKIP("Requires ffmpeg to generate a synthetic MPEG-TS timestamp-wrap fixture.");
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto originalPath = directory.filePath(QStringLiteral("original.ts"));
    QProcess generator;
    generator.start(ffmpeg, {
        QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("testsrc2=size=160x90:rate=25:duration=18"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("sine=frequency=440:sample_rate=48000:duration=20"),
        QStringLiteral("-filter:a"), QStringLiteral("asetpts=PTS-2/TB"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"),
        QStringLiteral("-preset"), QStringLiteral("ultrafast"),
        QStringLiteral("-g"), QStringLiteral("25"), QStringLiteral("-bf"), QStringLiteral("0"),
        QStringLiteral("-c:a"), QStringLiteral("aac"), QStringLiteral("-copyts"),
        QStringLiteral("-avoid_negative_ts"), QStringLiteral("disabled"),
        QStringLiteral("-mpegts_copyts"), QStringLiteral("1"),
        QStringLiteral("-muxdelay"), QStringLiteral("0"), originalPath,
    });
    QVERIFY(generator.waitForFinished(15000));
    QCOMPARE(generator.exitStatus(), QProcess::NormalExit);
    QVERIFY2(generator.exitCode() == 0, generator.readAllStandardError().constData());
    QFile original(originalPath);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const auto transport = original.readAll();
    QCOMPARE(transport.size() % 188, 0);

    // Deliver video PTS=0 before the leading audio PTS=-2s (encoded modulo
    // 2^33). This makes libavformat choose the post-wrap timestamp origin,
    // reproducing audio near 95442s versus video near zero from the user log.
    // Keep packet ordering within each PID, including PAT/PMT and continuations.
    QByteArray prefix;
    QByteArray remaining;
    int videoStarts = 0;
    for (qsizetype offset = 0; offset < transport.size(); offset += 188) {
        const auto packet = transport.mid(offset, 188);
        QCOMPARE(static_cast<unsigned char>(packet[0]), 0x47);
        const auto pid = ((static_cast<unsigned char>(packet[1]) & 0x1f) << 8)
            | static_cast<unsigned char>(packet[2]);
        if (pid == 256 && (packet[1] & 0x40) != 0) {
            ++videoStarts;
        }
        if (pid != 257 && videoStarts <= 3) {
            prefix.append(packet);
        } else {
            remaining.append(packet);
        }
    }
    QVERIFY(videoStarts > 3);
    const auto wrappedPath = directory.filePath(QStringLiteral("wrapped.ts"));
    QFile wrapped(wrappedPath);
    QVERIFY(wrapped.open(QIODevice::WriteOnly));
    QCOMPARE(wrapped.write(prefix), prefix.size());
    QCOMPARE(wrapped.write(remaining), remaining.size());
    wrapped.close();

    int requests = 0;
    int disconnected = 0;
    bool headersForwarded = true;
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (auto *socket = server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::disconnected, &server, [&]() { ++disconnected; });
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
                const auto request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n") || socket->property("sent").toBool()) {
                    return;
                }
                socket->setProperty("sent", true);
                ++requests;
                headersForwarded = headersForwarded && request.contains("X-Player-Test: timestamp-wrap")
                    && request.contains("User-Agent: " + expectedUserAgent.toUtf8() + "\r\n");
                // No Content-Length and no range support: exercise the live
                // HTTP path, not file seeking or a fully downloaded clip.
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nConnection: close\r\n\r\n");
                socket->write(request.startsWith("GET /healthy.ts") ? transport : prefix + remaining);
            });
        }
    });
    const auto previousHeadless = qgetenv("OKILTV_HEADLESS_TEST");
    const auto previousDisable = qgetenv("OKILTV_DISABLE_TS_TIMESTAMP_NORMALIZATION");
    const auto restoreEnvironment = qScopeGuard([&]() {
        if (previousDisable.isNull()) {
            qunsetenv("OKILTV_DISABLE_TS_TIMESTAMP_NORMALIZATION");
        } else {
            qputenv("OKILTV_DISABLE_TS_TIMESTAMP_NORMALIZATION", previousDisable);
        }
        if (previousHeadless.isNull()) {
            qunsetenv("OKILTV_HEADLESS_TEST");
        } else {
            qputenv("OKILTV_HEADLESS_TEST", previousHeadless);
        }
    });
    qputenv("OKILTV_HEADLESS_TEST", "0");
    qputenv("OKILTV_DISABLE_TS_TIMESTAMP_NORMALIZATION", disableNormalization ? "1" : "0");
    OKILTV::Player::MpvPlayer player;
    QMap<QString, QString> options {
        { QStringLiteral("vo"), QStringLiteral("null") },
        { QStringLiteral("ao"), QStringLiteral("null") },
        { QStringLiteral("hwdec"), QStringLiteral("no") },
        { QStringLiteral("load-scripts"), QStringLiteral("no") },
    };
    options.insert(QStringLiteral("http-header-fields"), QStringLiteral("X-Player-Test: timestamp-wrap"));
    player.configureUserAgent(userAgent);
    player.configureOptions(options);
    QSignalSpy errors(&player, &OKILTV::Player::MpvPlayer::errorOccurred);
    const auto wrappedUrl = QStringLiteral("http://127.0.0.1:%1/wrapped.ts").arg(server.serverPort());
    const auto healthyUrl = QStringLiteral("http://127.0.0.1:%1/healthy.ts").arg(server.serverPort());
    if (disableNormalization) {
        player.play(wrappedUrl);
        QTRY_VERIFY_WITH_TIMEOUT(player.position() > 1.0, 5000);
        QVERIFY(player.propertyString("current-ao").has_value());
        QCOMPARE(player.propertyInt("aid").value_or(-1), 1);
        QVERIFY(!player.propertyDouble("audio-pts").has_value());
    } else {
        QSignalSpy loaded(&player, &OKILTV::Player::MpvPlayer::fileLoaded);
        const QStringList urls { wrappedUrl, healthyUrl, wrappedUrl };
        for (int index = 0; index < urls.size(); ++index) {
            player.play(urls[index]);
            QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), index + 1, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(player.propertyDouble("audio-pts").has_value(), 2000);
            const auto audioPosition = player.propertyDouble("audio-pts").value();
            // Correct timestamps must synchronize immediately, not after many
            // seconds of gradual drift compensation with initial-audio-sync=no.
            QVERIFY(audioPosition >= 0.0 && audioPosition < 10.0);
            QVERIFY(std::abs(player.propertyDouble("avsync").value_or(100000.0)) < 0.1);
            QTRY_VERIFY_WITH_TIMEOUT(player.propertyDouble("audio-pts").value_or(audioPosition) > audioPosition + 0.2, 2000);
            QVERIFY(std::abs(player.propertyDouble("avsync").value_or(100000.0)) < 0.1);
        }
        QCOMPARE(requests, 3);
        player.stop();
        QTRY_COMPARE_WITH_TIMEOUT(disconnected, 3, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(player.m_retiredLiveStreams.isEmpty(), 3000);
    }
    QVERIFY(headersForwarded);
    QCOMPARE(errors.count(), 0);
}

void AppModelTests::profilesModelGetTracksActiveSource()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedRuntimeContext runtimeContext;
    RuntimeContext context;
    context.launchMode = LaunchMode::Portable;
    context.dataRootOverride = tempDir.path();
    AppDataPaths::initializeRuntime(context);
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    ServerProfile first;
    first.name = QStringLiteral("Source A");
    first.type = ProfileType::M3UUrl;
    first.isActive = true;
    first.m3uUrl = QStringLiteral("https://example.invalid/a.m3u");
    QVERIFY(settings.addProfile(first));
    settings.setActiveProfileId(first.id);
    ServerProfile second;
    second.name = QStringLiteral("Source B");
    QVERIFY(settings.addProfile(second));
    ProfilesModel model(&settings);
    QSignalSpy changed(&model, &ProfilesModel::dataChanged);

    for (const auto &activeId : { second.id, first.id }) {
        QVERIFY(model.selectProfile(guidToString(activeId)));
        QCOMPARE(model.activeProfileId(), guidToString(activeId));
        for (int row = 0; row < model.rowCount(); ++row) {
            const auto profile = model.get(row);
            const auto expected = profile.value(QStringLiteral("id")).toString() == guidToString(activeId);
            QCOMPARE(profile.value(QStringLiteral("isActive")).toBool(), expected);
            QCOMPARE(model.data(model.index(row, 0), ProfilesModel::IsActiveRole).toBool(), expected);
        }
        QCOMPARE(model.get(0).value(QStringLiteral("m3UUrl")).toString(), first.m3uUrl);
    }
    QCOMPARE(changed.count(), 2);
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

void AppModelTests::appControllerRestoresVolumeAfterExit_data()
{
    QTest::addColumn<double>("volume");
    QTest::newRow("adjusted") << 37.0;
    QTest::newRow("muted") << 0.0;
    QTest::newRow("maximum") << 100.0;
}

void AppModelTests::appControllerRestoresVolumeAfterExit()
{
    QFETCH(double, volume);
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.settings->setActiveProfileId(std::nullopt);
    harness.playerController->setVolume(volume);
    harness.appController->savePlaybackForApplicationExit();
    harness.settings->load();
    QCOMPARE(harness.settings->current().playerVolume, volume);
    harness.playerController->setVolume(75.0);
    harness.appController->initialize();
    QCOMPARE(harness.playerController->volume(), volume);
    QCOMPARE(harness.playerController->muted(), volume == 0.0);
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

void AppModelTests::liveBufferTunerLearnsOnlyRepeatedDemandGaps()
{
    using OKILTV::Player::LiveBufferTuner;
    LiveBufferTuner tuner;
    // Continuous delivery and cache-full reads never imply a batched provider.
    for (int tick = 0; tick < 400; ++tick) {
        const auto now = static_cast<double>(tick) / 4.0;
        tuner.observe(now, now, tick % 8 < 4);
    }
    QCOMPARE(tuner.targetSeconds(5.0), 5.0);
    tuner.reset();
    for (int tick = 0; tick <= 84; ++tick) {
        const auto now = static_cast<double>(tick) / 4.0;
        const auto burst = tick / 28;
        // A 7-second provider cycle includes one second of intentional cache idle.
        tuner.observe(now, static_cast<double>(burst * 7), tick % 28 < 4);
        if (tick < 84) {
            QCOMPARE(tuner.targetSeconds(5.0), 5.0);
        }
    }
    QCOMPARE(tuner.detectedIntervalSeconds(), 7.0);
    QCOMPARE(tuner.targetSeconds(5.0), 8.5);
    QCOMPARE(tuner.targetSeconds(20.0), 20.0); // Preserve a user's larger setting.
    tuner.reset();
    for (int tick = 0; tick <= 88; ++tick) {
        const auto now = static_cast<double>(tick) / 4.0;
        const auto edge = now >= 22.0 ? 22.0 : (now >= 14.0 ? 14.0 : (now >= 7.0 ? 7.0 : 0.0));
        tuner.observe(now, edge, false);
    }
    QCOMPARE(tuner.detectedIntervalSeconds(), 8.0);
    QCOMPARE(tuner.targetSeconds(3.0), 10.0); // 7/8-second jitter requires an extra 2-second margin.
}

void AppModelTests::liveBufferTunerIgnoresPauseSeekAndOutage()
{
    using OKILTV::Player::LiveBufferTuner;
    LiveBufferTuner tuner;
    for (int tick = 0; tick <= 56; ++tick) {
        tuner.observe(static_cast<double>(tick) / 4.0, static_cast<double>((tick / 28) * 7), false);
    }
    tuner.interruptObservation(); // Pause/resume or a manual cache seek.
    for (int tick = 100; tick <= 156; ++tick) {
        tuner.observe(static_cast<double>(tick) / 4.0, static_cast<double>(((tick - 100) / 28) * 7), false);
    }
    QCOMPARE(tuner.targetSeconds(5.0), 5.0);
    tuner.observe(100.0, 50.0, false); // Missing samples / suspended app.
    tuner.observe(100.25, 2.0, false); // Timeline reset / backwards seek.
    tuner.observe(100.5, std::numeric_limits<double>::quiet_NaN(), false);
    QCOMPARE(tuner.targetSeconds(5.0), 5.0);
    tuner.reset();
    for (int tick = 0; tick <= 480; ++tick) {
        tuner.observe(static_cast<double>(tick) / 4.0, static_cast<double>((tick / 160) * 40), false);
    }
    QCOMPARE(tuner.targetSeconds(5.0), 5.0); // 40-second outages aren't a cache-sizing signal.
    tuner.reset();
    for (int tick = 0; tick <= 300; ++tick) {
        tuner.observe(static_cast<double>(tick) / 4.0, static_cast<double>((tick / 28) * 7), true);
    }
    QCOMPARE(tuner.targetSeconds(5.0), 5.0); // Consumer backpressure, even with periodic packet jumps.
}

void AppModelTests::liveBufferTunerBoundsAndRelaxesCapacity()
{
    using OKILTV::Player::LiveBufferTuner;
    LiveBufferTuner tuner;
    for (int tick = 0; tick <= 180; ++tick) {
        tuner.observe(static_cast<double>(tick) / 4.0, static_cast<double>((tick / 60) * 15), false);
    }
    QCOMPARE(tuner.targetSeconds(5.0), 18.0);
    for (int tick = 181; tick < 440; ++tick) {
        tuner.observe(static_cast<double>(tick) / 4.0, static_cast<double>(tick) / 4.0, false);
    }
    QCOMPARE(tuner.targetSeconds(5.0), 17.5);
    tuner.reset();
    QCOMPARE(tuner.targetSeconds(5.0), 5.0);
}

void AppModelTests::liveReservePreservesManualPauseAndStopsCleanly()
{
    PlayerController controller;
    controller.m_liveDeliveryTimer.stop();
    controller.m_positionTimer.stop();
    Channel channel;
    channel.id = 482;
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live-reserve-test.ts");
    controller.playChannel(channel);
    QVERIFY(controller.beginLiveReserve(0.0, false));
    controller.refreshBufferingState();
    QVERIFY(controller.isBuffering());
    controller.togglePause();
    QVERIFY(controller.m_userPausedManually);
    QVERIFY(!controller.m_buffering.liveReservePending());
    QVERIFY(!controller.evaluateLiveReserve());
    QVERIFY(controller.m_userPausedManually);
    controller.stop();
    QVERIFY(!controller.m_buffering.liveReservePending());
    QCOMPARE(controller.m_buffering.liveReserveTarget(), 0.0);
    QVERIFY(!controller.m_buffering.m_liveRetryMs.has_value());
    QCOMPARE(controller.effectiveLiveBufferTargetSeconds(), controller.player()->bufferTargetSeconds());
}

void AppModelTests::liveReservePausesOnlyAtDepletionRegardlessOfAdaptation()
{
    using State = OKILTV::Player::MpvPlayer::CacheReadState;
    PlayerController controller;
    controller.m_liveDeliveryTimer.stop();
    controller.m_positionTimer.stop();
    // Match the roughly seven-second provider cadence seen in session logs.
    for (int tick = 0; tick <= 84; ++tick) {
        controller.m_buffering.observeDelivery(static_cast<double>(tick) / 4.0,
                                             static_cast<double>((tick / 28) * 7), false);
    }
    QCOMPARE(controller.effectiveLiveBufferTargetSeconds(), 8.5);
    const auto wasPlaying = controller.isPlaying();
    for (const auto cache : { 7.0, 3.0, 1.34, 1.0, 0.5, 0.11 }) {
        QVERIFY(!controller.beginLiveReserve(cache, false));
        QVERIFY(!controller.isBuffering());
    }
    QVERIFY(!controller.beginLiveReserve(0.0, true)); // Consumer backpressure.
    QVERIFY(controller.beginLiveReserve(0.1, false));
    QCOMPARE(controller.isPlaying(), wasPlaying); // Only backend signals own this.
    QCOMPARE(controller.m_buffering.liveReserveTarget(), 8.5);
    for (const auto cache : { 0.1, 0.25, 3.0, 7.0, 8.49 }) {
        QVERIFY(controller.advanceLiveReserve(cache, State { 28.0, false, false }, 2000));
        QVERIFY(controller.isBuffering());
    }
    QVERIFY(!controller.advanceLiveReserve(8.5, State { 35.0, false, false }, 9000));
    QVERIFY(!controller.isBuffering());
    QVERIFY(!controller.m_buffering.m_liveRetryMs.has_value());
    QVERIFY(controller.beginLiveReserve(0.0, false));
    QVERIFY(controller.advanceLiveReserve(7.0, State { 42.0, false, false }, 2000));
    QVERIFY(!controller.advanceLiveReserve(8.5, State { 49.0, false, false }, 7000));
    QCOMPARE(controller.effectiveLiveBufferTargetSeconds(), 8.5);
    controller.player()->configurePlaybackTuning(5.0, true, 30.0);
    QCOMPARE(controller.effectiveLiveBufferTargetSeconds(), 30.0);
    QVERIFY(!controller.beginLiveReserve(7.0, false));
    QVERIFY(!controller.beginLiveReserve(1.34, false));
    QVERIFY(controller.beginLiveReserve(0.0, false));
    QCOMPARE(controller.m_buffering.liveReserveTarget(), 30.0);
    QVERIFY(controller.advanceLiveReserve(7.0, State { 56.0, false, false }, 2000));
    QVERIFY(controller.advanceLiveReserve(29.9, State { 79.0, false, false }, 11000));
    QVERIFY(!controller.advanceLiveReserve(30.0, State { 79.1, false, false }, 12000));
    controller.stop();
}

void AppModelTests::liveReserveHandlesMissingTelemetryEofAndOutage()
{
    using State = OKILTV::Player::MpvPlayer::CacheReadState;
    PlayerController controller;
    controller.m_liveDeliveryTimer.stop();
    controller.m_positionTimer.stop();
    QVERIFY(!controller.beginLiveReserve(std::nullopt, false));
    QVERIFY(!controller.beginLiveReserve(std::numeric_limits<double>::quiet_NaN(), false));
    QVERIFY(!controller.beginLiveReserve(-1.0, false));
    QVERIFY(controller.beginLiveReserve(0.05, false)); // Protect even before cadence is learned.
    QCOMPARE(controller.m_buffering.liveReserveTarget(), 3.0);
    QVERIFY(controller.advanceLiveReserve(0.0, State { 1.0, false, false }, 9000));
    QVERIFY(!controller.advanceLiveReserve(std::nullopt, std::nullopt, 10000));
    QVERIFY(!controller.beginLiveReserve(0.0, false)); // Allow the reconnect watchdog to work.
    controller.resetLiveReserve(false);
    QVERIFY(controller.beginLiveReserve(0.05, false));
    QVERIFY(!controller.advanceLiveReserve(2.0, State { 2.0, true, false }, 1000));
    QVERIFY(!controller.beginLiveReserve(0.05, false)); // Capacity-limited release imposes a retry cooldown.
    controller.resetLiveReserve(false);
    QVERIFY(controller.beginLiveReserve(0.05, false));
    QVERIFY(!controller.advanceLiveReserve(0.05, State { 2.0, false, true }, 250)); // Release EOF tail.
    controller.stop();
    QVERIFY(!controller.m_buffering.m_liveRetryMs.has_value());
}

void AppModelTests::liveReservePreservesDeliveryLearning()
{
    using State = OKILTV::Player::MpvPlayer::CacheReadState;
    PlayerController controller;
    controller.m_liveDeliveryTimer.stop();
    controller.m_positionTimer.stop();
    for (int tick = 0; tick <= 84; ++tick) {
        const auto now = static_cast<double>(tick) / 4.0;
        controller.m_buffering.observeDelivery(now, static_cast<double>((tick / 28) * 7), false);
        if (tick == 24 || tick == 52) {
            QVERIFY(controller.beginLiveReserve(0.05, false));
        }
        if (tick == 28 || tick == 56) {
            QVERIFY(!controller.advanceLiveReserve(7.9, State { now, false, false }, 1000));
        }
    }
    // Repeated app-owned holds must not reset the three-burst learning window.
    QCOMPARE(controller.m_buffering.detectedIntervalSeconds(), 7.0);
    QCOMPARE(controller.effectiveLiveBufferTargetSeconds(), 8.5);
    controller.stop();
}

void AppModelTests::mpvPicturePresetsPreserveUserShaders()
{
    OKILTV::Player::MpvPlayer mpv;
    mpv.configureOptions({ { QStringLiteral("vo"), QStringLiteral("null") },
                           { QStringLiteral("ao"), QStringLiteral("null") },
                           { QStringLiteral("glsl-shaders"), QStringLiteral("user.glsl") } });
    mpv.configurePicturePreset(QStringLiteral("warm"));
    if (!mpv.ensureInitialized()) {
        QSKIP("libmpv unavailable");
    }
    for (const auto &preset : { "warm", "cold", "movie", "vivid", "sport" }) {
        mpv.configurePicturePreset(QString::fromLatin1(preset));
        const auto shaders = mpv.propertyString("glsl-shaders").value_or(QString());
        QVERIFY(shaders.contains(QStringLiteral("user.glsl")));
        QVERIFY(shaders.contains(QString::fromLatin1(preset) + QStringLiteral(".glsl")));
        QCOMPARE(shaders.count(QStringLiteral(".glsl")), 2);
        QVERIFY(QFile::exists(mpv.m_appliedPictureShader));
        mpv.configurePicturePreset(QString::fromLatin1(preset));
        QCOMPARE(mpv.propertyString("glsl-shaders").value_or(QString()), shaders);
        QVERIFY(!mpv.m_reinitializePending);
    }
    mpv.configurePicturePreset(QStringLiteral("standard"));
    QCOMPARE(mpv.propertyString("glsl-shaders").value_or(QString()), QStringLiteral("user.glsl"));
    mpv.configurePicturePreset(QStringLiteral("invalid"));
    QCOMPARE(mpv.propertyString("glsl-shaders").value_or(QString()), QStringLiteral("user.glsl"));
}

void AppModelTests::mpvImageSmoothingAppliesWithoutRetune()
{
    OKILTV::Player::MpvPlayer mpv;
    mpv.configureOptions({ { QStringLiteral("vo"), QStringLiteral("null") },
                           { QStringLiteral("ao"), QStringLiteral("null") },
                           { QStringLiteral("sharpen"), QStringLiteral("0.25") } });
    mpv.configureImageSmoothing(true);
    if (!mpv.ensureInitialized()) {
        QSKIP("libmpv unavailable");
    }
    QCOMPARE(mpv.propertyDouble("sharpen"), std::optional<double>(-0.5));
    mpv.configureImageSmoothing(false);
    QCOMPARE(mpv.propertyDouble("sharpen"), std::optional<double>(0.25));
    mpv.configureImageSmoothing(true);
    QCOMPARE(mpv.propertyDouble("sharpen"), std::optional<double>(-0.5));
    QVERIFY(!mpv.m_reinitializePending);

    OKILTV::Player::MpvPlayer defaultPlayer;
    defaultPlayer.configureOptions({ { QStringLiteral("vo"), QStringLiteral("null") },
                                     { QStringLiteral("ao"), QStringLiteral("null") } });
    QVERIFY(defaultPlayer.ensureInitialized());
    QCOMPARE(defaultPlayer.propertyDouble("sharpen"), std::optional<double>(0.0));
    defaultPlayer.configureImageSmoothing(true);
    QCOMPARE(defaultPlayer.propertyDouble("sharpen"), std::optional<double>(-0.5));
    defaultPlayer.configureImageSmoothing(false);
    QCOMPARE(defaultPlayer.propertyDouble("sharpen"), std::optional<double>(0.0));
    QVERIFY(!defaultPlayer.m_reinitializePending);
}

void AppModelTests::mpvLiveRefillPolicyResetsOnRetune()
{
    using OKILTV::Player::MpvPlayer;
    MpvPlayer mpv;
    mpv.configureOptions({ { QStringLiteral("vo"), QStringLiteral("null") },
                           { QStringLiteral("ao"), QStringLiteral("null") } });
    mpv.configurePlaybackTuning(5.0, false, 5.0);
    if (!mpv.ensureInitialized()) {
        QSKIP("libmpv unavailable");
    }
    mpv.setStartupBufferingStrictMode(false);
    MpvPlayer::SteadyStateBufferingPolicy policy { 8.0, 7.0, 64 * 1024 * 1024, 32 * 1024 * 1024, 2.0 };
    QVERIFY(mpv.setSteadyStateBufferingPolicy(policy));
    QCOMPARE(mpv.propertyFlag("cache-pause"), std::optional<bool>(true));
    QCOMPARE(mpv.propertyDouble("cache-pause-wait"), std::optional<double>(2.0));
    // Identical capacity must still allow disabling the adaptive refill policy.
    policy.refillSeconds = 0.0;
    QVERIFY(mpv.setSteadyStateBufferingPolicy(policy));
    QCOMPARE(mpv.propertyFlag("cache-pause"), std::optional<bool>(false));
    policy.refillSeconds = 2.0;
    QVERIFY(mpv.setSteadyStateBufferingPolicy(policy));
    mpv.setStartupBufferingStrictMode(false); // New FastLive tune, strict flag already false.
    QCOMPARE(mpv.propertyFlag("cache-pause"), std::optional<bool>(false));
    QCOMPARE(mpv.propertyDouble("cache-pause-wait"), std::optional<double>(0.0));
    mpv.setStartupBufferingStrictMode(true);
    QCOMPARE(mpv.propertyDouble("cache-pause-wait"), std::optional<double>(5.0));
}

void AppModelTests::mpvLiveContinuouslyRefillsBeyondReserve()
{
    const auto previousHeadless = qgetenv("OKILTV_HEADLESS_TEST");
    const auto restoreEnvironment = qScopeGuard([&]() {
        if (previousHeadless.isNull()) {
            qunsetenv("OKILTV_HEADLESS_TEST");
        } else {
            qputenv("OKILTV_HEADLESS_TEST", previousHeadless);
        }
    });
    qputenv("OKILTV_HEADLESS_TEST", "0");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile media(directory.filePath(QStringLiteral("buffer.wav")));
    QVERIFY(media.open(QIODevice::WriteOnly));
    constexpr quint32 sampleRate = 48000;
    constexpr quint32 dataBytes = sampleRate * 2 * 60;
    QDataStream wave(&media);
    wave.setByteOrder(QDataStream::LittleEndian);
    wave.writeRawData("RIFF", 4);
    wave << quint32(36 + dataBytes);
    wave.writeRawData("WAVEfmt ", 8);
    wave << quint32(16) << quint16(1) << quint16(1) << sampleRate;
    wave << quint32(sampleRate * 2) << quint16(2) << quint16(16);
    wave.writeRawData("data", 4);
    wave << dataBytes;
    QCOMPARE(media.write(QByteArray(dataBytes, '\0')), qint64(dataBytes));
    media.close();

    OKILTV::Player::MpvPlayer player;
    player.configureOptions({ { QStringLiteral("vo"), QStringLiteral("null") },
                              { QStringLiteral("ao"), QStringLiteral("null") } });
    player.configurePlaybackTuning(5.0, false, 3.0);
    player.setStartupBufferingStrictMode(false);
    if (!player.ensureInitialized()) {
        QSKIP("libmpv unavailable");
    }
    QCOMPARE(player.propertyDouble("cache-secs"), std::optional<double>(11.0));
    QCOMPARE(player.propertyDouble("demuxer-hysteresis-secs"), std::optional<double>(0.0));
    // Exercise the runtime policy too: zero must survive normalization and
    // catch-up's positive hysteresis must not leak into a later Live tune.
    QVERIFY(player.setSteadyStateBufferingPolicy({ 90.0, 85.0, 96LL << 20, 32LL << 20, std::nullopt }));
    player.resetSteadyStateBuffering();
    QCOMPARE(player.propertyDouble("cache-secs"), std::optional<double>(11.0));
    QCOMPARE(player.propertyDouble("demuxer-hysteresis-secs"), std::optional<double>(0.0));
    player.play(media.fileName());
    const auto cacheFull = [&]() {
        player.refreshCachedTelemetryFast();
        const auto state = player.cacheReadState();
        return state.has_value() && state->idle
            && player.demuxerCacheDurationSeconds().value_or(0.0) > 10.0;
    };
    QTRY_VERIFY_WITH_TIMEOUT(cacheFull(), 5000);
    const auto initialEnd = player.propertyDouble("demuxer-cache-time");
    QVERIFY(initialEnd.has_value());
    // Cached media advances while the reserve is still well above 3 seconds;
    // waiting for the old 2-second refill floor would fail this deadline.
    QTRY_VERIFY_WITH_TIMEOUT(player.propertyDouble("demuxer-cache-time").value_or(0.0) > *initialEnd + 1.0, 4000);
    QVERIFY(player.propertyDouble("demuxer-cache-duration").value_or(0.0) > 8.0);
    QVERIFY(player.propertyDouble("demuxer-cache-duration").value_or(60.0) < 14.0);
    player.stop();
}

void AppModelTests::playerControllerLiveTuneUsesFastStartupPolicy()
{
    PlayerController playerController;
    QStringList playerLogs;
    const auto subscriptionId = DebugLogger::instance().subscribe([&playerLogs](const DebugLogger::Entry &entry) {
        if (entry.category == QStringLiteral("player")) {
            playerLogs.push_back(entry.message);
        }
    });

    Channel channel;
    channel.id = 71;
    channel.name = QStringLiteral("Fast Live Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/fast-live");

    playerController.playChannel(channel);

    QVERIFY(!playerController.m_buffering.startupPending());
    QVERIFY(!playerController.m_buffering.startupProbeTimer().isActive());
    QVERIFY(!playerController.m_buffering.startupFallbackTimer().isActive());
    QVERIFY(!playerController.player()->m_startupBufferingStrictMode);
    QVERIFY(playerLogs.contains(QStringLiteral("Startup policy selected for tune: fast-live.")));
    QVERIFY(playerLogs.contains(
        QStringLiteral(
            "Channel switch requested on primary live player; skipping explicit stop and relying on loadfile replace.")));

    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::playerControllerShowsLoadingIndicatorAfterTuneDelay_data()
{
    QTest::addColumn<QString>("mode");
    QTest::newRow("live") << QStringLiteral("live");
    QTest::newRow("catchup") << QStringLiteral("catchup");
}

void AppModelTests::playerControllerShowsLoadingIndicatorAfterTuneDelay()
{
    QFETCH(QString, mode);
    PlayerController controller;
    Channel channel;
    channel.id = 111;
    channel.name = QStringLiteral("Channel One Eleven");
    channel.profileId = QUuid::createUuid();
    controller.m_currentChannel = channel;
    controller.m_playbackMode = mode;
    controller.setChannelSwitchInProgress(true);
    // Keep backend I/O out of the state-machine test; deliver lifecycle signals explicitly.
    controller.startPlaybackRequest(controller.player(), QString(), false);
    QVERIFY(controller.m_loadingIndicatorPending);
    QVERIFY(controller.m_loadingIndicatorDelayTimer.isActive());
    QCOMPARE(controller.m_loadingIndicatorDelayTimer.interval(), 1500);
    QVERIFY(!controller.isLoading());

    emit controller.player()->pauseStateChanged(false);
    QVERIFY(controller.isPlaying());
    QVERIFY(controller.m_loadingIndicatorPending);
    QVERIFY(controller.channelSwitchInProgress());
    controller.m_loadingIndicatorDelayTimer.stop();
    QVERIFY(QMetaObject::invokeMethod(&controller.m_loadingIndicatorDelayTimer, "timeout"));
    QVERIFY(controller.isLoading());

    // A stale restart before the new file is loaded must not dismiss loading.
    emit controller.player()->playbackRestarted();
    QVERIFY(controller.isLoading());
    controller.m_pauseToggleRequested = true;
    emit controller.player()->pauseStateChanged(true);
    emit controller.player()->bufferingStateChanged(true);
    QVERIFY(!controller.isLoading());
    QVERIFY(!controller.isBuffering());
    emit controller.player()->pauseStateChanged(true); // Repeated backend observation.
    QVERIFY(!controller.isLoading());
    QVERIFY(!controller.isBuffering());
    controller.m_pauseToggleRequested = true;
    emit controller.player()->pauseStateChanged(false);
    QVERIFY(controller.isLoading());
    emit controller.player()->bufferingStateChanged(false);

    emit controller.player()->fileLoaded();
    QVERIFY(controller.isLoading());
    emit controller.m_catchupStandbyPlayer.playbackRestarted();
    QVERIFY(controller.isLoading());
    emit controller.player()->playbackRestarted();
    QVERIFY(!controller.isLoading());
    QVERIFY(!controller.m_loadingIndicatorPending);
    QVERIFY(!controller.channelSwitchInProgress());

    controller.m_pauseToggleRequested = true;
    emit controller.player()->pauseStateChanged(true);
    emit controller.player()->bufferingStateChanged(true);
    QVERIFY(!controller.isLoading());
    QVERIFY(!controller.isBuffering());
    // Strict startup can render its first frame while still building reserve.
    controller.m_userPausedManually = false;
    controller.beginDeferredLoadingIndicator();
    controller.m_loadingIndicatorDelayTimer.stop();
    controller.m_loadingPlaybackFileLoaded = true;
    controller.m_buffering.setStartupPending(true);
    emit controller.player()->playbackRestarted();
    QVERIFY(controller.isLoading());
    controller.m_buffering.setStartupPending(false);
    controller.refreshBufferingState();
    QVERIFY(!controller.isLoading());
    QVERIFY(!controller.m_loadingIndicatorPending);

    controller.beginDeferredLoadingIndicator();
    controller.stop();
    QVERIFY(!controller.isLoading());
    QVERIFY(!controller.m_loadingIndicatorPending);
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
    QVERIFY(!playerController.m_buffering.startupPending());

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "errorOccurred",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("simulated-startup-error"))));

    QVERIFY(playerController.m_recovery.active());
    QVERIFY(playerController.m_recovery.attemptInFlight() || playerController.m_recovery.waitingStop());
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
    QVERIFY(!playerController.m_buffering.startupPending());

    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "playbackEnded", Qt::DirectConnection));

    QVERIFY(playerController.m_recovery.active());
    QVERIFY(playerController.m_recovery.attemptInFlight() || playerController.m_recovery.waitingStop());
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
    QVERIFY(playerController.m_recovery.active());

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

    QVERIFY(playerController.m_recovery.active());
    QCOMPARE(playbackErrorSpy.count(), 0);

    // Reconnect now includes a stop-settle window per attempt; keep timeout lenient for CI jitter.
    QTRY_VERIFY_WITH_TIMEOUT(playerController.channelLoadFailed(), 15000);
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
    const auto unsubscribe = qScopeGuard([subscriptionId]() { DebugLogger::instance().unsubscribe(subscriptionId); });

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
    // The last request is the return-to-live tune; provider paths are masked.
    auto livePlayLog = std::find_if(playRequestLogs.crbegin(), playRequestLogs.crend(), [](const QString &line) {
        return line.startsWith(QStringLiteral("Play requested:"));
    });
    QVERIFY(livePlayLog != playRequestLogs.crend());
    QVERIFY(!livePlayLog->contains(QStringLiteral("cache-secs=120")));
    QVERIFY(!livePlayLog->contains(QStringLiteral("demuxer-hysteresis-secs=115")));
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
    const auto unsubscribe = qScopeGuard([subscriptionId]() { DebugLogger::instance().unsubscribe(subscriptionId); });

    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup119"),
        QStringLiteral("Past Show"));
    QVERIFY(!playerController.m_buffering.startupPending());

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "errorOccurred",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("simulated-initial-archive-error"))));

    QVERIFY(playerController.m_recovery.active());
    QVERIFY(!playerController.channelLoadFailed());
    QTRY_VERIFY_WITH_TIMEOUT(!reconnectStartLogs.isEmpty(), 1000);
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("http://127.0.0.1/***")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("force-seekable=yes")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("cache-secs=90")));
    QCOMPARE(playerController.playbackMode(), QStringLiteral("catchup"));
    QCOMPARE(playerController.currentPlaybackUrl(), QStringLiteral("http://127.0.0.1/catchup119"));

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

    QVERIFY(!playerController.m_buffering.startupPending());
    QVERIFY(!playerController.m_buffering.startupFallbackTimer().isActive());
    QVERIFY(!playerController.m_buffering.startupProbeTimer().isActive());

    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(!playerController.m_buffering.startupPending());
    QVERIFY(!playerController.m_buffering.startupFallbackTimer().isActive());
    QVERIFY(!playerController.m_buffering.startupProbeTimer().isActive());
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

    const auto archivedStart = QDateTime::currentDateTimeUtc().addSecs(-600);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup72"),
        QStringLiteral("Past Show"),
        archivedStart,
        archivedStart.addSecs(60));
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
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("http://127.0.0.1/***")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("force-seekable=yes")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("hr-seek=no")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("cache=yes")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("cache-secs=90")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("demuxer-readahead-secs=90")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("demuxer-hysteresis-secs=85")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("demuxer-seekable-cache=yes")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("demuxer-max-bytes=100663296")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("demuxer-max-back-bytes=33554432")));
    QVERIFY(reconnectStartLogs.constLast().contains(QStringLiteral("length=120")));
    if (!playRequestLogs.isEmpty()) {
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("force-seekable=yes")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("hr-seek=no")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("cache=yes")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("cache-secs=90")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("demuxer-readahead-secs=90")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("demuxer-hysteresis-secs=85")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("demuxer-seekable-cache=yes")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("demuxer-max-bytes=100663296")));
        QVERIFY(playRequestLogs.constFirst().contains(QStringLiteral("demuxer-max-back-bytes=33554432")));
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

void AppModelTests::playerControllerCatchupReconnectRestoresStreamPositionAfterFileLoaded()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 272;
    channel.name = QStringLiteral("Archive Resume Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live272");

    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup272"),
        QStringLiteral("Past Show"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    playerController.m_player.m_cachedTelemetry.positionSeconds = 52.0;

    QStringList resumeLogs;
    const auto subscriptionId = DebugLogger::instance().subscribe([&resumeLogs](const DebugLogger::Entry &entry) {
        if (entry.category == QStringLiteral("player")
            && entry.message.contains(QStringLiteral("restoring stream-relative position"))) {
            resumeLogs.push_back(entry.message);
        }
    });

    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "errorOccurred",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("simulated-archive-error"))));
    QVERIFY(playerController.m_recovery.active());
    QVERIFY(playerController.m_recovery.attemptInFlight() || playerController.m_recovery.waitingStop());
    if (!playerController.m_recovery.attemptInFlight()) {
        QTest::qWait(500);
        playerController.handleReconnectAttemptTick();
    }
    QVERIFY(playerController.m_recovery.attemptInFlight());
    QVERIFY(playerController.m_catchupSession.m_catchupReconnectResumeStreamRelativeSeconds.has_value());
    QCOMPARE(static_cast<int>(std::lround(playerController.m_catchupSession.m_catchupReconnectResumeStreamRelativeSeconds.value())), 52);

    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(!playerController.m_catchupSession.m_catchupReconnectResumeStreamRelativeSeconds.has_value());
    QTRY_VERIFY_WITH_TIMEOUT(!resumeLogs.isEmpty(), 1000);
    QVERIFY(resumeLogs.constLast().contains(QStringLiteral("52.000s")));

    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::playerControllerCatchupRollbackGuardCorrectsLargeWarmupRegression()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 273;
    channel.name = QStringLiteral("Archive Guard Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live273");

    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup273"),
        QStringLiteral("Past Show"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    QStringList guardLogs;
    const auto subscriptionId = DebugLogger::instance().subscribe([&guardLogs](const DebugLogger::Entry &entry) {
        if (entry.category == QStringLiteral("player")
            && entry.message.contains(QStringLiteral("rollback guard corrected backward jump"))) {
            guardLogs.push_back(entry.message);
        }
    });

    // Non-initial URL load context keeps immediate rollback correction behavior.
    playerController.m_catchupSession.m_catchupRollbackInitialLoadContext = false;
    playerController.m_player.m_cachedTelemetry.positionSeconds = 80.0;
    playerController.updatePosition();
    playerController.m_player.m_cachedTelemetry.positionSeconds = 40.0;
    playerController.updatePosition();

    QVERIFY(playerController.m_catchupSession.m_catchupRollbackGuardConsumed);
    QTRY_VERIFY_WITH_TIMEOUT(!guardLogs.isEmpty(), 1000);
    QVERIFY(guardLogs.constLast().contains(QStringLiteral("rollback=40.000s")));

    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::playerControllerCatchupRollbackGuardDefersCorrectionUntilSeekableOnInitialLoad()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 274;
    channel.name = QStringLiteral("Archive Guard Defer Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live274");

    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup274"),
        QStringLiteral("Past Show"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    QStringList deferredLogs;
    QStringList appliedLogs;
    const auto subscriptionId = DebugLogger::instance().subscribe([&deferredLogs, &appliedLogs](const DebugLogger::Entry &entry) {
        if (entry.category != QStringLiteral("player")) {
            return;
        }
        if (entry.message.contains(QStringLiteral("rollback guard deferred correction armed"))) {
            deferredLogs.push_back(entry.message);
        } else if (entry.message.contains(QStringLiteral("rollback guard deferred correction applied"))) {
            appliedLogs.push_back(entry.message);
        }
    });

    playerController.m_player.m_cachedTelemetry.positionSeconds = 80.0;
    playerController.updatePosition();
    playerController.m_player.m_cachedTelemetry.positionSeconds = 40.0;
    playerController.updatePosition();
    QVERIFY(playerController.m_catchupSession.m_catchupRollbackDeferredPending);
    QTRY_VERIFY_WITH_TIMEOUT(!deferredLogs.isEmpty(), 1000);

    playerController.m_player.m_cachedTelemetry.demuxerSeekableRangeSeconds = std::pair<double, double> { 0.0, 120.0 };
    playerController.m_player.m_cachedTelemetry.positionSeconds = 41.0;
    playerController.updatePosition();

    QVERIFY(!playerController.m_catchupSession.m_catchupRollbackDeferredPending);
    QTRY_VERIFY_WITH_TIMEOUT(!appliedLogs.isEmpty(), 1000);

    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::playerControllerCatchupRollbackGuardDeferredCorrectionExpiresOnInitialLoad()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 275;
    channel.name = QStringLiteral("Archive Guard Defer Expire Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live275");

    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup275"),
        QStringLiteral("Past Show"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    QStringList expiredLogs;
    const auto subscriptionId = DebugLogger::instance().subscribe([&expiredLogs](const DebugLogger::Entry &entry) {
        if (entry.category == QStringLiteral("player")
            && entry.message.contains(QStringLiteral("rollback guard deferred correction expired"))) {
            expiredLogs.push_back(entry.message);
        }
    });

    playerController.m_player.m_cachedTelemetry.positionSeconds = 90.0;
    playerController.updatePosition();
    playerController.m_player.m_cachedTelemetry.positionSeconds = 40.0;
    playerController.updatePosition();
    QVERIFY(playerController.m_catchupSession.m_catchupRollbackDeferredPending);

    playerController.m_catchupSession.m_deferredStartedMs.reset();
    playerController.m_player.m_cachedTelemetry.positionSeconds = 41.0;
    playerController.updatePosition();

    QVERIFY(!playerController.m_catchupSession.m_catchupRollbackDeferredPending);
    QTRY_VERIFY_WITH_TIMEOUT(!expiredLogs.isEmpty(), 1000);

    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::catchupOwnedStreamSessionClosesProviderWhileBufferedBytesRemainReadable()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    const QByteArray payload(64 * 1024, 's');
    bool requestSeen = false;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        auto *socket = server.nextPendingConnection();
        socket->setParent(&server);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
            socket->readAll();
            if (requestSeen) {
                return;
            }
            requestSeen = true;
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\n\r\n");
            socket->write(payload);
            socket->flush();
        });
    });

    const auto session = OKILTV::Player::CatchupStreamSession::create(
        QStringLiteral("http://127.0.0.1:%1/catchup.ts").arg(server.serverPort()));
    QStringList closeLogs;
    const auto subscriptionId = DebugLogger::instance().subscribe([&closeLogs](const DebugLogger::Entry &entry) {
        if (entry.category == QStringLiteral("player")
            && entry.message.startsWith(QStringLiteral("Catch-up owned stream provider closed:"))) {
            closeLogs.push_back(entry.message);
        }
    });
    QVERIFY(session->start());
    QTRY_VERIFY_WITH_TIMEOUT(requestSeen, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(session->bufferedBytes() >= payload.size(), 1000);

    session->closeProviderConnection(QStringLiteral("test-close-provider"));
    QVERIFY(session->providerConnectionClosed());

    QByteArray actual(payload.size(), Qt::Uninitialized);
    const auto readBytes = session->read(actual.data(), static_cast<quint64>(actual.size()));
    QCOMPARE(readBytes, static_cast<qint64>(payload.size()));
    QCOMPARE(actual, payload);
    char eofByte = '\0';
    QCOMPARE(session->read(&eofByte, 1), qint64(0));
    QTRY_VERIFY_WITH_TIMEOUT(!closeLogs.isEmpty(), 1000);
    QVERIFY(closeLogs.constLast().contains(QStringLiteral("appClose=yes")));
    QVERIFY(closeLogs.constLast().contains(QStringLiteral("appReason=test-close-provider")));
    QVERIFY(closeLogs.constLast().contains(QStringLiteral("abortExpected=yes")));
    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::catchupOwnedStreamSessionPreservesConfiguredRequestHeaders()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QByteArray requestBytes;
    bool requestSeen = false;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        auto *socket = server.nextPendingConnection();
        socket->setParent(&server);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
            requestBytes += socket->readAll();
            if (requestSeen || !requestBytes.contains("\r\n\r\n")) {
                return;
            }
            requestSeen = true;
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->flush();
            socket->disconnectFromHost();
        });
    });

    OKILTV::Player::CatchupStreamSession::HeaderList headers;
    headers.append(qMakePair(QByteArrayLiteral("User-Agent"), QByteArray("OKILTV-Agent/9.9")));
    headers.append(qMakePair(QByteArrayLiteral("Authorization"), QByteArray("Bearer branch-test-token")));
    const auto session = OKILTV::Player::CatchupStreamSession::create(
        QStringLiteral("http://127.0.0.1:%1/catchup.ts").arg(server.serverPort()),
        headers);

    QVERIFY(session->start());
    QTRY_VERIFY_WITH_TIMEOUT(requestSeen, 1000);
    QVERIFY(requestBytes.contains("User-Agent: OKILTV-Agent/9.9\r\n"));
    QVERIFY(requestBytes.contains("Authorization: Bearer branch-test-token\r\n"));
}

void AppModelTests::ownedStreamTimesOutWhenProviderStopsSending()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        auto *socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, socket, [socket]() { socket->readAll(); });
    });
    const auto session = OKILTV::Player::CatchupStreamSession::create(
        QStringLiteral("http://127.0.0.1:%1/stalled.ts").arg(server.serverPort()), {},
        { 1024, 512, 1024, QStringLiteral("test-timeout"), false, 100 });
    QVERIFY(session->start());
    QTRY_VERIFY_WITH_TIMEOUT(session->hasNetworkError(), 1500);
    QTRY_VERIFY_WITH_TIMEOUT(session->providerConnectionClosed(), 1000);
    QVERIFY(session->errorString().contains(QStringLiteral("Timed out")));
}

void AppModelTests::catchupOwnedStreamSessionCloseWithBackpressure_data()
{
    QTest::addColumn<bool>("abortTransfer");
    QTest::newRow("abort-running-reply") << true;
    QTest::newRow("close-finished-reply") << false;
}

void AppModelTests::catchupOwnedStreamSessionCloseWithBackpressure()
{
    QFETCH(bool, abortTransfer);
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const QByteArray payload(64 * 1024, 'b');
    QByteArray request;
    bool sent = false;
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        auto *socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
            request += socket->readAll();
            if (sent || !request.contains("\r\n\r\n")) {
                return;
            }
            sent = true;
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: "
                + QByteArray::number(payload.size() + (abortTransfer ? 1 : 0)) + "\r\n\r\n");
            socket->write(payload);
            socket->flush();
        });
    });
    const auto session = OKILTV::Player::CatchupStreamSession::create(
        QStringLiteral("http://127.0.0.1:%1/backpressure.ts").arg(server.serverPort()),
        {},
        { 1024, 512, 128 * 1024, QStringLiteral("test"), false, 100 });
    QVERIFY(session->start());
    QTRY_COMPARE_WITH_TIMEOUT(session->bufferedBytes(), qsizetype(1024), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(session->m_reply && session->m_reply->bytesAvailable() > 0, 2000);
    QTest::qWait(250);
    QVERIFY(!session->hasNetworkError()); // Intentional backpressure must not time out.
    if (!abortTransfer) {
        QTRY_VERIFY_WITH_TIMEOUT(session->providerConnectionClosed(), 2000);
    }
    session->closeProviderConnection(QStringLiteral("test-backpressure"));
    QVERIFY(session->providerConnectionClosed());
    QVERIFY(!session->hasNetworkError());

    auto reader = QtConcurrent::run([session]() {
        QByteArray result;
        char buffer[512];
        for (;;) {
            const auto count = session->read(buffer, sizeof(buffer));
            if (count <= 0) {
                return qMakePair(result, count);
            }
            result.append(buffer, static_cast<qsizetype>(count));
        }
    });
    const auto cancelReader = qScopeGuard([&]() {
        session->cancelRead();
        reader.waitForFinished();
    });
    QTRY_VERIFY_WITH_TIMEOUT(reader.isFinished(), 3000);
    const auto result = reader.result();
    QCOMPARE(result.second, qint64(0));
    QCOMPARE(result.first, abortTransfer ? payload.first(1024) : payload);
    QVERIFY(session->peakBufferedBytes() <= 1024);
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
    QCOMPARE(snapshot.value(QStringLiteral("bufferDurationText")).toString(), QStringLiteral("90.00 s"));
    QCOMPARE(snapshot.value(QStringLiteral("bufferDurationSeconds")).toDouble(), 90.0);
    QCOMPARE(snapshot.value(QStringLiteral("mpvBufferDurationSeconds")).toDouble(), 4096.0);
}

void AppModelTests::playerControllerCatchupReconnectStabilizationIgnoresCacheDurationOutliers()
{
    PlayerController playerController;
    playerController.m_waitForDataStreamSeconds = 10.0;
    playerController.m_positionTimer.stop();
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
    QVERIFY(playerController.m_recovery.active());
    QVERIFY(playerController.m_recovery.attemptInFlight() || playerController.m_recovery.waitingStop());
    if (!playerController.m_recovery.attemptInFlight()) {
        QTest::qWait(500);
        playerController.handleReconnectAttemptTick();
    }
    QVERIFY(playerController.m_recovery.attemptInFlight());

    playerController.m_isPlaying = true;
    playerController.m_backendBuffering = false;
    playerController.m_recovery.m_playbackStalled = false;
    playerController.m_recovery.m_lastPlaybackPositionSeconds = -1.0;

    // Exercise real elapsed time: twelve rapid calls hid the ten-second timeout.
    for (int i = 0; i < 12; ++i) {
        QTest::qWait(1000);
        QVERIFY(playerController.m_recovery.attemptInFlight());
        QCOMPARE(playerController.m_recovery.attempts(), 1);
        playerController.m_player.m_cachedTelemetry.positionSeconds = static_cast<double>(i + 1);
        playerController.m_player.m_cachedTelemetry.displayedVideoFramePtsSeconds = static_cast<double>(i + 1);
        playerController.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = 4096.0;
        playerController.updatePosition();
    }

    QVERIFY(!playerController.m_recovery.active());
    QVERIFY(!playerController.m_recovery.attemptInFlight());
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
    QVERIFY(playerController.m_catchupSession.reloadInFlight());
    playerController.runCatchupTimelineReload();
    playerController.finishCatchupTimelineReload(QStringLiteral("test-stop-ack"));
    QCOMPARE(
        playerController.currentPlaybackUrl(),
        QStringLiteral("http://provider.example/timeshift/user/pass/59/2026-05-18:12-02/73.ts"));
    QCOMPARE(playerController.playbackMode(), QStringLiteral("catchup"));
    QCOMPARE(playerController.catchupProgramLabel(), QStringLiteral("12:00 - 13:00  Archive Show"));
    QCOMPARE(playerController.catchupTimelinePositionSeconds(), 158.0);

    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::playerControllerXtreamCatchupSeekWaitsForStopAckBeforeReload()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 2731;
    channel.name = QStringLiteral("Xtream Archive Stop Ack Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/2731.ts");

    const auto startUtc = QDateTime::fromString(QStringLiteral("2026-05-18T12:00:00Z"), Qt::ISODate);
    const auto stopUtc = startUtc.addSecs(3600);
    const auto canonicalUrl =
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/2731.ts");
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://cdn.example/archive/initial2731.ts"),
        QStringLiteral("12:00 - 13:00  Archive Show"),
        startUtc,
        stopUtc,
        canonicalUrl);

    playerController.seekTimeshiftToFraction(158.0 / 3600.0);

    QVERIFY(playerController.m_catchupSession.reloadInFlight());
    QCOMPARE(
        playerController.currentPlaybackUrl(),
        QStringLiteral("http://cdn.example/archive/initial2731.ts"));
    playerController.m_isPlaying = true;
    playerController.m_player.m_cachedTelemetry.positionSeconds = 900.0;
    playerController.updatePosition();
    QCOMPARE(playerController.catchupTimelinePositionSeconds(), 158.0);
    playerController.runCatchupTimelineReload();
    playerController.finishCatchupTimelineReload(QStringLiteral("test-stop-ack"));

    QTRY_VERIFY_WITH_TIMEOUT(!playerController.m_catchupSession.reloadInFlight(), 1000);
    QCOMPARE(
        playerController.currentPlaybackUrl(),
        QStringLiteral("http://provider.example/timeshift/user/pass/59/2026-05-18:12-02/2731.ts"));
}

void AppModelTests::playerControllerXtreamCatchupSeekTimeoutReloadsWithoutStopAck()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 2732;
    channel.name = QStringLiteral("Xtream Archive Timeout Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/2732.ts");

    const auto startUtc = QDateTime::fromString(QStringLiteral("2026-05-18T12:00:00Z"), Qt::ISODate);
    const auto stopUtc = startUtc.addSecs(3600);
    const auto canonicalUrl =
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/2732.ts");
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://cdn.example/archive/initial2732.ts"),
        QStringLiteral("12:00 - 13:00  Archive Show"),
        startUtc,
        stopUtc,
        canonicalUrl);

    playerController.seekTimeshiftToFraction(158.0 / 3600.0);
    QVERIFY(playerController.m_catchupSession.reloadInFlight());

    QTRY_VERIFY_WITH_TIMEOUT(!playerController.m_catchupSession.reloadInFlight(), 1000);
    QCOMPARE(
        playerController.currentPlaybackUrl(),
        QStringLiteral("http://provider.example/timeshift/user/pass/59/2026-05-18:12-02/2732.ts"));
}

void AppModelTests::playerControllerXtreamCatchupSeekCoalescesRapidClicksLastWins()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 2733;
    channel.name = QStringLiteral("Xtream Archive Coalesce Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/2733.ts");

    const auto startUtc = QDateTime::fromString(QStringLiteral("2026-05-18T12:00:00Z"), Qt::ISODate);
    const auto stopUtc = startUtc.addSecs(3600);
    const auto canonicalUrl =
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/2733.ts");
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://cdn.example/archive/initial2733.ts"),
        QStringLiteral("12:00 - 13:00  Archive Show"),
        startUtc,
        stopUtc,
        canonicalUrl);

    playerController.seekTimeshiftToFraction(158.0 / 3600.0);
    QVERIFY(playerController.m_catchupSession.reloadInFlight());
    playerController.seekTimeshiftToFraction(301.0 / 3600.0);
    QVERIFY(playerController.m_catchupSession.m_queuedSeek.has_value());
    QCOMPARE(
        static_cast<int>(std::lround(playerController.m_catchupSession.m_queuedSeek.value())),
        301);
    playerController.runCatchupTimelineReload();
    playerController.finishCatchupTimelineReload(QStringLiteral("test-stop-ack"));
    QTRY_VERIFY_WITH_TIMEOUT(!playerController.m_catchupSession.reloadInFlight(), 1000);
    playerController.runCatchupTimelineReload();
    playerController.finishCatchupTimelineReload(QStringLiteral("test-stop-ack"));
    QTRY_VERIFY_WITH_TIMEOUT(!playerController.m_catchupSession.reloadInFlight(), 1000);
    QCOMPARE(
        playerController.currentPlaybackUrl(),
        QStringLiteral("http://provider.example/timeshift/user/pass/56/2026-05-18:12-05/2733.ts"));
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
    QVERIFY(playerController.m_catchupSession.reloadInFlight());
    playerController.runCatchupTimelineReload();
    playerController.finishCatchupTimelineReload(QStringLiteral("test-stop-ack"));
    QCOMPARE(
        playerController.currentPlaybackUrl(),
        QStringLiteral("http://provider.example/timeshift/user/pass/59/2026-05-18:12-02/173.ts"));

    DebugLogger::instance().unsubscribe(subscriptionId);
}

void AppModelTests::playerControllerCatchupDegradationWatchdogTriggersSilentRecovery()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 3174;
    channel.name = QStringLiteral("Catch-up Degrade Silent Recovery Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3174.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-1200);
    const auto stopUtc = QDateTime::currentDateTimeUtc().addSecs(1800);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3174"),
        QStringLiteral("Past Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3174.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    playerController.m_isPlaying = true;
    playerController.m_backendBuffering = true;
    playerController.m_recovery.m_playbackStalled = true;
    playerController.m_recovery.m_lastPlaybackPositionSeconds = 100.0;
    playerController.m_catchupSession.m_catchupTimelineAvailableSeconds = 1800.0;
    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 120.0;

    for (int i = 0; i < 8; ++i) {
        playerController.m_player.m_cachedTelemetry.positionSeconds = 102.0 + i;
        playerController.m_player.m_cachedTelemetry.displayedVideoFramePtsSeconds = 101.0;
        playerController.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = 70.0;
        playerController.m_player.m_cachedTelemetry.cacheSpeedBytesPerSecond = 0.0;
        playerController.updatePosition();
    }

    // 80s degradation watchdog removed: low cache by itself must not trigger reconnect churn.
    QVERIFY(!playerController.m_recovery.active());
    QVERIFY(!playerController.m_catchupSession.reloadInFlight());
}

void AppModelTests::playerControllerCatchupDegradationWatchdogNearEndSuppressesRecovery()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 3175;
    channel.name = QStringLiteral("Catch-up Near-End Suppress Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3175.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-1200);
    const auto stopUtc = QDateTime::currentDateTimeUtc().addSecs(120);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3175"),
        QStringLiteral("Past Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3175.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    playerController.m_isPlaying = true;
    playerController.m_backendBuffering = true;
    playerController.m_recovery.m_playbackStalled = true;
    playerController.m_recovery.m_lastPlaybackPositionSeconds = 100.0;
    for (int i = 0; i < 10; ++i) {
        playerController.m_player.m_cachedTelemetry.positionSeconds = 1150.0 + i;
        playerController.m_player.m_cachedTelemetry.displayedVideoFramePtsSeconds = 100.0;
        playerController.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = 50.0;
        playerController.m_player.m_cachedTelemetry.cacheSpeedBytesPerSecond = 0.0;
        playerController.updatePosition();
    }

    QVERIFY(!playerController.m_recovery.active());
    QVERIFY(!playerController.m_catchupSession.reloadInFlight());
}

void AppModelTests::playerControllerCatchupDegradationWatchdogEscalatesToHardRestore()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 3176;
    channel.name = QStringLiteral("Catch-up Hard Restore Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3176.ts");

    const auto startUtc = QDateTime::fromString(QStringLiteral("2026-05-18T12:00:00Z"), Qt::ISODate);
    const auto stopUtc = startUtc.addSecs(3600);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3176"),
        QStringLiteral("Past Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3176.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    playerController.m_isPlaying = true;
    playerController.m_backendBuffering = true;
    playerController.m_recovery.m_playbackStalled = true;
    playerController.m_recovery.m_lastPlaybackPositionSeconds = 200.0;
    playerController.m_catchupSession.m_catchupTimelineAvailableSeconds = 1800.0;
    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 300.0;
    playerController.m_player.m_cachedTelemetry.positionSeconds = 201.0;
    playerController.m_player.m_cachedTelemetry.displayedVideoFramePtsSeconds = 200.0;
    playerController.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = 0.0;
    playerController.m_player.m_cachedTelemetry.cacheSpeedBytesPerSecond = 0.0;
    playerController.updatePosition();

    QVERIFY(playerController.m_catchupSession.reloadInFlight());
}

void AppModelTests::playerControllerCatchupDegradationWatchdogPrefersSeamlessCutoverWhenStandbyReady()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 3177;
    channel.name = QStringLiteral("Catch-up Seamless Degradation Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3177.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-1800);
    const auto stopUtc = QDateTime::currentDateTimeUtc().addSecs(1800);
    const auto canonicalUrl =
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3177.ts");
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3177"),
        QStringLiteral("Current Show"),
        startUtc,
        stopUtc,
        canonicalUrl);
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    playerController.m_isPlaying = true;
    playerController.m_backendBuffering = true;
    playerController.m_recovery.m_playbackStalled = true;
    playerController.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Armed;
    playerController.m_catchupSession.standby().m_ready = true;
    playerController.m_catchupSession.standby().m_videoReady = true;
    playerController.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Loading;
    playerController.m_catchupSeamlessStandbyPlayer = &playerController.m_catchupStandbyPlayer;
    playerController.m_catchupSession.standby().m_url =
        QStringLiteral("http://provider.example/timeshift/user/pass/59/2026-05-18:12-02/3177.ts");
    playerController.m_catchupSession.standby().m_baseOffset = 120.0;
    playerController.m_catchupSession.m_catchupTimelineAvailableSeconds = 1800.0;
    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 300.0;
    playerController.m_catchupSession.m_nearZeroTicks = 0;
    playerController.m_catchupSession.m_recoverySince.reset();

    playerController.evaluateCatchupDegradationRecovery(
        0.0,
        0.0,
        false,
        false);

    QCOMPARE(playerController.playbackPlayer(), &playerController.m_catchupStandbyPlayer);
    QCOMPARE(playerController.m_catchupStandbyPlayer.m_steadyStateCacheLimitSeconds, 90.0);
    QCOMPARE(playerController.m_catchupStandbyPlayer.m_steadyStateCacheHysteresisSeconds, 85.0);
    QCOMPARE(playerController.m_catchupStandbyPlayer.m_steadyStateDemuxerMaxBytes, static_cast<qint64>(96) * 1024 * 1024);
    QCOMPARE(playerController.m_catchupStandbyPlayer.m_steadyStateDemuxerMaxBackBytes, static_cast<qint64>(32) * 1024 * 1024);
    QVERIFY(!playerController.m_catchupSession.reloadInFlight());
    QVERIFY(!playerController.m_catchupSession.standby().pending());
}

void AppModelTests::playerControllerCatchupDegradationWatchdogFallsBackToHardRestoreWhenStandbyNotReady()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 3178;
    channel.name = QStringLiteral("Catch-up Degradation Fallback Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3178.ts");

    const auto startUtc = QDateTime::fromString(QStringLiteral("2026-05-18T12:00:00Z"), Qt::ISODate);
    const auto stopUtc = startUtc.addSecs(3600);
    const auto canonicalUrl =
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3178.ts");
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3178"),
        QStringLiteral("Past Show"),
        startUtc,
        stopUtc,
        canonicalUrl);
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    playerController.m_isPlaying = true;
    playerController.m_backendBuffering = true;
    playerController.m_recovery.m_playbackStalled = true;
    playerController.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Armed;
    playerController.m_catchupSession.standby().m_ready = false;
    playerController.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Loading;
    playerController.m_catchupSession.m_catchupTimelineAvailableSeconds = 1800.0;
    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 300.0;
    playerController.m_catchupSession.m_nearZeroTicks = 0;
    playerController.m_catchupSession.m_recoverySince.reset();

    playerController.evaluateCatchupDegradationRecovery(
        0.0,
        0.0,
        false,
        false);

    QVERIFY(playerController.m_catchupSession.reloadInFlight());
}

void AppModelTests::playerControllerCatchupProgrammeBoundaryBypassesSeamlessExtension()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 3180;
    channel.name = QStringLiteral("Catch-up Programme End Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3180.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-3900);
    const auto stopUtc = startUtc.addSecs(3598);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3180"),
        QStringLiteral("Past Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3180.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    playerController.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Armed;
    playerController.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Loading;
    playerController.m_catchupSession.standby().m_ready = false;
    playerController.m_catchupSession.standby().m_fallbackDeferred = true;
    playerController.m_catchupSession.standby().fallbackTimer().start();
    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 3598.5;

    QVERIFY(playerController.handleCatchupPlaybackEndedRecovery());

    QTRY_VERIFY_WITH_TIMEOUT(!playerController.currentChannelValue().has_value(), 1000);
    QCOMPARE(playerController.playbackMode(), QStringLiteral("live"));
    QVERIFY(!playerController.m_catchupSession.standby().pending());
    QVERIFY(!playerController.m_catchupSession.reloadInFlight());
}

void AppModelTests::playerControllerCatchupProgrammeBoundaryDetectionRequiresStopEdgeAndEndPosition()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 31801;
    channel.name = QStringLiteral("Catch-up Boundary Guard Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/31801.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-900);
    const auto stopUtc = QDateTime::currentDateTimeUtc().addSecs(900);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup31801"),
        QStringLiteral("Running Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/31801.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 1799.5;
    QVERIFY(!playerController.maybeStopCatchupAtProgrammeBoundary(
        1799.5,
        std::nullopt,
        QStringLiteral("test-running-edge")));
    QCOMPARE(playerController.playbackMode(), QStringLiteral("catchup"));
    QVERIFY(playerController.currentChannelValue().has_value());
    QVERIFY(!playerController.m_catchupSession.m_catchupProgramBoundaryReached);

    const auto pastStartUtc = QDateTime::currentDateTimeUtc().addSecs(-3600);
    const auto pastStopUtc = QDateTime::currentDateTimeUtc().addSecs(-2);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup31801-past"),
        QStringLiteral("Past Show"),
        pastStartUtc,
        pastStopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:11-00/31801.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 300.0;
    QVERIFY(!playerController.maybeStopCatchupAtProgrammeBoundary(
        300.0,
        std::nullopt,
        QStringLiteral("test-past-not-ended")));
    QCOMPARE(playerController.playbackMode(), QStringLiteral("catchup"));
    QVERIFY(playerController.currentChannelValue().has_value());
    QVERIFY(!playerController.m_catchupSession.m_catchupProgramBoundaryReached);

    // Wall-clock programme end is not yet the published archive edge.
    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 3598.5;
    QVERIFY(!playerController.maybeStopCatchupAtProgrammeBoundary(
        3598.5, std::nullopt, QStringLiteral("test-end-inside-safety-margin")));
}

void AppModelTests::playerControllerCatchupPastProgrammeDemuxerEofStopsLikeExplicitStop()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 31802;
    channel.name = QStringLiteral("Catch-up EOF Stop Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/31802.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-3900);
    const auto stopUtc = startUtc.addSecs(3598);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup31802"),
        QStringLiteral("Past Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/31802.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    playerController.m_player.m_cachedTelemetry.positionSeconds = 3598.5;
    playerController.m_player.m_cachedTelemetry.displayedVideoFramePtsSeconds = 3598.5;
    playerController.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = 2.0;
    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 3598.5;
    playerController.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Armed;
    playerController.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Loading;
    playerController.m_catchupSession.standby().m_ready = false;
    playerController.m_catchupSession.standby().m_fallbackDeferred = true;
    playerController.m_catchupSession.standby().fallbackTimer().start();
    playerController.m_isPlaying = true;

    QVERIFY(!playerController.maybeStopCatchupAtProgrammeBoundary(
        3598.5,
        2.0,
        QStringLiteral("test-buffered-boundary")));
    QVERIFY(playerController.currentChannelValue().has_value());
    QCOMPARE(playerController.playbackMode(), QStringLiteral("catchup"));

    playerController.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = 0.4;
    QVERIFY(playerController.maybeStopCatchupAtProgrammeBoundary(
        3598.5,
        0.4,
        QStringLiteral("test-direct-boundary")));
    QTRY_VERIFY_WITH_TIMEOUT(!playerController.currentChannelValue().has_value(), 1000);
    QCOMPARE(playerController.playbackMode(), QStringLiteral("live"));
    QCOMPARE(playerController.nowPlayingName(), QStringLiteral("No channel"));
    QCOMPARE(playerController.currentPlaybackUrl(), QStringLiteral(""));
    QVERIFY(!playerController.m_catchupSession.standby().pending());
    QVERIFY(!playerController.m_catchupSession.reloadInFlight());
}

void AppModelTests::playerControllerCatchupSeamlessStandbyRetriesWithStopFirstBackoff()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 3181;
    channel.name = QStringLiteral("Catch-up Seamless Retry Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3181.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-900);
    const auto stopUtc = QDateTime::currentDateTimeUtc().addSecs(900);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3181"),
        QStringLiteral("Current Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3181.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    const auto armed = playerController.armSeamlessCatchupRollingExtension(
        QStringLiteral("test"),
        300.0,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-05/3181.ts"),
        300.0);
    QVERIFY(armed);

    QVERIFY(playerController.startSeamlessCatchupStandbyLoad());
    QVERIFY(playerController.m_catchupSession.standby().stopPending());
    QVERIFY(!playerController.m_catchupSession.standby().loadIssued());

    QVERIFY(QMetaObject::invokeMethod(&playerController.m_catchupStandbyPlayer, "playbackStopped", Qt::DirectConnection));
    QVERIFY(playerController.m_catchupSession.standby().loadIssued());
    QVERIFY(!playerController.m_catchupSession.standby().stopPending());
    QVERIFY(playerController.m_catchupStandbyPlayer.audioEnabledRequested());
    QVERIFY(playerController.m_catchupSession.standby().m_lastAttempt.has_value());

    QVERIFY(QMetaObject::invokeMethod(
        &playerController.m_catchupStandbyPlayer,
        "errorOccurred",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("simulated-standby-failure"))));
    QVERIFY(!playerController.m_catchupSession.standby().loadIssued());

    QVERIFY(!playerController.startSeamlessCatchupStandbyLoad());
    QVERIFY(!playerController.m_catchupSession.standby().stopPending());
}

void AppModelTests::playerControllerCatchupSeamlessStandbyRetryRefreshesXtreamUrl()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 3182;
    channel.name = QStringLiteral("Catch-up Seamless Refresh Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3182.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-1800);
    const auto stopUtc = QDateTime::currentDateTimeUtc().addSecs(1800);
    const auto canonicalUrl =
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3182.ts");
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3182"),
        QStringLiteral("Current Show"),
        startUtc,
        stopUtc,
        canonicalUrl);
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    const auto armedUrl =
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2001-01-01:00-00/3182.ts");
    const auto armed = playerController.armSeamlessCatchupRollingExtension(
        QStringLiteral("test"),
        300.0,
        armedUrl,
        300.0);
    QVERIFY(armed);

    QVERIFY(playerController.launchSeamlessCatchupStandbyLoad(&playerController.m_catchupStandbyPlayer));
    QVERIFY(playerController.m_catchupSession.standby().loadIssued());
    QVERIFY(!playerController.m_catchupSession.standby().url().isEmpty());
    QVERIFY(playerController.m_catchupSession.standby().url() != armedUrl);
    QVERIFY(playerController.m_catchupSession.standby().baseOffset() >= 0.0);
    // Wall-clock publication advanced while recovery was paused. The next
    // fallback response must follow the watched position, not the old delay.
    playerController.m_catchupSession.m_catchupContinuousFallback = true;
    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 152.0;
    playerController.m_catchupSession.m_catchupDesiredDelaySeconds = 0.0;
    QVERIFY(playerController.refreshSeamlessCatchupStandbyRetryUrl());
    QCOMPARE(playerController.m_catchupSession.standby().baseOffset(), 120.0);
    playerController.abortSeamlessCatchupRolling(QStringLiteral("test-eof-reload"), true);
    playerController.m_catchupSession.m_lastRolling.reset();
    playerController.m_catchupSession.m_rollingSince.reset();
    QVERIFY(playerController.extendCatchupRollingWindow(QStringLiteral("playback-ended"), false));
    QCOMPARE(playerController.m_catchupSession.m_catchupContinuousRecoveryTarget, std::optional<double>(152.0));
    playerController.runCatchupTimelineReload();
    QVERIFY(!playerController.m_catchupSession.m_catchupReconnectResumeStreamRelativeSeconds.has_value());
    QCOMPARE(playerController.m_catchupSession.m_catchupTimelinePositionSeconds, 120.0);


}

void AppModelTests::playerControllerCatchupSeamlessCutoverRequiresStandbyVideoReady()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 3183;
    channel.name = QStringLiteral("Catch-up Seamless Video Ready Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3183.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-900);
    const auto stopUtc = QDateTime::currentDateTimeUtc().addSecs(900);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3183"),
        QStringLiteral("Current Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3183.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    const auto armed = playerController.armSeamlessCatchupRollingExtension(
        QStringLiteral("test"),
        300.0,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-05/3183.ts"),
        300.0);
    QVERIFY(armed);
    QVERIFY(playerController.launchSeamlessCatchupStandbyLoad(&playerController.m_catchupStandbyPlayer));
    QVERIFY(playerController.seamlessStandbyPrewarmActive());
    QVERIFY(playerController.seamlessStandbyPlayerObject() != nullptr);

    QVERIFY(QMetaObject::invokeMethod(&playerController.m_catchupStandbyPlayer, "playbackRestarted", Qt::DirectConnection));
    QVERIFY(playerController.m_catchupSession.standby().ready());
    QVERIFY(!playerController.m_catchupSession.standby().videoReady());
    QVERIFY(!playerController.standbySeamlessVideoReady());
    QVERIFY(!playerController.usingSharedPlayback());

    playerController.m_catchupSession.standby().m_fallbackDeferred = true;
    playerController.m_catchupSession.standby().m_videoReady = true;
    QVERIFY(playerController.standbySeamlessVideoReady());
}

void AppModelTests::playerControllerCatchupSeamlessSecondCycleTracksBaseStandbySignals()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 3184;
    channel.name = QStringLiteral("Catch-up Seamless Second Cycle Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3184.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-900);
    const auto stopUtc = QDateTime::currentDateTimeUtc().addSecs(900);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3184"),
        QStringLiteral("Current Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3184.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    // Simulate post-first-cutover state: active playback is standby player, so next standby is base player.
    playerController.setSharedPlaybackPlayer(&playerController.m_catchupStandbyPlayer, false);
    QCOMPARE(playerController.playbackPlayer(), &playerController.m_catchupStandbyPlayer);

    const auto armed = playerController.armSeamlessCatchupRollingExtension(
        QStringLiteral("test-second-cycle"),
        300.0,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-05/3184.ts"),
        300.0);
    QVERIFY(armed);
    QCOMPARE(playerController.m_catchupSeamlessStandbyPlayer.data(), &playerController.m_player);
    QVERIFY(playerController.launchSeamlessCatchupStandbyLoad(&playerController.m_player));
    QVERIFY(playerController.m_catchupSession.standby().loadIssued());
    QVERIFY(!playerController.m_catchupSession.standby().ready());
    QVERIFY(!playerController.m_catchupSession.standby().videoReady());

    QVERIFY(QMetaObject::invokeMethod(&playerController.m_player, "playbackRestarted", Qt::DirectConnection));
    QVERIFY(playerController.m_catchupSession.standby().ready());
    QVERIFY(!playerController.m_catchupSession.standby().videoReady());

    QVERIFY(QMetaObject::invokeMethod(&playerController.m_player, "videoReconfigured", Qt::DirectConnection));
    QVERIFY(playerController.m_catchupSession.standby().videoReady());
}

void AppModelTests::playerControllerCatchupSeamlessStandbyFailureArmsFastRetry()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 3185;
    channel.name = QStringLiteral("Catch-up Seamless Fast Retry Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3185.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-900);
    const auto stopUtc = QDateTime::currentDateTimeUtc().addSecs(900);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3185"),
        QStringLiteral("Current Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3185.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    QVERIFY(playerController.armSeamlessCatchupRollingExtension(
        QStringLiteral("test-fast-retry"),
        300.0,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-05/3185.ts"),
        300.0));
    QVERIFY(playerController.launchSeamlessCatchupStandbyLoad(&playerController.m_catchupStandbyPlayer));
    QVERIFY(playerController.m_catchupSession.standby().loadIssued());

    QVERIFY(QMetaObject::invokeMethod(
        &playerController.m_catchupStandbyPlayer,
        "errorOccurred",
        Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("simulated-transient-provider-error"))));

    QVERIFY(playerController.m_catchupSession.standby().retryPending());
    QVERIFY(playerController.m_catchupSession.standby().retryBudget() > 0);
    QVERIFY(playerController.m_catchupSession.standby().m_retryWindow.has_value());
    QVERIFY(playerController.m_catchupSession.standby().retryTimer().isActive());
}

void AppModelTests::playerControllerCatchupDegradationWatchdogDefersHardRestoreDuringFastRetryWindow()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 3186;
    channel.name = QStringLiteral("Catch-up Seamless Retry Grace Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3186.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-1800);
    const auto stopUtc = QDateTime::currentDateTimeUtc().addSecs(1800);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3186"),
        QStringLiteral("Current Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3186.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    playerController.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Armed;
    playerController.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Armed;
    playerController.m_catchupSession.standby().m_retryPending = true;
    playerController.m_catchupSession.standby().m_retryBudget = 1;
    playerController.m_catchupSession.standby().m_retryWindow = playerController.m_liveDeliveryClock.elapsed();
    playerController.m_catchupSession.m_nearZeroTicks = 0;
    playerController.m_catchupSession.m_recoverySince.reset();
    playerController.m_catchupSession.m_catchupTimelineAvailableSeconds = 1800.0;
    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 1200.0;

    playerController.evaluateCatchupDegradationRecovery(
        0.0,
        0.0,
        false,
        false);

    QVERIFY(!playerController.m_catchupSession.reloadInFlight());
    QVERIFY(playerController.m_catchupSession.standby().pending());
}

void AppModelTests::playerControllerCatchupEofCloseArmsSeamlessRolloverRegardlessOfCacheLevel()
{
    const auto runScenario = [](const double cacheDurationSeconds) {
        PlayerController playerController;

        Channel channel;
        channel.id = 3187;
        channel.name = QStringLiteral("Catch-up EOF Arm Channel");
        channel.profileId = QUuid::createUuid();
        channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3187.ts");

        const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-1800);
        const auto stopUtc = QDateTime::currentDateTimeUtc().addSecs(1800);
        playerController.playCatchupChannel(
            channel,
            QStringLiteral("http://127.0.0.1/catchup3187"),
            QStringLiteral("Current Show"),
            startUtc,
            stopUtc,
            QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3187.ts"));
        QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
        QVERIFY(QMetaObject::invokeMethod(
            playerController.player(),
            "pauseStateChanged",
            Qt::DirectConnection,
            Q_ARG(bool, false)));

        playerController.m_isPlaying = true;
        playerController.m_player.m_cachedTelemetry.positionSeconds = 42.0;
        playerController.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = cacheDurationSeconds;
        playerController.m_player.m_cachedTelemetry.cacheSpeedBytesPerSecond = 0.0;
        playerController.m_catchupSession.m_catchupTimelineAvailableSeconds = 1800.0;
        playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 1680.0;
        playerController.m_catchupSession.m_catchupTransportEndTimelineSeconds = 1500.0;
        playerController.m_catchupSession.m_catchupDesiredDelaySeconds = 120.0;
        playerController.m_catchupSession.m_catchupActiveEofObserved = true;
        auto closedSession = OKILTV::Player::CatchupStreamSession::create(
            QStringLiteral("http://provider.example/archive/segment.ts"));
        closedSession->closeProviderConnection(QStringLiteral("test-closed"));
        playerController.m_catchupActiveStreamSession = closedSession;

        playerController.updatePosition();

        const auto rolloverStarted = playerController.m_catchupSession.standby().pending()
            || playerController.m_catchupSession.standby().loadIssued()
            || playerController.m_catchupSession.standby().ready();
        QVERIFY(rolloverStarted);
        QVERIFY(!playerController.m_catchupSession.reloadInFlight());
    };

    runScenario(59.0);
    runScenario(90.0);
}

void AppModelTests::playerControllerCatchupEofCloseWaitsBeforeStandbyWarmup()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 3188;
    channel.name = QStringLiteral("Catch-up EOF Delay Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3188.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-1800);
    const auto stopUtc = QDateTime::currentDateTimeUtc().addSecs(1800);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3188"),
        QStringLiteral("Current Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3188.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    playerController.m_isPlaying = true;
    playerController.m_player.m_cachedTelemetry.positionSeconds = 42.0;
    playerController.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = 10.0;
    playerController.m_player.m_cachedTelemetry.cacheSpeedBytesPerSecond = 0.0;
    playerController.m_catchupSession.m_catchupTimelineAvailableSeconds = 1620.0;
    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 1480.0;
    playerController.m_catchupSession.m_catchupTransportEndTimelineSeconds = 1485.0;
    playerController.m_catchupSession.m_catchupDesiredDelaySeconds = 120.0;
    playerController.m_catchupSession.m_catchupActiveEofObserved = true;
    auto closedSession = OKILTV::Player::CatchupStreamSession::create(
        QStringLiteral("http://provider.example/archive/segment.ts"));
    closedSession->closeProviderConnection(QStringLiteral("test-closed"));
    playerController.m_catchupActiveStreamSession = closedSession;

    playerController.updatePosition();

    QVERIFY(playerController.m_catchupSession.standby().pending());
    QVERIFY(!playerController.m_catchupSession.standby().loadIssued());
    QVERIFY(playerController.m_catchupSession.standby().delayPending());
    QVERIFY(playerController.m_catchupSession.standby().delayTimer().isActive());
}

void AppModelTests::playerControllerCatchupSeamlessCutoverDoesNotForceCloseNewActiveSessionFromStaleEofTick_data()
{
    QTest::addColumn<double>("activeCacheSeconds");
    QTest::newRow("degradation-cutover") << 0.4;
    QTest::newRow("near-edge-cutover") << 10.0;
}

void AppModelTests::playerControllerCatchupSeamlessCutoverDoesNotForceCloseNewActiveSessionFromStaleEofTick()
{
    QFETCH(double, activeCacheSeconds);
    PlayerController playerController;

    Channel channel;
    channel.id = 3191;
    channel.name = QStringLiteral("Catch-up Stale EOF Guard Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3191.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-1800);
    const auto stopUtc = QDateTime::currentDateTimeUtc().addSecs(1800);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3191"),
        QStringLiteral("Current Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3191.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    playerController.m_catchupSession.m_catchupProgressTransportReady = false; // A replacement is still loading.
    playerController.m_catchupSession.m_catchupProgressSeekTargetSeconds = 900.0;

    // Prepare a committed seamless cutover path during degradation recovery.
    playerController.m_isPlaying = true;
    playerController.m_player.m_cachedTelemetry.positionSeconds = 1200.0;
    playerController.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = activeCacheSeconds;
    playerController.m_player.m_cachedTelemetry.cacheSpeedBytesPerSecond = 0.0;
    playerController.m_player.m_cachedTelemetry.displayedVideoFramePtsSeconds = 100.0;
    playerController.m_catchupStandbyPlayer.m_cachedTelemetry.demuxerCacheDurationSeconds = 108.0;
    playerController.m_catchupStandbyPlayer.m_cachedTelemetry.positionSeconds = 2.0;

    playerController.m_catchupSession.m_catchupTimelineAvailableSeconds = 1600.0;
    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 1200.0;
    playerController.m_catchupSession.m_catchupTransportEndTimelineSeconds = 1200.2;
    playerController.m_catchupSession.m_catchupDesiredDelaySeconds = 100.0;
    playerController.m_catchupSession.m_nearZeroTicks = 0;
    playerController.m_catchupSession.m_recoverySince.reset();

    playerController.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Armed;
    playerController.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Loading;
    playerController.m_catchupSession.standby().m_ready = true;
    playerController.m_catchupSession.standby().m_videoReady = true;
    playerController.m_catchupSeamlessStandbyPlayer = &playerController.m_catchupStandbyPlayer;
    playerController.m_catchupSession.standby().m_url = QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-10/3191.ts");
    playerController.m_catchupSession.standby().m_baseOffset = 1200.0;
    playerController.m_catchupSession.standby().m_fallbackDeferred = true;

    auto oldActiveSession = OKILTV::Player::CatchupStreamSession::create(
        QStringLiteral("http://provider.example/archive/old-segment.ts"));
    auto promotedSession = OKILTV::Player::CatchupStreamSession::create(
        QStringLiteral("http://provider.example/archive/new-segment.ts"));
    playerController.m_catchupActiveStreamSession = oldActiveSession;
    playerController.m_catchupStandbyStreamSession = promotedSession;

    playerController.updatePosition();
    if (activeCacheSeconds > 1.0) {
        QCOMPARE(playerController.playbackPlayer(), &playerController.m_player);
        playerController.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = 0.2;
        playerController.m_catchupSession.m_recoverySince = playerController.m_liveDeliveryClock.elapsed(); // Exercise near-edge cutover, not the degradation watchdog.
        playerController.updatePosition();
    }

    QCOMPARE(playerController.playbackPlayer(), &playerController.m_catchupStandbyPlayer);
    QVERIFY(playerController.m_catchupSession.m_catchupProgressTransportReady);
    QVERIFY(!playerController.m_catchupSession.m_catchupProgressSeekTargetSeconds.has_value());
    QVERIFY(playerController.m_catchupActiveStreamSession);
    QCOMPARE(playerController.m_catchupActiveStreamSession->sourceUrl(), QStringLiteral("http://provider.example/archive/new-segment.ts"));
    QVERIFY(!playerController.m_catchupActiveStreamSession->closeRequestedByApp());
    QCOMPARE(playerController.catchupTimelinePositionSeconds(), 1202.0);
    QVERIFY(!playerController.catchupTimelineAtLiveEdge());
    QVERIFY(!playerController.m_catchupSession.m_catchupProgramBoundaryReached);

    playerController.updatePosition();
    QCOMPARE(playerController.catchupTimelinePositionSeconds(), 1202.0);
    QVERIFY(!playerController.m_catchupSession.reloadInFlight());
    QVERIFY(!playerController.m_catchupSession.m_catchupRollbackDeferredPending);
    QVERIFY(!playerController.m_catchupSession.m_catchupRollbackGuardConsumed);
}

void AppModelTests::playerControllerCatchupSeamlessRejectsDeadStandbySession_data()
{
    QTest::addColumn<bool>("hasMedia");
    QTest::addColumn<bool>("drain");
    QTest::newRow("empty-response") << false << false;
    QTest::newRow("completed-with-queued-media") << true << false;
    QTest::newRow("completed-with-mpv-cache") << true << true;
}

void AppModelTests::playerControllerCatchupSeamlessRejectsDeadStandbySession()
{
    QFETCH(bool, hasMedia);
    QFETCH(bool, drain);
    PlayerController playerController;

    Channel channel;
    channel.id = 3192;
    channel.name = QStringLiteral("Catch-up Dead Standby Guard Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3192.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-1800);
    const auto stopUtc = QDateTime::currentDateTimeUtc().addSecs(1800);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3192"),
        QStringLiteral("Current Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3192.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    playerController.m_isPlaying = true;
    playerController.m_player.m_cachedTelemetry.positionSeconds = 400.0;
    playerController.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = 0.4;
    playerController.m_player.m_cachedTelemetry.cacheSpeedBytesPerSecond = 0.0;
    playerController.m_player.m_cachedTelemetry.displayedVideoFramePtsSeconds = 100.0;
    playerController.m_catchupStandbyPlayer.m_cachedTelemetry.demuxerCacheDurationSeconds = 108.0;
    playerController.m_catchupStandbyPlayer.m_cachedTelemetry.positionSeconds = 402.0;

    playerController.m_catchupSession.m_catchupTimelineAvailableSeconds = 1600.0;
    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 1200.0;
    playerController.m_catchupSession.m_catchupTransportEndTimelineSeconds = 1200.2;
    playerController.m_catchupSession.m_catchupDesiredDelaySeconds = 100.0;
    playerController.m_catchupSession.m_nearZeroTicks = 0;
    playerController.m_catchupSession.m_recoverySince.reset();

    playerController.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Armed;
    playerController.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Loading;
    playerController.m_catchupSession.standby().m_ready = true;
    playerController.m_catchupSession.standby().m_videoReady = true;
    playerController.m_catchupSeamlessStandbyPlayer = &playerController.m_catchupStandbyPlayer;
    playerController.m_catchupSession.standby().m_url =
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-10/3192.ts");
    playerController.m_catchupSession.standby().m_baseOffset = 600.0;
    playerController.m_catchupSession.standby().m_fallbackDeferred = true;

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server, [&server, hasMedia]() {
        while (auto *socket = server.nextPendingConnection()) {
            const auto body = hasMedia ? QByteArray("media") : QByteArray {};
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size())
                          + "\r\nConnection: close\r\n\r\n" + body);
            socket->flush();
            socket->disconnectFromHost();
        }
    });
    auto deadStandbySession = OKILTV::Player::CatchupStreamSession::create(
        QStringLiteral("http://127.0.0.1:%1/dead-standby.ts").arg(server.serverPort()));
    QVERIFY(deadStandbySession->start());
    QTRY_VERIFY_WITH_TIMEOUT(deadStandbySession->providerConnectionClosed(), 1000);
    QVERIFY(!deadStandbySession->closeRequestedByApp());
    playerController.m_catchupStandbyStreamSession = deadStandbySession;

    if (hasMedia) {
        if (drain) {
            char buffer[5];
            QCOMPARE(deadStandbySession->read(buffer, sizeof(buffer)), 5);
            QCOMPARE(deadStandbySession->bufferedBytes(), 0);
        }
        QVERIFY(playerController.standbyCatchupSessionHealthyForCutover());
        playerController.m_catchupSession.m_catchupTransportEndTimelineSeconds = -421.0; // Stale URL edge from the reported failure.
        playerController.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = 13.87;
        QVERIFY(!playerController.maybeCommitSeamlessCatchupCutover(QStringLiteral("test-buffered-tail")));
        playerController.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = 0.0;
        playerController.m_catchupSession.m_catchupContinuousFallback = true;
        playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 152.0;
        playerController.m_catchupSession.standby().m_baseOffset = 120.0;
        playerController.m_catchupStandbyPlayer.m_cachedTelemetry.positionSeconds = 0.0;
        playerController.m_catchupStandbyPlayer.m_cachedTelemetry.demuxerSeekableRangeSeconds = std::make_pair(0.0, 90.0);
        QVERIFY(!playerController.maybeCommitSeamlessCatchupCutover(QStringLiteral("test-align-before-cutover")));
        QVERIFY(playerController.m_catchupSession.standby().alignmentIssued());
        for (int tick = 0; tick < 5; ++tick) {
            playerController.evaluateCatchupDegradationRecovery(0.1, 0.0, false, false);
        }
        QVERIFY(!playerController.m_catchupSession.reloadInFlight());
        QVERIFY(playerController.m_catchupSession.standby().pending());
        playerController.m_catchupStandbyPlayer.m_cachedTelemetry.positionSeconds = 32.0;
        playerController.m_player.m_cachedTelemetry.pauseState = true;
        playerController.m_isPlaying = false;
        playerController.m_userPausedManually = true;
        QVERIFY(!playerController.recoverCatchupAtAutomaticEof(true));
        playerController.m_userPausedManually = false;
        QVERIFY(playerController.recoverCatchupAtAutomaticEof(true));
        QCOMPARE(playerController.playbackPlayer(), &playerController.m_catchupStandbyPlayer);
    } else {
        QVERIFY(!playerController.maybeCommitSeamlessCatchupCutover(QStringLiteral("test-empty-response"), true));
        QCOMPARE(playerController.playbackPlayer(), &playerController.m_player);
        QVERIFY(!playerController.m_catchupSession.standby().ready());
        // A failed old response must not prevent opening its replacement.
        QVERIFY(playerController.startSeamlessCatchupStandbyLoad());
        QVERIFY(playerController.m_catchupSession.standby().stopPending());
    }
}

void AppModelTests::playerControllerCatchupReconnectWaitsForTransportSettleBeforeAttempt()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 4501;
    channel.name = QStringLiteral("Catch-up Reconnect Settle Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/4501.ts");

    const auto startUtc = QDateTime::currentDateTimeUtc().addSecs(-1800);
    const auto stopUtc = QDateTime::currentDateTimeUtc().addSecs(1800);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup4501"),
        QStringLiteral("Past Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/4501.ts"));
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
        Q_ARG(QString, QStringLiteral("simulated-catchup-reconnect"))));
    QVERIFY(playerController.m_recovery.active());
    QVERIFY(playerController.m_recovery.waitingStop());
    QCOMPARE(playerController.m_recovery.attempts(), 0);
    QVERIFY(!playerController.m_recovery.attemptInFlight());

    QTest::qWait(500);
    playerController.handleReconnectAttemptTick();
    QCOMPARE(playerController.m_recovery.attempts(), 1);
    QVERIFY(playerController.m_recovery.attemptInFlight());
}

void AppModelTests::playerControllerCatchupReconnectAttemptLimitEscalatesToHardRestore()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 4502;
    channel.name = QStringLiteral("Catch-up Reconnect Limit Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/4502.ts");

    const auto startUtc = QDateTime::fromString(QStringLiteral("2026-05-18T12:00:00Z"), Qt::ISODate);
    const auto stopUtc = startUtc.addSecs(3600);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup4502"),
        QStringLiteral("Past Show"),
        startUtc,
        stopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/4502.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    playerController.m_recovery.start();
    playerController.m_recovery.m_reconnectAttemptCount = 5;
    playerController.m_catchupSession.m_catchupTimelineAvailableSeconds = 1800.0;
    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 300.0;

    playerController.handleReconnectAttemptTick();

    QVERIFY(playerController.m_catchupSession.reloadInFlight());
    QVERIFY(!playerController.m_channelLoadFailed);
}

void AppModelTests::playerControllerCatchupCurrentProgramFollowsPlayback()
{
    PlayerController controller;
    Channel channel;
    channel.id = 4698;
    channel.tvgId = QStringLiteral("archive-test");
    channel.streamUrl = QStringLiteral("http://provider.example/live/channel.ts");
    EpgEntry program;
    program.channelId = channel.tvgId;
    program.title = QStringLiteral("Archive programme");
    program.start = QDateTime::currentDateTimeUtc().addSecs(-7200);
    program.stop = program.start.addSecs(3600);
    QVariantMap atActivation;
    connect(&controller, &PlayerController::playbackChannelActivated, &controller,
        [&controller, &atActivation]() { atActivation = controller.catchupCurrentProgram(); });
    controller.playCatchupChannel(channel, QStringLiteral("http://provider.example/archive.ts"),
        QStringLiteral("A formatted transport label"), program.start, program.stop, {},
        std::nullopt, std::nullopt, 900.0, 180, false, program);
    QCOMPARE(atActivation.value(QStringLiteral("title")).toString(), program.title);
    QCOMPARE(atActivation.value(QStringLiteral("timeRange")).toString(), epgEntryTimeRange(program));
    QCOMPARE(atActivation.value(QStringLiteral("progressPercent")).toDouble(), 25.0);
    QSignalSpy changed(&controller, &PlayerController::catchupTimelineChanged);
    controller.m_catchupSession.m_catchupTimelinePositionSeconds = 1800;
    controller.syncCatchupTimelineState();
    QVERIFY(!changed.isEmpty());
    QCOMPARE(controller.catchupCurrentProgram().value(QStringLiteral("progressPercent")).toDouble(), 50.0);
    controller.syncCatchupTimelineState(); // An unchanged/paused media clock must not advance progress.
    QCOMPARE(controller.catchupCurrentProgram().value(QStringLiteral("progressPercent")).toDouble(), 50.0);
    controller.m_catchupSession.m_catchupTimelinePositionSeconds = 0;
    controller.syncCatchupTimelineState();
    QCOMPARE(controller.catchupCurrentProgram().value(QStringLiteral("progressPercent")).toDouble(), 0.0);
    controller.returnToLiveFromCatchup();
    QVERIFY(controller.catchupCurrentProgram().isEmpty());
    controller.playCatchupChannel(channel, QStringLiteral("http://provider.example/archive.ts"),
        program.title, program.start, program.stop);
    QVERIFY(controller.catchupCurrentProgram().isEmpty());
    controller.stop();
    QVERIFY(controller.catchupCurrentProgram().isEmpty());
}

void AppModelTests::appControllerArchivesContinueAcrossProgrammes_data()
{
    QTest::addColumn<QString>("mode");
    QTest::newRow("m3u-default") << QStringLiteral("default");
    QTest::newRow("m3u-append") << QStringLiteral("append");
}

void AppModelTests::appControllerArchivesContinueAcrossProgrammes()
{
    QFETCH(QString, mode);
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->epgRefreshInProgress(), 5000);
    auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    auto &channel = channels[0];
    channel.catchupSupported = true;
    channel.catchupWindowHours = 72;
    channel.catchupMode = mode;
    channel.catchupSourceTemplate = (mode == QStringLiteral("default") ? QStringLiteral("http://127.0.0.1:1/archive.ts?") : QString {})
        + QStringLiteral("utc={utc}&lutc={lutc}&duration={duration}");
    harness.channelListModel->setChannels(channels, {});
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-7200);
    QList<EpgEntry> entries;
    for (int i = 0; i < 3; ++i) {
        EpgEntry entry;
        entry.channelId = channel.tvgId;
        entry.title = QStringLiteral("Programme %1").arg(i);
        entry.start = start.addSecs(i * 600);
        entry.stop = entry.start.addSecs(600);
        entries.append(entry);
    }
    harness.epgService->loadFromEntries(entries);
    harness.appController->playCatchup(toVariantMap(channel), toVariantMap(entries.first()));
    auto *player = harness.playerController.get();
    player->m_positionTimer.stop();
    QVERIFY(player->m_catchupSession.m_catchupEndless);
    QVERIFY(!player->m_currentLoadfileOptions.contains(QStringLiteral("length=")));
    const auto url = player->currentPlaybackUrl();
    QSignalSpy activated(player, &PlayerController::playbackChannelActivated);
    for (int i = 0; i < 3; ++i) {
        const auto offset = i * 600 + 65;
        player->m_catchupSession.m_catchupTimelinePositionSeconds = offset;
        player->syncCatchupTimelineState();
        QCOMPARE(player->catchupCurrentProgram().value(QStringLiteral("title")).toString(), entries[i].title);
        QCOMPARE(player->catchupTimelinePositionSeconds(), 65.0);
        QVERIFY(!player->maybeStopCatchupAtProgrammeBoundary(offset, 0.0, QStringLiteral("test")));
        emit player->catchupProgressObserved({channel, start, entries.first().stop, start.addSecs(offset), true});
        QVERIFY(harness.appController->catchupActionState(toVariantMap(channel), toVariantMap(entries[i]))
            .value(QStringLiteral("resumeAvailable")).toBool());
        QCOMPARE(player->currentPlaybackUrl(), url);
    }
    QCOMPARE(activated.count(), 0);
    QVERIFY(!harness.appController->catchupActionState(toVariantMap(channel), toVariantMap(entries.first()))
        .value(QStringLiteral("resumeAvailable")).toBool());
    double base = -1;
    const auto regenerated = player->regeneratedCatchupUrl(1265, &base);
    QCOMPARE(base, 1265.0);
    QCOMPARE(QUrlQuery(QUrl(regenerated)).queryItemValue(QStringLiteral("utc")).toLongLong(), start.addSecs(1265).toSecsSinceEpoch());
    QVERIFY(QUrlQuery(QUrl(regenerated)).queryItemValue(QStringLiteral("lutc")).toLongLong() > entries.last().stop.toSecsSinceEpoch());
    // Neither missing EPG nor a passive refresh can end the session or reuse an old programme bookmark.
    player->m_catchupSession.m_catchupTimelinePositionSeconds = 1900;
    player->syncCatchupTimelineState();
    QVERIFY(player->catchupCurrentProgram().isEmpty());
    QCOMPARE(player->catchupProgramLabel(), QStringLiteral("Catch-up"));
    emit player->catchupProgressObserved({channel, start, entries.first().stop, start.addSecs(1900), true});
    QVERIFY(harness.appController->m_observedCatchupSession.isEmpty());
    EpgEntry later = entries.last();
    later.start = start.addSecs(1800);
    later.stop = start.addSecs(2400);
    later.title = QStringLiteral("Later EPG");
    entries.append(later);
    harness.epgService->loadFromEntries(entries);
    player->syncCatchupTimelineState();
    QCOMPARE(player->catchupProgramLabel(), later.title);
    player->returnToLiveFromCatchup();
    QCOMPARE(player->currentPlaybackUrl(), channel.streamUrl);
}

void AppModelTests::playerControllerArchiveCrossesProgrammesWithOneRequest_data()
{
    QTest::addColumn<bool>("xtream");
    QTest::newRow("xtream") << true;
    QTest::newRow("m3u") << false;
}

void AppModelTests::playerControllerArchiveCrossesProgrammesWithOneRequest()
{
    QFETCH(bool, xtream);
    const auto ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) { QSKIP("ffmpeg needed for the audio/video archive fixture."); }
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("archive.ts"));
    QProcess generator;
    generator.start(ffmpeg, {
        QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("testsrc2=size=64x48:rate=10:duration=20"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=frequency=440:duration=20"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-preset"), QStringLiteral("ultrafast"),
        QStringLiteral("-g"), QStringLiteral("10"), QStringLiteral("-bf"), QStringLiteral("0"),
        QStringLiteral("-c:a"), QStringLiteral("aac"), path
    });
    QVERIFY(generator.waitForFinished(15000));
    QVERIFY2(generator.exitCode() == 0, generator.readAllStandardError().constData());
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto payload = file.readAll();
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    int requests = 0;
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (auto *socket = server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
                const auto request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n") || socket->property("sent").toBool()) { return; }
                socket->setProperty("sent", true);
                ++requests;
                // Keep HTTP open across both programme boundaries. EPG alone
                // must not close it, reload mpv, or request another URL.
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: "
                    + QByteArray::number(payload.size() + 188) + "\r\n\r\n" + payload);
            });
        }
    });
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    auto *controller = harness.playerController.get();
    const auto headless = qgetenv("OKILTV_HEADLESS_TEST");
    const auto trace = qgetenv("OKILTV_TRACE_MPV");
    const auto restore = qScopeGuard([&]() {
        controller->stop();
        qputenv("OKILTV_HEADLESS_TEST", headless);
        if (trace.isNull()) { qunsetenv("OKILTV_TRACE_MPV"); } else { qputenv("OKILTV_TRACE_MPV", trace); }
    });
    qputenv("OKILTV_HEADLESS_TEST", "0");
    qputenv("OKILTV_TRACE_MPV", "1");
    controller->m_player.configureOptions({{QStringLiteral("vo"), QStringLiteral("null")},
        {QStringLiteral("ao"), QStringLiteral("null")}, {QStringLiteral("hwdec"), QStringLiteral("no")},
        {QStringLiteral("vf"), QStringLiteral("lavfi=[showinfo]")}});
    Channel channel;
    channel.source = xtream ? ChannelSource::Xtream : ChannelSource::M3U;
    channel.profileId = QUuid::createUuid();
    channel.tvgId = QStringLiteral("continuous-fixture");
    channel.catchupSupported = true;
    channel.catchupWindowHours = 24;
    channel.catchupMode = QStringLiteral("append");
    channel.catchupSourceTemplate = QStringLiteral("utc={utc}&lutc={lutc}");
    channel.streamUrl = QStringLiteral("http://127.0.0.1:%1/archive.ts").arg(server.serverPort());
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-3600);
    QList<EpgEntry> entries;
    for (int i = 0; i < 3; ++i) {
        EpgEntry entry;
        entry.channelId = channel.tvgId;
        entry.title = QStringLiteral("Programme %1").arg(i);
        entry.start = start.addSecs(i * 3);
        entry.stop = start.addSecs(i == 2 ? 20 : (i + 1) * 3);
        entries.append(entry);
    }
    harness.epgService->loadFromEntries(entries);
    harness.appController->m_epgLoadedProfileId = channel.profileId;
    const auto url = xtream ? QStringLiteral("http://127.0.0.1:%1/timeshift/user/pass/57/%2/1.ts")
        .arg(server.serverPort()).arg(start.toString(QStringLiteral("yyyy-MM-dd:HH-mm"))) : channel.streamUrl;
    QSignalSpy loaded(controller, &PlayerController::playbackFileLoaded);
    controller->playCatchupChannel(channel, url, entries.first().title, start, entries.first().stop, url,
        std::nullopt, std::nullopt, std::nullopt, 180, true, entries.first());
    auto *backend = controller->playbackPlayer();
    QTRY_VERIFY_WITH_TIMEOUT(backend->position() > 7.0, 12000);
    QCOMPARE(controller->catchupCurrentProgram().value(QStringLiteral("title")).toString(), entries.last().title);
    QVERIFY(backend->propertyDouble("audio-pts").value_or(-1.0) > 5.0);
    const auto frameCursor = DebugLogger::instance().latestCursor();
    const auto audioPosition = backend->propertyDouble("audio-pts").value();
    QTRY_VERIFY_WITH_TIMEOUT(backend->propertyDouble("audio-pts").value_or(-1.0) > audioPosition + 0.5, 3000);
    const auto frames = DebugLogger::instance().entriesSince(frameCursor);
    QVERIFY(std::count_if(frames.cbegin(), frames.cend(), [](const auto &entry) {
        return entry.message.contains(QStringLiteral("showinfo")) && entry.message.contains(QStringLiteral("pts_time:"));
    }) > 2);
    QVERIFY(std::abs(backend->propertyDouble("avsync").value_or(1000.0)) < 0.2);
    QCOMPARE(controller->playbackPlayer(), backend);
    QCOMPARE(loaded.count(), 1);
    QCOMPARE(requests, 1);
    QCOMPARE(controller->playbackMode(), QStringLiteral("catchup"));
}

void AppModelTests::playerControllerContinuousCatchupWaitsAndBoundsRecovery()
{
    PlayerController player;
    Channel channel;
    channel.streamUrl = QStringLiteral("http://127.0.0.1:1/live.ts");
    channel.source = ChannelSource::M3U;
    channel.catchupSupported = true;
    channel.catchupWindowHours = 24;
    channel.catchupMode = QStringLiteral("append");
    channel.catchupSourceTemplate = QStringLiteral("utc={utc}&lutc={lutc}");
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-600);
    player.playCatchupChannel(channel, channel.streamUrl + QStringLiteral("?utc=0"), QStringLiteral("Archive"),
        start, start.addSecs(60), {}, std::nullopt, std::nullopt, std::nullopt, 180, true);
    player.m_positionTimer.stop();
    player.m_catchupSession.m_catchupTimelinePositionSeconds = 420;
    QVERIFY(player.handleCatchupPlaybackEndedRecovery());
    QVERIFY(player.m_catchupSession.publicationWaiting());
    QVERIFY(!player.m_catchupSession.reloadInFlight());
    const auto url = player.currentPlaybackUrl();
    player.updatePosition();
    QCOMPARE(player.currentPlaybackUrl(), url);
    player.m_catchupSession.clearPublicationWait();
    for (int i = 0; i < 3; ++i) {
        player.m_catchupSession.m_catchupContinuousRecoveryTarget = 100;
        QVERIFY(player.reloadCatchupForTimelineSeek(100));
        player.m_catchupSession.cancelReload();
        player.m_catchupSession.reloadAckTimer().stop();
    }
    player.m_catchupSession.m_catchupContinuousRecoveryTarget = 100;
    QVERIFY(!player.reloadCatchupForTimelineSeek(100));
    QVERIFY(player.channelLoadFailed());
    QVERIFY(player.handleCatchupPlaybackEndedRecovery());
    QVERIFY(player.catchupTimelineNoticeText().contains(QStringLiteral("Retry")));
    player.stop();
    QVERIFY(!player.m_catchupSession.publicationWaiting());
}

void AppModelTests::playerControllerEndlessCatchupChangesEpgWithoutRetuning()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    auto *controller = harness.playerController.get();
    Channel channel;
    channel.id = 4699;
    channel.tvgId = QStringLiteral("endless-test");
    channel.streamUrl = QStringLiteral("http://provider.example/live/channel.ts");
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-7200);
    EpgEntry first;
    first.channelId = channel.tvgId;
    first.title = QStringLiteral("First programme");
    first.start = start;
    first.stop = start.addSecs(3600);
    EpgEntry second = first;
    second.title = QStringLiteral("Second programme");
    second.start = first.stop;
    second.stop = start.addSecs(10800);
    harness.epgService->loadFromEntries({first, second});
    controller->playCatchupChannel(channel, QStringLiteral("http://provider.example/archive.ts"), first.title,
        first.start, first.stop, QStringLiteral("http://provider.example/timeshift/user/pass/60/2026-09-12:12-00/1.ts"),
        std::nullopt, std::nullopt, std::nullopt, 180, true);
    const auto url = controller->currentPlaybackUrl();
    const auto options = controller->m_currentLoadfileOptions;
    QVERIFY(!options.contains(QStringLiteral("length=")));
    controller->m_catchupSession.m_catchupTimelinePositionSeconds = 3599;
    controller->syncCatchupTimelineState();
    QCOMPARE(controller->catchupProgramLabel(), first.title);
    QCOMPARE(controller->catchupTimelineStartEpochMs(), first.start.toMSecsSinceEpoch());
    QSignalSpy switchSpy(controller, &PlayerController::channelSwitchInProgressChanged);
    QSignalSpy loadingSpy(controller, &PlayerController::isLoadingChanged);
    controller->m_catchupSession.m_catchupTimelinePositionSeconds = 3601;
    controller->syncCatchupTimelineState();
    QCOMPARE(controller->catchupProgramLabel(), second.title);
    QCOMPARE(controller->catchupTimelineStartEpochMs(), second.start.toMSecsSinceEpoch());
    QCOMPARE(controller->catchupTimelinePositionSeconds(), 1.0);
    QCOMPARE(controller->catchupCurrentProgram().value(QStringLiteral("title")).toString(), second.title);
    QVERIFY(std::abs(controller->catchupCurrentProgram().value(QStringLiteral("progressPercent")).toDouble()
        - 100.0 / 7200.0) < 0.001);
    QCOMPARE(controller->m_catchupSession.m_catchupProgramStartUtc, first.start);
    QCOMPARE(controller->currentPlaybackUrl(), url);
    QCOMPARE(controller->m_currentLoadfileOptions, options);
    QVERIFY(!controller->maybeStopCatchupAtProgrammeBoundary(3601.0, 0.0, QStringLiteral("endless-test")));
    QVERIFY(controller->currentChannelValue().has_value());
    QCOMPARE(switchSpy.count(), 0);
    QCOMPARE(loadingSpy.count(), 0);
    controller->m_catchupSession.m_catchupTimelinePositionSeconds = 3500;
    controller->syncCatchupTimelineState();
    QCOMPARE(controller->catchupProgramLabel(), first.title);
    controller->m_catchupSession.m_catchupTimelinePositionSeconds = 3601;
    controller->syncCatchupTimelineState();
    QCOMPARE(controller->catchupProgramLabel(), second.title);
    QCOMPARE(controller->catchupTimelineEndEpochMs(), second.stop.toMSecsSinceEpoch());
    QCOMPARE(controller->catchupTimelineDurationSeconds(), 7200.0);
    controller->seekTimeshiftToFraction(0.25);
    QVERIFY(std::abs(controller->m_catchupSession.m_catchupTimelinePositionSeconds - 5400.0) < 1.0);
    QVERIFY(controller->m_catchupSession.m_reloadBase >= 5400.0);
    harness.epgService->clear();
    controller->syncCatchupTimelineState();
    QCOMPARE(controller->catchupProgramLabel(), QStringLiteral("Catch-up"));
    QVERIFY(controller->catchupCurrentProgram().isEmpty());
    QVERIFY(std::abs(controller->catchupTimelineEndEpochMs()
        - QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()) < 1000);
    controller->returnToLiveFromCatchup();
    QVERIFY(!controller->m_catchupSession.m_catchupEndless);
    QCOMPARE(controller->currentPlaybackUrl(), channel.streamUrl);
}

void AppModelTests::playerControllerCatchupRefillsWithoutReplacingTransport()
{
    PlayerController controller;
    Channel channel;
    channel.id = 4609;
    channel.streamUrl = QStringLiteral("http://provider.example/live/4609.ts");
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-1200);
    controller.playCatchupChannel(channel, QStringLiteral("http://provider.example/archive.ts"),
                                 QStringLiteral("Refill"), start, start.addSecs(2400));
    auto session = OKILTV::Player::CatchupStreamSession::create(QStringLiteral("http://provider.example/archive.ts"));
    session->configureContinuous({QStringLiteral("http://provider.example/archive.ts"), start, start.addSecs(2400)});
    controller.m_catchupActiveStreamSession = session;
    controller.m_isPlaying = true;
    controller.m_channelSwitchInProgress = false;
    const auto url = controller.currentPlaybackUrl();

    QVERIFY(!controller.evaluateCatchupRebuffering(std::nullopt, false));
    for (const auto cache : { 7.0, 1.1, 1.0, 0.56, 0.11 }) {
        QVERIFY(!controller.evaluateCatchupRebuffering(cache, false));
        QVERIFY(!controller.m_buffering.catchupRefilling());
    }
    QVERIFY(controller.evaluateCatchupRebuffering(0.1, false));
    controller.refreshBufferingState();
    QVERIFY(controller.isBuffering());
    QVERIFY(controller.evaluateCatchupRebuffering(5.0, false));
    QVERIFY(controller.evaluateCatchupRebuffering(9.9, false));
    QVERIFY(!controller.evaluateCatchupRebuffering(10.0, false));
    QCOMPARE(controller.currentPlaybackUrl(), url);
    QVERIFY(!session->closeRequestedByApp());

    QVERIFY(controller.evaluateCatchupRebuffering(0.0, false));
    QVERIFY(!controller.evaluateCatchupRebuffering(2.0, true)); // Drain short EOF tail.
    QVERIFY(controller.evaluateCatchupRebuffering(0.05, false));
    controller.togglePause();
    QVERIFY(controller.m_userPausedManually);
    QVERIFY(!controller.evaluateCatchupRebuffering(20.0, false));
    QVERIFY(controller.m_userPausedManually);
    controller.m_userPausedManually = false;
    QVERIFY(controller.evaluateCatchupRebuffering(0.05, false));
    QVERIFY(controller.seekCatchupToTimelinePosition(300.0));
    QVERIFY(!controller.m_buffering.catchupRefilling());
    QVERIFY(controller.evaluateCatchupRebuffering(0.05, false));
    session->closeProviderConnection(QStringLiteral("test-error-or-teardown"));
    QVERIFY(!controller.evaluateCatchupRebuffering(0.05, false));
    controller.stop();
    QVERIFY(!controller.m_buffering.catchupRefilling());
}

void AppModelTests::playerControllerCatchupRefillTimeoutRequiresPlayableReserve()
{
    PlayerController controller;
    Channel channel;
    channel.id = 4610;
    channel.streamUrl = QStringLiteral("http://provider.example/live/4610.ts");
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-1200);
    controller.playCatchupChannel(channel, QStringLiteral("http://provider.example/archive.ts"),
                                 QStringLiteral("Refill timeout"), start, start.addSecs(2400));
    controller.m_positionTimer.stop();
    auto session = OKILTV::Player::CatchupStreamSession::create(QStringLiteral("http://provider.example/archive.ts"));
    session->configureContinuous({QStringLiteral("http://provider.example/archive.ts"), start, start.addSecs(2400)});
    controller.m_catchupActiveStreamSession = session;
    controller.m_isPlaying = true;
    controller.m_channelSwitchInProgress = false;
    QVERIFY(controller.evaluateCatchupRebuffering(0.1, false));
    QTest::qWait(30010);
    QVERIFY(controller.evaluateCatchupRebuffering(0.0, false));
    QVERIFY(controller.evaluateCatchupRebuffering(0.36, false));
    QVERIFY(!controller.evaluateCatchupRebuffering(2.0, false)); // Byte cap can still release a usable tail.
    QVERIFY(!session->closeRequestedByApp());
}

void AppModelTests::playerControllerPlaysAcrossMpegTsConfigurationChange_data()
{
    QTest::addColumn<bool>("live");
    QTest::addColumn<bool>("failedBoundary");
    QTest::newRow("catchup") << false << false;
    QTest::newRow("live") << true << false;
    QTest::newRow("live-boundary-unavailable") << true << true;
}

void AppModelTests::playerControllerPlaysAcrossMpegTsConfigurationChange()
{
    QFETCH(bool, live);
    QFETCH(bool, failedBoundary);
    if (failedBoundary && qEnvironmentVariableIsSet("OKILTV_CATCHUP_PERIOD_TEST_URL")) {
        QSKIP("Unavailable-boundary injection uses local fixtures only.");
    }
    const auto previousHeadless = qgetenv("OKILTV_HEADLESS_TEST");
    const auto previousTrace = qgetenv("OKILTV_TRACE_MPV");
    const auto restore = qScopeGuard([&]() {
        if (previousHeadless.isNull()) { qunsetenv("OKILTV_HEADLESS_TEST"); }
        else { qputenv("OKILTV_HEADLESS_TEST", previousHeadless); }
        if (previousTrace.isNull()) { qunsetenv("OKILTV_TRACE_MPV"); }
        else { qputenv("OKILTV_TRACE_MPV", previousTrace); }
    });
    qputenv("OKILTV_HEADLESS_TEST", "0");
    qputenv("OKILTV_TRACE_MPV", "1");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QByteArray source;
    const auto clockCapture = qEnvironmentVariable("OKILTV_CATCHUP_CLOCK_FIXTURE");
    if (failedBoundary && !clockCapture.isEmpty()) { QSKIP("Clock fixture has no configuration boundary."); }
    const auto capture = clockCapture.isEmpty() ? qEnvironmentVariable("OKILTV_CATCHUP_PERIOD_FIXTURE") : clockCapture;
    if (!capture.isEmpty()) {
        QFile file(capture);
        QVERIFY(file.open(QIODevice::ReadOnly));
        source = file.readAll();
    } else {
        const auto ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty()) { QSKIP("ffmpeg needed to generate the codec-switch fixture."); }
        for (int index = 0; index < (live ? 3 : 2); ++index) {
            const bool alternate = index == 1;
            const auto path = directory.filePath(QStringLiteral("%1.ts").arg(index));
            QProcess generator;
            generator.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-y"),
                QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), live && alternate ? QStringLiteral("color=size=320x180:rate=25") : QStringLiteral("color=size=160x90:rate=25"),
                QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=frequency=440:sample_rate=48000"),
                QStringLiteral("-t"), QStringLiteral("6"), QStringLiteral("-c:v"), live && alternate ? QStringLiteral("mpeg2video") : QStringLiteral("libx264"),
                QStringLiteral("-preset"), QStringLiteral("ultrafast"), QStringLiteral("-g"), QStringLiteral("25"),
                QStringLiteral("-c:a"), !alternate ? QStringLiteral("aac") : QStringLiteral("mp2"),
                QStringLiteral("-mpegts_service_id"), QString::number(!alternate ? 1 : 15821),
                QStringLiteral("-mpegts_start_pid"), QString::number(!alternate ? 256 : 101),
                QStringLiteral("-mpegts_pmt_start_pid"), QString::number(!alternate ? 4096 : 4095),
                QStringLiteral("-output_ts_offset"), QString::number(!alternate ? 0 : 44000), path});
            QVERIFY(generator.waitForFinished(15000));
            QCOMPARE(generator.exitCode(), 0);
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            source += file.readAll();
        }
    }
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    int requests = 0;
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (auto *socket = server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
                const auto request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n") || socket->property("sent").toBool()) { return; }
                socket->setProperty("sent", true);
                ++requests;
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(source.size())
                    + "\r\nConnection: close\r\n\r\n" + source);
                socket->disconnectFromHost();
            });
        }
    });
    const auto logCursor = DebugLogger::instance().latestCursor();
    const auto diagnostics = qScopeGuard([&]() {
        if (QTest::currentTestFailed()) {
            for (const auto &entry : DebugLogger::instance().entriesSince(logCursor)) {
                qInfo().noquote() << entry.line;
            }
        }
    });
    PlayerController controller;
    controller.applySettings({}, {{QStringLiteral("vo"), QStringLiteral("null")},
        {QStringLiteral("ao"), QStringLiteral("null")}, {QStringLiteral("hwdec"), QStringLiteral("no")},
        {QStringLiteral("speed"), qEnvironmentVariableIsSet("OKILTV_CATCHUP_PERIOD_TEST_URL") ? QStringLiteral("1") : QStringLiteral("2")},
        {QStringLiteral("vf"), QStringLiteral("lavfi=[showinfo]")}}, 5.0, false, 3.0, {});
    Channel channel;
    channel.id = 455;
    channel.streamUrl = QStringLiteral("http://provider.example/live/455.ts");
    const auto localUrl = QStringLiteral("http://127.0.0.1:%1/timeshift/test/test/56/2026-09-17:03-59/455.ts").arg(server.serverPort());
    const auto testUrl = qEnvironmentVariable("OKILTV_CATCHUP_PERIOD_TEST_URL", localUrl);
    const auto base = testUrl == localUrl ? 0.0 : qEnvironmentVariable("OKILTV_CATCHUP_PERIOD_TEST_BASE", QStringLiteral("0")).toDouble();
    const auto start = QDateTime::fromString(QStringLiteral("2026-09-17T03:55:00Z"), Qt::ISODate);
    if (live) {
        channel.streamUrl = testUrl;
        controller.playChannel(channel);
    } else {
        controller.playCatchupChannel(channel, testUrl, QStringLiteral("Configuration transition"),
            start, start.addSecs(3300), testUrl, std::nullopt, base, base);
    }
    auto *player = controller.playbackPlayer();
    QSignalSpy errors(player, &OKILTV::Player::MpvPlayer::errorOccurred);
    const auto activeSession = [&]() { return live ? player->m_liveStream : controller.m_catchupActiveStreamSession; };
    QTRY_VERIFY_WITH_TIMEOUT(activeSession() != nullptr, 5000);
    const auto session = activeSession();
    const auto stopPlayback = qScopeGuard([&]() {
        controller.stop();
        QTest::qWait(200);
    });
    QTimer providerProgress;
    if (testUrl != localUrl) {
        connect(&providerProgress, &QTimer::timeout, &controller, [&]() {
            qInfo() << "Provider probe: timeline=" << controller.catchupTimelinePositionSeconds()
                    << "generation=" << session->readGeneration()
                    << "codec=" << player->audioCodec().value_or(QString {})
                    << "sameSession=" << (activeSession() == session);
        });
        providerProgress.start(30000);
    }
    if (!clockCapture.isEmpty()) {
        QTRY_VERIFY_WITH_TIMEOUT(player->position() > 20.0 || activeSession() != session, 30000);
        QCOMPARE(activeSession(), session);
        QCOMPARE(session->readGeneration(), quint64(0));
        const auto before = player->propertyDouble("audio-pts").value_or(-1.0);
        QVERIFY(before > 0.0);
        const auto cursor = DebugLogger::instance().latestCursor();
        QTRY_VERIFY_WITH_TIMEOUT(player->propertyDouble("audio-pts").value_or(-1.0) > before + 0.5, 3000);
        const auto frames = DebugLogger::instance().entriesSince(cursor);
        QVERIFY(std::count_if(frames.cbegin(), frames.cend(), [](const auto &entry) {
            return entry.message.contains(QStringLiteral("showinfo")) && entry.message.contains(QStringLiteral("pts_time:"));
        }) > 2);
        QVERIFY(std::abs(player->propertyDouble("avsync").value_or(1000.0)) < 0.2);
        QVERIFY(!session->hasNetworkError());
        QVERIFY(!controller.m_catchupSession.m_catchupContinuousFallback);
        QCOMPARE(errors.count(), 0);
        if (testUrl == localUrl) { QCOMPARE(requests, 1); }
        qInfo() << "Isolated clock fixture: audio and video advancing after 20s; same HTTP session, no reload.";
        return;
    }
    if (testUrl == localUrl) {
        QTRY_VERIFY_WITH_TIMEOUT(session->nextPeriodBaseSeconds().has_value(), capture.isEmpty() ? 5000 : 40000);
        controller.togglePause();
        QTRY_VERIFY_WITH_TIMEOUT(player->pauseState().value_or(false), 2000);
        QTest::qWait(150);
        QCOMPARE(session->readGeneration(), quint64(0));
        controller.togglePause();
    }
    if (failedBoundary) {
        QTRY_VERIFY_WITH_TIMEOUT(session->nextPeriodBaseSeconds().has_value(), 10000);
        session->closeProviderConnection(QStringLiteral("test-unavailable-boundary"));
        QVERIFY(player->advanceLiveMediaPeriod());
        QCOMPARE(errors.count(), 1);
        QVERIFY(player->m_liveStream == nullptr);
        QVERIFY(!player->m_livePeriodTimer.isActive());
        QVERIFY(controller.m_recovery.active());
        return;
    }
    QTRY_VERIFY_WITH_TIMEOUT(session->readGeneration() == 1 || activeSession() != session,
                            testUrl == localUrl ? 40000 : 420000);
    QCOMPARE(activeSession(), session);
    QCOMPARE(session->readGeneration(), quint64(1));
    QTRY_COMPARE_WITH_TIMEOUT(player->audioCodec().value_or(QString {}), QStringLiteral("mp2"), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(player->propertyDouble("audio-pts").value_or(-1.0) > 0.0, 5000);
    const auto audioPosition = player->propertyDouble("audio-pts").value();
    const auto frameCursor = DebugLogger::instance().latestCursor();
    const auto framesSinceCutover = [&]() {
        const auto entries = DebugLogger::instance().entriesSince(frameCursor);
        return std::count_if(entries.cbegin(), entries.cend(), [](const auto &entry) {
            return entry.message.contains(QStringLiteral("showinfo")) && entry.message.contains(QStringLiteral("pts_time:"));
        });
    };
    QTRY_VERIFY_WITH_TIMEOUT(player->propertyDouble("audio-pts").value_or(audioPosition) > audioPosition + 0.3, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(framesSinceCutover() > 2, 3000);
    QVERIFY(std::abs(player->propertyDouble("avsync").value_or(1000.0)) < 0.2);
    QCOMPARE(activeSession(), session);
    QVERIFY(!session->hasNetworkError());
    QVERIFY(!controller.m_catchupSession.m_catchupContinuousFallback);
    if (!live) {
        QVERIFY(controller.catchupTimelinePositionSeconds() >= base + (capture.isEmpty() && testUrl == localUrl ? 6.0 : 48.0));
    }
    if (live && capture.isEmpty() && testUrl == localUrl) {
        QTRY_COMPARE_WITH_TIMEOUT(player->videoCodec().value_or(QString {}), QStringLiteral("mpeg2video"), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(session->readGeneration() == 2 || activeSession() != session, 10000);
        QCOMPARE(activeSession(), session);
        QCOMPARE(session->readGeneration(), quint64(2));
        QTRY_COMPARE_WITH_TIMEOUT(player->audioCodec().value_or(QString {}), QStringLiteral("aac"), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(player->videoCodec().value_or(QString {}), QStringLiteral("h264"), 3000);
        QVERIFY(!controller.m_recovery.active());
    }
    if (testUrl == localUrl) { QCOMPARE(requests, 1); }
    QCOMPARE(errors.count(), 0);
    qInfo() << "Period cutover verified: timeline=" << controller.catchupTimelinePositionSeconds()
            << "audio=" << player->audioCodec().value_or(QString {})
            << "avsync=" << player->propertyDouble("avsync").value_or(1000.0)
            << "newVideoFrames=" << framesSinceCutover() << "sameSession=true";
    controller.stop();
    QVERIFY(session->closeRequestedByApp());
    QVERIFY(!player->m_livePeriodTimer.isActive());
}

void AppModelTests::playerControllerSkipsRetriedArchiveGap_data()
{
    QTest::addColumn<bool>("finite");
    QTest::addColumn<bool>("endless");
    QTest::addColumn<bool>("shortResumeTail");
    QTest::newRow("continuous") << false << false << false;
    QTest::newRow("finite") << true << false << false;
    QTest::newRow("continuous-endless") << false << true << false;
    QTest::newRow("finite-endless") << true << true << false;
    QTest::newRow("endless-short-resume-tail") << false << true << true;
}

void AppModelTests::playerControllerSkipsRetriedArchiveGap()
{
    QFETCH(bool, finite);
    QFETCH(bool, endless);
    QFETCH(bool, shortResumeTail);
    const auto previousHeadless = qgetenv("OKILTV_HEADLESS_TEST");
    const auto previousTrace = qgetenv("OKILTV_TRACE_MPV");
    const auto previousContinuous = qgetenv("OKILTV_DISABLE_CATCHUP_CONTINUOUS");
    const auto restore = qScopeGuard([&]() {
        if (previousHeadless.isNull()) { qunsetenv("OKILTV_HEADLESS_TEST"); }
        else { qputenv("OKILTV_HEADLESS_TEST", previousHeadless); }
        if (previousTrace.isNull()) { qunsetenv("OKILTV_TRACE_MPV"); }
        else { qputenv("OKILTV_TRACE_MPV", previousTrace); }
        if (previousContinuous.isNull()) { qunsetenv("OKILTV_DISABLE_CATCHUP_CONTINUOUS"); }
        else { qputenv("OKILTV_DISABLE_CATCHUP_CONTINUOUS", previousContinuous); }
    });
    qputenv("OKILTV_HEADLESS_TEST", "0");
    qputenv("OKILTV_TRACE_MPV", "1");
    qputenv("OKILTV_DISABLE_CATCHUP_CONTINUOUS", finite ? "1" : "0");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QByteArray source;
    const auto capture = shortResumeTail ? QString {} : qEnvironmentVariable("OKILTV_TEST_FORWARD_GAP_CAPTURE");
    if (!capture.isEmpty()) {
        QFile file(capture);
        QVERIFY(file.open(QIODevice::ReadOnly));
        source = file.readAll();
    } else {
        const auto ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty()) { QSKIP("ffmpeg needed to generate archive gap fixture."); }
        for (int index = 0; index < 2; ++index) {
            const auto path = directory.filePath(QStringLiteral("%1.ts").arg(index));
            QProcess generator;
            generator.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-y"),
                QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("color=size=160x90:rate=25"),
                QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=frequency=440:sample_rate=48000"),
                QStringLiteral("-t"), index == 0 ? QStringLiteral("4") : QStringLiteral("8"),
                QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-bf"), QStringLiteral("2"),
                QStringLiteral("-g"), QStringLiteral("25"), QStringLiteral("-c:a"), QStringLiteral("mp2"),
                QStringLiteral("-output_ts_offset"), index == 0 ? QStringLiteral("0") : QStringLiteral("683.2"), path});
            QVERIFY(generator.waitForFinished(15000));
            QCOMPARE(generator.exitCode(), 0);
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            source += file.readAll();
        }
    }
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    int requests = 0;
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (auto *socket = server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
                const auto request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n") || socket->property("sent").toBool()) { return; }
                socket->setProperty("sent", true);
                ++requests;
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(source.size())
                    + "\r\nConnection: close\r\n\r\n" + source);
                socket->disconnectFromHost();
            });
        }
    });
    const auto logCursor = DebugLogger::instance().latestCursor();
    const auto diagnostics = qScopeGuard([&]() {
        if (QTest::currentTestFailed()) {
            for (const auto &entry : DebugLogger::instance().entriesSince(logCursor)) { qInfo().noquote() << entry.line; }
        }
    });
    PlayerController controller;
    controller.applySettings({}, {{QStringLiteral("vo"), QStringLiteral("null")},
        {QStringLiteral("ao"), QStringLiteral("null")}, {QStringLiteral("hwdec"), QStringLiteral("no")},
        {QStringLiteral("vf"), QStringLiteral("lavfi=[showinfo]")}}, 5.0, false, 3.0, {});
    Channel channel;
    channel.id = 454;
    channel.streamUrl = QStringLiteral("http://provider.example/live/454.ts");
    const auto url = QStringLiteral("http://127.0.0.1:%1/timeshift/test/test/20/2026-09-17:19-23/454.ts").arg(server.serverPort());
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-3600);
    bool resumePointInjected = false;
    connect(controller.playbackPlayer(), &OKILTV::Player::MpvPlayer::fileLoaded, &controller, [&]() {
        if (!shortResumeTail || resumePointInjected || requests != 2
            || !controller.m_catchupActiveStreamSession
            || controller.m_catchupActiveStreamSession->readGeneration() != 0) { return; }
        resumePointInjected = true;
        // Reproduce an unavailable resume point at the gap: the old four-
        // second period cannot reach this point or supply its seek reserve.
        // Pin this point independently of timer/decoder scheduling on the host.
        controller.m_catchupSession.m_catchupTimelinePositionSeconds = 10.0;
        controller.m_catchupSession.m_catchupReconnectResumeStreamRelativeSeconds = 10.0;
        controller.m_catchupSession.setAlignmentActive(true);
        controller.m_catchupSession.resetAlignmentSeek();
        controller.playbackPlayer()->setPaused(true);
    });
    controller.playCatchupChannel(channel, url, QStringLiteral("Archive gap"), start, start.addSecs(1200), url,
                                 std::nullopt, std::nullopt, std::nullopt, 180, endless);
    const auto stop = qScopeGuard([&]() { controller.stop(); QTest::qWait(200); });
    QTRY_VERIFY_WITH_TIMEOUT(controller.catchupTimelinePositionSeconds() > 680.0 || requests > 2, 30000);
    QCOMPARE(requests, 2);
    QCOMPARE(resumePointInjected, shortResumeTail);
    const auto session = controller.m_catchupActiveStreamSession;
    QVERIFY(session);
    QCOMPARE(session->readGeneration(), quint64(1));
    QVERIFY(!session->hasNetworkError());
    QVERIFY(!controller.m_catchupSession.alignmentActive());
    QVERIFY(!controller.m_catchupSession.m_catchupReconnectResumeStreamRelativeSeconds);
    QVERIFY(!controller.m_catchupSession.m_catchupContinuousRecoveryTarget);
    auto *player = controller.playbackPlayer();
    QTRY_VERIFY_WITH_TIMEOUT(player->propertyDouble("audio-pts").value_or(-1.0) > 0.0, 5000);
    const auto position = controller.catchupTimelinePositionSeconds();
    const auto audio = player->propertyDouble("audio-pts").value_or(-1.0);
    QVERIFY(audio >= 0);
    const auto cursor = DebugLogger::instance().latestCursor();
    QTRY_VERIFY_WITH_TIMEOUT(controller.catchupTimelinePositionSeconds() > position + 0.5, 3000);
    QVERIFY(player->propertyDouble("audio-pts").value_or(-1.0) > audio);
    const auto frames = DebugLogger::instance().entriesSince(cursor);
    QVERIFY(std::count_if(frames.cbegin(), frames.cend(), [](const auto &entry) {
        return entry.message.contains(QStringLiteral("showinfo")) && entry.message.contains(QStringLiteral("pts_time:"));
    }) > 2);
    QVERIFY(std::abs(player->propertyDouble("avsync").value_or(1000.0)) < 0.2);
    QCOMPARE(controller.m_catchupActiveStreamSession, session);
    QCOMPARE(requests, 2);
}

void AppModelTests::playerControllerRecoversFailedContinuousAtWatchedPosition_data()
{
    QTest::addColumn<bool>("finitePeriods");
    QTest::newRow("continuous") << false;
    QTest::newRow("finite-period-recovery") << true;
}

void AppModelTests::playerControllerStandbyTimeoutLeavesWaitLoop()
{
    PlayerController controller;
    Channel channel;
    channel.id = 4613;
    channel.streamUrl = QStringLiteral("http://provider.example/live/4613.ts");
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-3600);
    const auto url = QStringLiteral("http://provider.example/timeshift/test/test/60/2026-09-17:19-23/4613.ts");
    controller.playCatchupChannel(channel, url, QStringLiteral("Timeout recovery"), start,
        start.addSecs(7200), url, std::nullopt, std::nullopt, std::nullopt, 180, true);
    controller.m_positionTimer.stop();
    controller.m_catchupSession.m_catchupTimelinePositionSeconds = 54.0;
    controller.m_catchupSession.m_catchupContinuousFallback = true;
    controller.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Armed;
    controller.m_catchupSession.standby().m_phase = OKILTV::App::Playback::CatchupStandbyTransition::Phase::Loading;
    controller.m_catchupSession.standby().m_ready = true;
    controller.m_catchupSession.standby().m_videoReady = true;
    controller.m_catchupSeamlessStandbyPlayer = &controller.m_catchupStandbyPlayer;
    controller.m_catchupStandbyPlayer.m_cachedTelemetry.positionSeconds = 0.0;
    controller.m_catchupStandbyPlayer.m_cachedTelemetry.demuxerCacheDurationSeconds = 0.0;
    QVERIFY(controller.handleCatchupPlaybackEndedRecovery());
    QVERIFY(controller.m_catchupSession.standby().fallbackDeferred());
    QVERIFY(controller.m_catchupSession.standby().fallbackTimer().isActive());
    QVERIFY(QMetaObject::invokeMethod(&controller.m_catchupSession.standby().fallbackTimer(), "timeout", Qt::DirectConnection));
    QVERIFY(!controller.m_catchupSession.standby().pending());
    QVERIFY(!controller.m_catchupSession.standby().fallbackDeferred());
    QVERIFY(!controller.m_catchupSession.standby().fallbackTimer().isActive());
    QVERIFY(controller.m_catchupSession.reloadInFlight());
    controller.stop();
}

void AppModelTests::playerControllerRecoversFailedContinuousAtWatchedPosition()
{
    QFETCH(bool, finitePeriods);
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server, [&server]() {
        while (auto *socket = server.nextPendingConnection()) {
            const QByteArray body(1880, 'x'); // Corrupt transport: must not be passed to the decoder.
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: 1880\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        }
    });
    const auto url = QStringLiteral("http://127.0.0.1:%1/timeshift/test/test/20/2026-09-13:12-00/1.ts").arg(server.serverPort());
    PlayerController controller;
    Channel channel;
    channel.id = 4611;
    channel.streamUrl = QStringLiteral("http://provider.example/live/4611.ts");
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-1200);
    controller.playCatchupChannel(channel, url, QStringLiteral("Recovery"), start, start.addSecs(2400), url);
    controller.m_positionTimer.stop();
    auto session = OKILTV::Player::CatchupStreamSession::create(url);
    if (finitePeriods) {
        session->configureMediaPeriods(0.0);
    } else {
        session->configureContinuous({url, start, start.addSecs(2400)});
    }
    QVERIFY(session->start());
    QTRY_VERIFY_WITH_TIMEOUT(session->failedMediaTransport(), 3000);
    controller.m_catchupActiveStreamSession = session;
    auto *player = controller.playbackPlayer();
    player->m_cachedTelemetry.positionSeconds = 152.0;
    player->m_cachedTelemetry.demuxerCacheDurationSeconds = 13.87;
    QVERIFY(!controller.recoverFailedContinuousCatchup());
    QVERIFY(!controller.canUseSeamlessCatchupRolling());
    QVERIFY(!controller.m_catchupSession.reloadInFlight());
    player->m_cachedTelemetry.demuxerCacheDurationSeconds = 0.5;
    QVERIFY(controller.recoverFailedContinuousCatchup());
    QVERIFY(controller.m_catchupSession.reloadInFlight());
    QCOMPARE(controller.m_catchupSession.m_reloadBase, 120.0);
    QVERIFY(!controller.m_catchupSession.standby().pending());
    controller.runCatchupTimelineReload();
    QVERIFY(!controller.m_catchupSession.m_catchupReconnectResumeStreamRelativeSeconds.has_value());
    QCOMPARE(controller.m_catchupSession.m_catchupTimelinePositionSeconds, 120.0);
    player->fileLoaded();
    QVERIFY(!controller.m_catchupSession.alignmentActive());
    QVERIFY(!controller.advanceCatchupRecoveryAlignment());
    QVERIFY(controller.m_catchupSession.m_catchupContinuousFallback);
    controller.stop();
}

void AppModelTests::playerControllerRecoveryWaitsForCachedResumePoint()
{
    PlayerController controller;
    Channel channel;
    channel.id = 4612;
    channel.streamUrl = QStringLiteral("http://provider.example/live/4612.ts");
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-1200);
    controller.playCatchupChannel(channel, QStringLiteral("http://provider.example/archive.ts"),
                                 QStringLiteral("Paced recovery"), start, start.addSecs(2400));
    controller.m_positionTimer.stop();
    controller.m_catchupSession.m_catchupContinuousFallback = true;
    controller.m_catchupSession.m_catchupTimelinePositionSeconds = 643.389;
    controller.m_catchupSession.m_catchupReconnectResumeStreamRelativeSeconds = 43.389;
    auto *player = controller.playbackPlayer();
    player->fileLoaded();
    QVERIFY(controller.m_catchupSession.alignmentActive());
    player->m_cachedTelemetry.positionSeconds = 1.0;
    player->m_cachedTelemetry.demuxerSeekableRangeSeconds = std::make_pair(0.0, 5.0);
    QVERIFY(controller.advanceCatchupRecoveryAlignment());
    QVERIFY(!controller.m_catchupSession.alignmentSeekIssued());
    QCOMPARE(controller.m_catchupSession.m_catchupTimelinePositionSeconds, 643.389);
    player->m_cachedTelemetry.demuxerSeekableRangeSeconds = std::make_pair(0.0, 45.0);
    QVERIFY(controller.advanceCatchupRecoveryAlignment());
    QVERIFY(controller.m_catchupSession.alignmentSeekIssued());
    QVERIFY(controller.advanceCatchupRecoveryAlignment()); // Command alone does not confirm a seek.
    controller.togglePause(); // Manual pause retains ownership of the waiting transport.
    QVERIFY(controller.m_userPausedManually);
    player->m_cachedTelemetry.positionSeconds = 43.4;
    QVERIFY(!controller.advanceCatchupRecoveryAlignment());
    QVERIFY(!controller.m_catchupSession.alignmentActive());
    QVERIFY(controller.m_userPausedManually);
    QVERIFY(!controller.m_catchupSession.m_catchupReconnectResumeStreamRelativeSeconds.has_value());
    controller.stop();
}

void AppModelTests::playerControllerCatchupTimelineShowsEpgEndAndReturnsLiveFromFuture()
{
    PlayerController playerController;

    Channel channel;
    channel.id = 4601;
    channel.name = QStringLiteral("Catch-up Timeline Lock Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/4601.ts");

    const auto runningStartUtc = QDateTime::currentDateTimeUtc().addSecs(-40 * 60);
    const auto runningStopUtc = QDateTime::currentDateTimeUtc().addSecs(20 * 60);
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup4601"),
        QStringLiteral("Running Show"),
        runningStartUtc,
        runningStopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/4601.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));

    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 0.0;
    playerController.seekTimeshiftToFraction(38.0 / 60.0);
    QCOMPARE(playerController.m_catchupSession.m_catchupTimelinePositionSeconds, 0.0);
    QVERIFY(playerController.catchupTimelineNoticeText().contains(QStringLiteral("3 minutes")));
    QVERIFY(!playerController.m_catchupSession.reloadInFlight());
    QCOMPARE(playerController.catchupTimelineEndEpochMs(), runningStopUtc.toMSecsSinceEpoch());
    QVERIFY(std::abs(playerController.catchupTimelineDurationSeconds() - 3600.0) < 1.0);
    QVERIFY(playerController.catchupTimelineAvailableEdgeEpochMs()
        <= QDateTime::currentDateTimeUtc().addSecs(-180).toMSecsSinceEpoch());

    playerController.seekTimeshiftRelative(2300.0);
    QCOMPARE(playerController.m_catchupSession.m_catchupTimelinePositionSeconds, 0.0);

    playerController.seekTimeshiftToFraction(0.50);
    QVERIFY(std::abs(playerController.m_catchupSession.m_catchupTimelinePositionSeconds - 1800.0) < 1.0);
    QVERIFY(playerController.catchupTimelineNoticeText().isEmpty());

    // Respect source configuration, not a hardcoded three-minute exclusion.
    playerController.m_catchupSession.m_catchupSafetySeconds = 300;
    QVERIFY(playerController.seekCatchupToTimelinePosition(2050.0));
    QVERIFY(!playerController.seekCatchupToTimelinePosition(2150.0));
    QVERIFY(playerController.catchupTimelineNoticeText().contains(QStringLiteral("5 minutes")));

    // A recently ended programme still displays its full end, while the
    // unpublished tail remains unavailable for seeking.
    playerController.m_catchupSession.m_catchupProgramStopUtc = QDateTime::currentDateTimeUtc().addSecs(-60);
    QCOMPARE(playerController.catchupTimelineEndEpochMs(), playerController.m_catchupSession.m_catchupProgramStopUtc.toMSecsSinceEpoch());
    const auto retainedPosition = playerController.m_catchupSession.m_catchupTimelinePositionSeconds;
    playerController.seekTimeshiftToFraction(1.0);
    QCOMPARE(playerController.m_catchupSession.m_catchupTimelinePositionSeconds, retainedPosition);
    QVERIFY(playerController.catchupTimelineNoticeText().contains(QStringLiteral("5 minutes")));

    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup4602"),
        QStringLiteral("Past Show"),
        QDateTime::currentDateTimeUtc().addSecs(-2 * 3600),
        QDateTime::currentDateTimeUtc().addSecs(-3600),
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:10-00/4601.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    playerController.m_catchupSession.m_catchupTimelinePositionSeconds = 0.0;
    playerController.seekTimeshiftToFraction(0.95);
    QVERIFY(playerController.m_catchupSession.m_catchupTimelinePositionSeconds > 0.0);
    QCOMPARE(playerController.catchupTimelineEndEpochMs(), playerController.m_catchupSession.m_catchupProgramStopUtc.toMSecsSinceEpoch());
    QVERIFY(playerController.catchupTimelineNoticeText().isEmpty());

    // Endless sessions use the watched programme's full EPG end too. A future
    // click must leave the archive and restore the exact saved live URL.
    playerController.playCatchupChannel(
        channel, QStringLiteral("http://127.0.0.1/catchup4603"),
        QStringLiteral("Running Show"), runningStartUtc, runningStopUtc,
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/4601.ts"));
    playerController.m_catchupSession.m_catchupEndless = true;
    EpgEntry runningProgram;
    runningProgram.start = runningStartUtc;
    runningProgram.stop = runningStopUtc;
    runningProgram.title = QStringLiteral("Running Show");
    playerController.updateCatchupProgramme(runningProgram);
    QCOMPARE(playerController.catchupTimelineEndEpochMs(), runningStopUtc.toMSecsSinceEpoch());
    playerController.seekTimeshiftToFraction(0.9);
    QCOMPARE(playerController.playbackMode(), QStringLiteral("live"));
    QCOMPARE(playerController.currentPlaybackUrl(), channel.streamUrl);
    QVERIFY(!playerController.catchupTimelineActive());
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
    QVERIFY(playerController.m_buffering.startupPending());
    QVERIFY(promotedPrimaryPlayer.m_startupBufferingStrictMode);

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
        static_cast<qint64>(8) * 1024 * 1024);
    QCOMPARE(
        OKILTV::Player::MpvPlayer::demuxerMaxBytesForBufferSeconds(10.0),
        static_cast<qint64>(20) * 1024 * 1024);
    QCOMPARE(
        OKILTV::Player::MpvPlayer::demuxerMaxBytesForBufferSeconds(60.0),
        static_cast<qint64>(120) * 1024 * 1024);
}

void AppModelTests::mpvPlayerRenderCallbackUpdatesHeartbeatTimestamp()
{
    OKILTV::Player::MpvPlayer player;
    OKILTV::Player::MpvVideoItem item;
    player.setRenderUpdateTarget(&item);
    QCOMPARE(player.lastRenderUpdateTimestampMs(), qint64(-1));
    player.requestFrameUpdate();
    QVERIFY(player.lastRenderUpdateTimestampMs() >= 0);
    QTRY_COMPARE(item.m_mpvRequestUpdateCount, quint64(1));

    // Simulate mpv notifying from its worker while the GUI replaces a PiP item.
    auto oldItem = std::make_unique<OKILTV::Player::MpvVideoItem>();
    player.setRenderUpdateTarget(oldItem.get());
    std::thread callback([&player]() {
        for (int index = 0; index < 100; ++index) {
            player.requestFrameUpdate();
        }
    });
    callback.join();
    oldItem.reset();
    player.setRenderUpdateTarget(&item);
    QTRY_COMPARE(item.m_mpvRequestUpdateCount, quint64(2));

    // A queued notification with no surviving target is harmless.
    auto closingItem = std::make_unique<OKILTV::Player::MpvVideoItem>();
    player.setRenderUpdateTarget(closingItem.get());
    std::thread closingCallback([&player]() { player.requestFrameUpdate(); });
    closingCallback.join();
    closingItem.reset();
    QTRY_VERIFY(!player.m_frameUpdateQueued.load());
    QCOMPARE(item.m_mpvRequestUpdateCount, quint64(2));

    player.setRenderUpdateTarget(&item);
    auto pendingItem = std::make_unique<OKILTV::Player::MpvVideoItem>();
    std::thread synchronize([&player, target = pendingItem.get()]() {
        player.setRenderUpdateTarget(target);
    });
    synchronize.join();
    QCOMPARE(player.m_updateTarget.data(), static_cast<QObject *>(&item));
    pendingItem.reset();
    QCoreApplication::sendPostedEvents(&player, QEvent::MetaCall);
    QCOMPARE(player.m_updateTarget.data(), static_cast<QObject *>(&item));
}

void AppModelTests::mpvPlayerDefersIdleBackendInitialization()
{
    OKILTV::Player::MpvPlayer player;
    QSignalSpy errors(&player, &OKILTV::Player::MpvPlayer::errorOccurred);
    player.setVolume(37);
    player.setAudioEnabled(false);
    player.stop();
    QVERIFY(player.diagnostics().isEmpty());
    QVERIFY(!player.propertyDouble("volume").has_value());
    QCOMPARE(errors.count(), 0);

    QVERIFY(player.ensureInitialized());
    QCOMPARE(player.propertyDouble("volume"), std::optional<double>(37.0));
    QCOMPARE(player.propertyString("aid").value_or(QString()), QStringLiteral("no"));
    player.setAudioEnabled(true);
    player.setVolume(62);
    QTRY_COMPARE(player.propertyDouble("volume"), std::optional<double>(62.0));
    QTRY_COMPARE(player.propertyString("aid").value_or(QString()), QStringLiteral("auto"));

    player.unload();
    player.stop();
    QVERIFY(!player.propertyDouble("volume").has_value());
    QVERIFY(player.ensureInitialized());
    QCOMPARE(player.propertyDouble("volume"), std::optional<double>(62.0));

    PlayerController controller;
    Channel channel;
    channel.id = 1;
    channel.name = QStringLiteral("Lazy standby test");
    channel.streamUrl = QStringLiteral("http://127.0.0.1/lazy-standby.ts");
    controller.playChannel(channel);
    QVERIFY(controller.m_catchupStandbyPlayer.diagnostics().isEmpty());
    controller.stop();
    QVERIFY(controller.m_catchupStandbyPlayer.diagnostics().isEmpty());
}

void AppModelTests::mpvPlayerInitializationErrorAllowsStateQueries()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QFile invalidLibrary(temp.filePath(QStringLiteral("invalid-mpv-library")));
    QVERIFY(invalidLibrary.open(QIODevice::WriteOnly));
    invalidLibrary.close();
    OKILTV::Player::MpvPlayer player;
    player.configureLibraryPath(invalidLibrary.fileName());
    bool received = false;
    connect(&player, &OKILTV::Player::MpvPlayer::errorOccurred, &player, [&](const QString &message) {
        QVERIFY(!message.isEmpty());
        QCOMPARE(player.position(), -1.0);
        QVERIFY(!player.pauseState().has_value());
        received = true;
    });
    QVERIFY(!player.ensureInitialized());
    QVERIFY(!received);
    QTRY_VERIFY(received);
}

void AppModelTests::mpvPlayerCacheWindowSecondsMapping()
{
    QCOMPARE(OKILTV::Player::MpvPlayer::cacheWindowSecondsForBufferTarget(2.0), 10.0);
    QCOMPARE(OKILTV::Player::MpvPlayer::cacheWindowSecondsForBufferTarget(10.0), 30.0);
    QCOMPARE(OKILTV::Player::MpvPlayer::cacheWindowSecondsForBufferTarget(60.0), 120.0);
}

void AppModelTests::mpvPlayerSteadyStateCacheBandMapping()
{
    QCOMPARE(OKILTV::Player::MpvPlayer::steadyStateBackBufferSeconds(), 30.0);
    QCOMPARE(OKILTV::Player::MpvPlayer::steadyStateCacheLimitSecondsForBufferTarget(2.0), 10.0);
    QCOMPARE(OKILTV::Player::MpvPlayer::steadyStateCacheLimitSecondsForBufferTarget(10.0), 30.0);
    QCOMPARE(OKILTV::Player::MpvPlayer::steadyStateCacheHysteresisSecondsForBufferTarget(2.0), 0.0);
    QCOMPARE(OKILTV::Player::MpvPlayer::steadyStateCacheHysteresisSecondsForBufferTarget(10.0), 0.0);
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
        11.0);
    QCOMPARE(
        PlayerController::adaptiveSteadyStateCacheHysteresisSeconds(3.0),
        0.0);
    QCOMPARE(
        PlayerController::adaptiveSteadyStateMaxBytes(3.0, std::nullopt),
        OKILTV::Player::MpvPlayer::demuxerMaxBytesForBufferSeconds(41.0));
    QCOMPARE(
        PlayerController::adaptiveSteadyStateMaxBackBytes(std::nullopt),
        OKILTV::Player::MpvPlayer::demuxerMaxBytesForBufferSeconds(30.0));
    QCOMPARE(
        PlayerController::adaptiveSteadyStateMaxBytes(3.0, 8.0 * 1000.0 * 1000.0),
        static_cast<qint64>(std::llround((8.0 * 1000.0 * 1000.0 / 8.0) * 41.0 * 1.25)));
    QCOMPARE(
        PlayerController::adaptiveSteadyStateMaxBackBytes(8.0 * 1000.0 * 1000.0),
        static_cast<qint64>(std::llround((8.0 * 1000.0 * 1000.0 / 8.0) * 30.0 * 1.25)));
    QCOMPARE(
        PlayerController::adaptiveCatchupMaxBytes(std::nullopt),
        static_cast<qint64>(96) * 1024 * 1024);
    QCOMPARE(
        PlayerController::adaptiveCatchupMaxBackBytes(std::nullopt),
        static_cast<qint64>(32) * 1024 * 1024);
    QCOMPARE(
        PlayerController::adaptiveCatchupMaxBytes(8.0 * 1000.0 * 1000.0),
        static_cast<qint64>(96) * 1024 * 1024);
    QCOMPARE(
        PlayerController::adaptiveCatchupMaxBackBytes(8.0 * 1000.0 * 1000.0),
        static_cast<qint64>(32) * 1024 * 1024);
    QVERIFY(
        PlayerController::adaptiveSteadyStateMaxBytes(3.0, 160.0 * 1000.0 * 1000.0)
        > static_cast<qint64>(8) * 1024 * 1024);
}

void AppModelTests::playerControllerCatchupCacheBudgetsAreBounded_data()
{
    QTest::addColumn<double>("bitrate");
    QTest::addColumn<qint64>("forwardBytes");
    QTest::addColumn<qint64>("backBytes");
    constexpr qint64 forwardCap = 96LL * 1024 * 1024;
    constexpr qint64 backCap = 32LL * 1024 * 1024;
    QTest::newRow("adaptive-low-bitrate") << 2e6 << qint64(28125000) << qint64(9375000);
    QTest::newRow("high-bitrate") << 25e6 << forwardCap << backCap;
    QTest::newRow("extreme-finite-bitrate") << std::numeric_limits<double>::max() << forwardCap << backCap;
    QTest::newRow("unknown-bitrate") << 0.0 << forwardCap << backCap;
    QTest::newRow("invalid-bitrate") << -1.0 << forwardCap << backCap;
    QTest::newRow("nan-bitrate") << std::numeric_limits<double>::quiet_NaN() << forwardCap << backCap;
    QTest::newRow("infinite-bitrate") << std::numeric_limits<double>::infinity() << forwardCap << backCap;
}

void AppModelTests::playerControllerCatchupCacheBudgetsAreBounded()
{
    QFETCH(double, bitrate);
    QFETCH(qint64, forwardBytes);
    QFETCH(qint64, backBytes);
    QCOMPARE(PlayerController::adaptiveCatchupMaxBytes(bitrate), forwardBytes);
    QCOMPARE(PlayerController::adaptiveCatchupMaxBackBytes(bitrate), backBytes);
}

void AppModelTests::playerControllerReconnectRebuildsReserveOnSameConnection()
{
    PlayerController c;
    c.m_positionTimer.stop();
    c.m_liveDeliveryTimer.stop();
    Channel channel;
    channel.id = 176;
    channel.streamUrl = QStringLiteral("http://127.0.0.1/recovery");
    c.m_currentChannel = channel;
    c.m_recovery.start();
    c.m_recovery.beginLoad(false, 3.0, c.m_liveDeliveryClock.elapsed());
    c.m_recovery.m_reconnectAttemptCount = 1;
    c.m_recovery.loaded(c.m_liveDeliveryClock.elapsed());
    c.m_recovery.m_phase = OKILTV::App::Playback::PlaybackRecovery::Phase::RebuildingReserve;
    c.m_recovery.m_reserveTarget = 7.5;
    c.m_recovery.m_reserveProgressMs = c.m_liveDeliveryClock.elapsed();
    c.m_recovery.m_totalStartedMs = c.m_liveDeliveryClock.elapsed();
    auto &t = c.m_player.m_cachedTelemetry;
    t.pauseState = true;
    t.cacheSpeedBytesPerSecond = 4198.0;
    for (const double cache : {0.0, 1.6, 0.608, 0.0, 0.0, 0.0, 6.72}) {
        t.demuxerCacheDurationSeconds = cache;
        c.updatePosition();
        c.handleReconnectAttemptTick();
        QVERIFY(c.m_recovery.reservePending());
        QVERIFY(!c.m_recovery.stabilizing());
        QCOMPARE(c.m_recovery.attempts(), 1);
        QVERIFY(c.m_recovery.attemptInFlight());
    }
    t.demuxerCacheDurationSeconds = 7.6;
    QVERIFY(!c.advanceReconnectReserve());
    QVERIFY(!c.m_recovery.reservePending());
    QVERIFY(!c.m_player.m_pauseRequested);
    // Another gap rebuilds the reserve without replacing the transport.
    c.m_recovery.m_phase = OKILTV::App::Playback::PlaybackRecovery::Phase::Stabilizing;
    c.m_recovery.m_reconnectRecoveryUnhealthyTickCount = 2;
    t.demuxerCacheDurationSeconds = 0.7;
    QVERIFY(c.advanceReconnectReserve());
    QVERIFY(c.m_player.m_pauseRequested);
    QVERIFY(!c.m_recovery.stabilizing());
    for (int i = 0; i < 4; ++i) {
        c.updatePosition();
        c.handleReconnectAttemptTick();
    }
    QCOMPARE(c.m_recovery.attempts(), 1);
    t.demuxerCacheDurationSeconds = 8.0;
    QVERIFY(!c.advanceReconnectReserve());
    QCOMPARE(c.m_recovery.m_reconnectRecoveryUnhealthyTickCount, 0);
}

void AppModelTests::playerControllerReconnectReserveHonorsStopAndFailure()
{
    for (const bool manualPause : {false, true}) {
        PlayerController c;
        c.m_positionTimer.stop();
        c.m_liveDeliveryTimer.stop();
        c.m_currentChannel = Channel {};
        c.m_recovery.start();
        c.m_recovery.beginLoad(false, 3.0, c.m_liveDeliveryClock.elapsed());
        c.m_recovery.loaded(c.m_liveDeliveryClock.elapsed());
        c.m_recovery.m_phase = OKILTV::App::Playback::PlaybackRecovery::Phase::RebuildingReserve;
        c.m_recovery.m_reserveTarget = 7.5;
        c.m_recovery.m_reserveProgressMs = c.m_liveDeliveryClock.elapsed();
        c.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = 2.0;
        if (manualPause) {
            c.togglePause();
            QVERIFY(!c.m_recovery.active());
            QVERIFY(c.m_userPausedManually);
            QVERIFY(c.m_player.m_pauseRequested);
        } else {
            OKILTV::Player::MpvPlayer::CacheReadState state;
            state.eof = true;
            c.m_player.m_cachedTelemetry.cacheReadState = state;
            QVERIFY(c.advanceReconnectReserve());
            QVERIFY(!c.m_recovery.attemptInFlight());
        }
        QVERIFY(!c.m_recovery.reservePending());
    }
}

void AppModelTests::playerControllerReconnectReserveTimesOutWithoutProgress()
{
    PlayerController c;
    c.m_positionTimer.stop();
    c.m_liveDeliveryTimer.stop();
    c.m_currentChannel = Channel {};
    c.m_recovery.start();
    c.m_recovery.beginLoad(false, 3.0, c.m_liveDeliveryClock.elapsed());
    c.m_recovery.loaded(c.m_liveDeliveryClock.elapsed());
    c.m_recovery.m_phase = OKILTV::App::Playback::PlaybackRecovery::Phase::RebuildingReserve;
    c.m_recovery.m_reserveTarget = 3.0;
    c.m_waitForDataStreamSeconds = 0.1;
    c.m_recovery.m_reserveProgressMs = c.m_liveDeliveryClock.elapsed();
    c.m_player.m_cachedTelemetry.demuxerCacheDurationSeconds = 0.0;
    c.m_player.m_cachedTelemetry.cacheSpeedBytesPerSecond = 4198.0;
    QTest::qWait(10100);
    QVERIFY(c.advanceReconnectReserve());
    QVERIFY(!c.m_recovery.attemptInFlight());
    QVERIFY(!c.m_recovery.reservePending());
}

void AppModelTests::playerControllerLiveReconnectAllowsFullStabilizationWindow()
{
    PlayerController controller;
    controller.m_positionTimer.stop();
    controller.m_liveDeliveryTimer.stop();
    controller.m_waitForDataStreamSeconds = 10.0;
    Channel channel;
    channel.id = 175;
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live175");
    controller.m_currentChannel = channel;
    controller.m_currentPlaybackUrl = channel.streamUrl;
    controller.m_isPlaying = true;
    controller.m_recovery.start();
    controller.m_recovery.beginLoad(false, 3.0, controller.m_liveDeliveryClock.elapsed());
    controller.m_recovery.m_reconnectAttemptCount = 1;
    controller.m_recovery.m_attemptStartedMs = controller.m_liveDeliveryClock.elapsed();
    // Time establishing the connection must not consume the health window.
    QTest::qWait(1100);
    for (int i = 0; i < 12; ++i) {
        auto &telemetry = controller.m_player.m_cachedTelemetry;
        telemetry.pauseState = false;
        telemetry.bufferingState = false;
        telemetry.positionSeconds = static_cast<double>(i + 1);
        telemetry.displayedVideoFramePtsSeconds = static_cast<double>(i + 1);
        // Provider bursts alternate with normal cache consumption below target.
        telemetry.demuxerCacheDurationSeconds = 3.1 - (i % 3) * 0.7;
        telemetry.cacheSpeedBytesPerSecond = i % 3 == 0 ? 1000000.0 : 0.0;
        controller.updatePosition();
        if (i == 0) {
            QVERIFY(controller.m_recovery.stabilizing());
            QVERIFY((controller.m_liveDeliveryClock.elapsed() - controller.m_recovery.m_attemptStartedMs) < 1000);
        }
        if (i < 11) {
            QTest::qWait(1000);
            controller.handleReconnectAttemptTick();
            QVERIFY(controller.m_recovery.attemptInFlight());
            QCOMPARE(controller.m_recovery.attempts(), 1);
        }
    }
    QVERIFY(!controller.m_recovery.active());
    QVERIFY(!controller.m_recovery.attemptInFlight());
}

void AppModelTests::playerControllerLiveWatchdogRespectsTuneGrace()
{
    PlayerController controller;
    controller.m_positionTimer.stop();
    controller.m_liveDeliveryTimer.stop();
    controller.m_waitForDataStreamSeconds = 10.0;
    Channel channel;
    channel.id = 176;
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live176");
    controller.m_currentChannel = channel;
    controller.m_currentPlaybackUrl = channel.streamUrl;
    controller.m_isPlaying = true;
    controller.m_tuneAttemptTimer.start();
    auto &telemetry = controller.m_player.m_cachedTelemetry;
    telemetry.positionSeconds = 0.0;
    telemetry.displayedVideoFramePtsSeconds = 0.0;
    telemetry.demuxerCacheDurationSeconds = 0.0;
    telemetry.cacheSpeedBytesPerSecond = 0.0;
    for (int i = 0; i < 5; ++i) {
        controller.updatePosition();
        QVERIFY(!controller.m_recovery.active());
    }
    // Once startup protection expires, a dead stream must still recover.
    controller.m_tuneAttemptTimer.invalidate();
    for (int i = 0; i < 4; ++i) {
        controller.updatePosition();
    }
    QVERIFY(controller.m_recovery.active());
    QVERIFY(controller.m_recovery.waitingStop());
}

void AppModelTests::timeshiftControllerPreparingUntilPlaybackAttached()
{
    QTemporaryDir tempDir;
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    PlayerController player;
    DvrController dvr(&settings, &player);
    MultiViewController multiview(&settings, nullptr, &player);
    TimeshiftController timeshift(&settings, &player, &dvr, &multiview);
    player.setTimeshiftController(&timeshift);
    timeshift.m_session.emplace();
    auto &session = *timeshift.m_session;
    session.state = TimeshiftController::SessionState::Running;
    QVERIFY(timeshift.isPreparing());
    QVERIFY(!timeshift.isActive());
    QCOMPARE(player.startupPolicyForPlaybackRequest(player.player()), PlayerController::StartupPolicy::StrictBuffered);
    session.playbackAttached = true;
    QVERIFY(!timeshift.isPreparing());
    QVERIFY(timeshift.isActive());
    session.playbackAttached = false;
    session.state = TimeshiftController::SessionState::Failed;
    QVERIFY(!timeshift.isPreparing());
}

void AppModelTests::timeshiftControllerProbeKeepsMetadataWithStderr()
{
#if defined(Q_OS_WIN)
    QSKIP("POSIX process shims are used by this test.");
#else
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const auto toolsDir = tempDir.filePath(QStringLiteral("tools"));
    QVERIFY(QDir().mkpath(toolsDir));
    QVERIFY(writeExecutableTextFile(QDir(toolsDir).filePath(QStringLiteral("ffprobe")), QStringLiteral(
        "#!/bin/sh\n"
        "echo 'non-existing PPS 0 referenced' >&2\n"
        "echo '{\"streams\":[{\"index\":0,\"codec_type\":\"video\",\"codec_name\":\"h264\"},"
        "{\"index\":1,\"codec_type\":\"audio\",\"codec_name\":\"aac\"},"
        "{\"index\":2,\"codec_type\":\"subtitle\",\"codec_name\":\"subrip\"}]}'\n")));
    QVERIFY(writeExecutableTextFile(QDir(toolsDir).filePath(QStringLiteral("ffmpeg")),
        QStringLiteral("#!/bin/sh\nexec sleep 30\n")));
    ScopedPathOverride scopedPath(toolsDir);
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    settings.current().timeshiftEnabled = true;
    settings.current().timeshiftStorageDirectory = tempDir.filePath(QStringLiteral("timeshift"));
    PlayerController player;
    DvrController dvr(&settings, &player);
    MultiViewController multiview(&settings, nullptr, &player);
    TimeshiftController timeshift(&settings, &player, &dvr, &multiview);
    Channel channel;
    channel.id = 177;
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live177");
    player.m_currentChannel = channel;
    QVERIFY(timeshift.startSessionForCurrentChannel(false, QStringLiteral("test")));
    QTRY_VERIFY_WITH_TIMEOUT(timeshift.m_session && timeshift.m_session->probeCompletionHandled, 3000);
    QCOMPARE(timeshift.m_session->audioTrackCount, 1);
    QCOMPARE(timeshift.m_session->subtitleTrackCount, 1);
    QVERIFY(!timeshift.m_session->avMasterPlaylistPath.isEmpty());
    QVERIFY(!timeshift.m_session->ingestProcess->arguments().contains(QStringLiteral("-sn")));
    timeshift.stopSession(false, QStringLiteral("test"), false, true);
#endif
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

void AppModelTests::dateTimeFormatsApplyOnlyOnSaveAndRefreshCachedPrograms()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    auto &settings = harness.settings->current();
    settings.dateOrder = QStringLiteral("dmy");
    settings.timeFormat = QStringLiteral("24h");
    auto *controller = harness.settingsController.get();
    controller->reload();
    auto *formatter = controller->dateTimeFormatter();
    formatter->apply(settings.dateOrder, settings.timeFormat);
    QSignalSpy formatSpy(formatter, &DateTimeFormatter::formatChanged);
    Channel channel;
    channel.id = 7;
    channel.profileId = harness.activeProfileId();
    channel.tvgId = QStringLiteral("format.channel");
    channel.name = QStringLiteral("Format channel");
    EpgEntry program;
    program.channelId = channel.tvgId;
    program.title = QStringLiteral("Format programme");
    program.start = QDateTime::currentDateTimeUtc().addSecs(-600);
    program.stop = program.start.addSecs(7200);
    harness.epgService->loadFromEntries({program});
    harness.appController->m_loadedChannels = {channel};
    harness.channelListModel->setChannels({channel}, {});
    QVERIFY(harness.channelListModel->selectById(channel.id));
    const auto oldMap = toVariantMap(program);
    harness.channelListModel->setCurrentProgramInfo({{channel.id, oldMap}});
    harness.guideStateModel->setChannels({channel});
    harness.guideStateModel->selectChannel(channel.id);
    harness.guideStateModel->selectProgram(oldMap);
    harness.epgGridModel->rebuild({channel}, 6, 24);
    harness.epgGridModel->setSelectedChannelId(channel.id);
    harness.epgGridModel->setSelectedProgramStart(oldMap.value("start").toString());
    harness.nowNextModel->setChannel(channel);
    harness.playbackNowNextModel->setChannel(channel);
    QTRY_VERIFY(!harness.nowNextModel->loading());
    QTRY_VERIFY(!harness.playbackNowNextModel->loading());
    const auto startEpoch = harness.epgGridModel->windowStartEpochMs();
    QSignalSpy resetSpy(harness.epgGridModel.get(), &QAbstractItemModel::modelReset);
    QSignalSpy tuneSpy(harness.playerController.get(), &PlayerController::playbackChannelActivated);
    const auto playerObject = harness.playerController->playbackPlayerObject();
    harness.playerController->m_playbackMode = QStringLiteral("catchup");
    harness.playerController->m_catchupSession.m_catchupDisplayProgram = program;

    controller->setDateOrder(QStringLiteral("mdy"));
    controller->setTimeFormat(QStringLiteral("12h"));
    QVERIFY(controller->dirty());
    QCOMPARE(formatSpy.count(), 0);
    QCOMPARE(formatter->timePattern(), QStringLiteral("HH:mm"));
    QVERIFY(formatter->preview(controller->dateOrder(), controller->timeFormat()).contains("Friday, 09.18  6:05 PM"));
    QCOMPARE(harness.nowNextModel->currentProgram().value("timeRange"), oldMap.value("timeRange"));
    controller->cancel();
    QVERIFY(!controller->dirty());
    QCOMPARE(controller->timeFormat(), QStringLiteral("24h"));
    controller->setDateOrder(QStringLiteral("mdy"));
    controller->setTimeFormat(QStringLiteral("12h"));
    // A pre-save worker or cache can still carry 24-hour labels. Readers must reformat them.
    harness.nowNextModel->refresh();
    controller->save();
    QCOMPARE(formatSpy.count(), 1);
    QVERIFY(!controller->dirty());
    QCOMPARE(formatter->timePattern(), QStringLiteral("h:mm AP"));
    const auto expected = program.start.toLocalTime().toString("h:mm AP")
        + " - " + program.stop.toLocalTime().toString("h:mm AP");
    QCOMPARE(harness.nowNextModel->currentProgram().value("timeRange").toString(), expected);
    QCOMPARE(harness.playbackNowNextModel->currentProgram().value("timeRange").toString(), expected);
    QCOMPARE(harness.guideStateModel->selectedProgram().value("timeRange").toString(), expected);
    QCOMPARE(harness.playerController->catchupCurrentProgram().value("timeRange").toString(), expected);
    QCOMPARE(harness.epgGridModel->selectedProgram().value("timeRange").toString(), expected);
    harness.channelListModel->setCurrentProgramInfo({{channel.id, oldMap}});
    QCOMPARE(harness.channelListModel->data(harness.channelListModel->index(0),
        ChannelListModel::CurrentProgramTimeRangeRole).toString(), expected);
    QTRY_VERIFY(!harness.nowNextModel->loading());
    harness.nowNextModel->setChannel(channel); // Cache hit after the in-flight result.
    QCOMPARE(harness.nowNextModel->currentProgram().value("timeRange").toString(), expected);
    QTRY_VERIFY(!harness.nowNextModel->loading());
    QCOMPARE(harness.epgGridModel->windowStartEpochMs(), startEpoch);
    QCOMPARE(harness.epgGridModel->selectedChannelId(), channel.id);
    QCOMPARE(harness.guideStateModel->selectedProgram().value("start"), oldMap.value("start"));
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(tuneSpy.count(), 0);
    QCOMPARE(harness.playerController->playbackPlayerObject(), playerObject);
    harness.settings->load();
    QCOMPARE(harness.settings->current().dateOrder, QStringLiteral("mdy"));
    QCOMPARE(harness.settings->current().timeFormat, QStringLiteral("12h"));
    controller->setTimeFormat(QStringLiteral("invalid"));
    QCOMPARE(controller->timeFormat(), QStringLiteral("system"));
    controller->cancel();
}

void AppModelTests::settingsControllerPreviewsUiTransparency()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    PlayerController player;
    MultiViewController multiView(&settings, nullptr, &player);
    ProfilesModel profiles(&settings);
    SettingsController controller(&settings, &player, &multiView, &profiles);
    QSignalSpy previewSpy(&controller, &SettingsController::uiTransparencyChanged);
    QSignalSpy settingsSpy(&controller, &SettingsController::settingsChanged);
    QSignalSpy dirtySpy(&controller, &SettingsController::dirtyChanged);

    QCOMPARE(controller.uiTransparency(), 100);
    controller.setUiTransparency(50);
    QCOMPARE(controller.uiTransparency(), 50);
    QCOMPARE(settings.current().uiTransparency, 100);
    QVERIFY(controller.dirty());
    QCOMPARE(previewSpy.count(), 1);
    QCOMPARE(dirtySpy.count(), 1);
    QCOMPARE(settingsSpy.count(), 0); // Dragging must not trigger unrelated settings work.
    controller.setUiTransparency(50);
    QCOMPARE(previewSpy.count(), 1);
    controller.cancel();
    QCOMPARE(controller.uiTransparency(), 100);
    QCOMPARE(previewSpy.count(), 2);
    QVERIFY(!controller.dirty());

    controller.setUiTransparency(-1);
    QCOMPARE(controller.uiTransparency(), 0);
    controller.setUiTransparency(101);
    QCOMPARE(controller.uiTransparency(), 100);
    QVERIFY(!controller.dirty());
    controller.setUiTransparency(35);
    controller.save();
    QVERIFY(!controller.dirty());
    SettingsManager restarted(tempDir.filePath(QStringLiteral("settings.json")));
    restarted.load();
    QCOMPARE(restarted.current().uiTransparency, 35);
    controller.setUiTransparency(0);
    controller.reload();
    QCOMPARE(controller.uiTransparency(), 35);
    QVERIFY(!controller.dirty());
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

    QCOMPARE(controller.overlayInactivitySeconds(), 60);
    controller.setOverlayInactivitySeconds(120);
    QVERIFY(controller.dirty());
    controller.cancel();
    QCOMPARE(controller.overlayInactivitySeconds(), 60);
    QVERIFY(!controller.dirty());
    controller.setOverlayInactivitySeconds(120);
    controller.save();
    settings.load();
    QCOMPARE(settings.current().overlayInactivitySeconds, 120);
    QVERIFY(!controller.dirty());
    controller.setOverlayInactivitySeconds(0);
    QCOMPARE(controller.overlayInactivitySeconds(), 1);
    controller.setOverlayInactivitySeconds(9999);
    QCOMPARE(controller.overlayInactivitySeconds(), 3600);
    controller.cancel();


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
    QCOMPARE(settings.current().guidePastHours, 999);
    QCOMPARE(controller.guidePastHours(), 999);
    controller.setEpgLookAheadHours(1000);
    controller.save();
    QCOMPARE(controller.epgLookAheadHours(), 999);
    settings.load();
    QCOMPARE(settings.current().guidePastHours, 999);
    QCOMPARE(settings.current().epgLookAheadHours, 999);

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

    controller.setPicturePreset(QStringLiteral("warm"));
    QVERIFY(controller.dirty());
    controller.cancel();
    QCOMPARE(controller.picturePreset(), QStringLiteral("standard"));
    QVERIFY(!controller.dirty());

    controller.setImageSmoothingEnabled(true);
    QVERIFY(controller.dirty());
    controller.cancel();
    QVERIFY(!controller.dirty());
    QCOMPARE(controller.imageSmoothingEnabled(), false);

    controller.setPlayerUserAgent(QStringLiteral("  CustomAgent/2.0  "));
    QVERIFY(controller.dirty());
    controller.cancel();
    QVERIFY(!controller.dirty());
    QCOMPARE(controller.playerUserAgent(), OKILTV::Core::defaultPlayerUserAgent());

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
    controller.setImageSmoothingEnabled(true);
    controller.setPicturePreset(QStringLiteral("movie"));
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
    QCOMPARE(settings.current().playerImageSmoothingEnabled, true);
    QCOMPARE(settings.current().playerPicturePreset, QStringLiteral("movie"));
    QCOMPARE(OKILTV::Core::appSettingsFromJson(OKILTV::Core::toJson(settings.current())).playerPicturePreset, QStringLiteral("movie"));
    QCOMPARE(settings.current().playerUserAgent, QStringLiteral("OKILTV-Agent/3.0"));
    QCOMPARE(settings.current().timeshiftEnabled, true);
    QCOMPARE(settings.current().timeshiftWindowMinutes, 360);
    QCOMPARE(settings.current().timeshiftSegmentSeconds, 60);
    QCOMPARE(settings.current().timeshiftStorageDirectory, QStringLiteral("/tmp/ts-cache"));
    QCOMPARE(settings.current().timeshiftMaxDiskGb, 128);
    QVERIFY(std::abs(controller.waitForDataStreamSeconds() - 120.0) < 0.0001);
    QVERIFY(std::abs(controller.bufferSizeSeconds() - 0.1) < 0.0001);
    QCOMPARE(controller.deinterlaceEnabled(), false);
    QCOMPARE(controller.imageSmoothingEnabled(), true);
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

void AppModelTests::catchupPipPreservesIndependentSessionsOnSwapAndClose()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);
    auto *first = harness.playerController.get();
    auto *multi = harness.multiViewController.get();
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-7200);
    const auto stop = start.addSecs(3600);
    first->playCatchupChannel(channels[0], QStringLiteral("http://127.0.0.1/archive-one"),
        QStringLiteral("Archive One"), start, stop);
    first->m_catchupSession.m_catchupTimelinePositionSeconds = 600;
    first->m_userPausedManually = true;
    first->m_pauseToggleRequested = true;
    first->setVolume(37);
    first->toggleMute();
    auto *firstBackend = first->player();
    QVERIFY(multi->togglePictureInPicture(-1));
    auto *second = multi->prepareCatchupPictureInPicture();
    QVERIFY(second);
    second->playCatchupChannel(channels[1], QStringLiteral("http://127.0.0.1/archive-two"),
        QStringLiteral("Archive Two"), start, stop);
    second->m_catchupSession.m_catchupTimelinePositionSeconds = 1200;
    auto *secondBackend = second->player();
    QSignalSpy firstTune(first, &PlayerController::playbackChannelActivated);
    QSignalSpy secondTune(second, &PlayerController::playbackChannelActivated);
    for (int i = 0; i < 3; ++i) {
        QVERIFY(multi->swapPrimaryWithPictureInPicture());
        QCOMPARE(multi->primaryController(), i % 2 == 0 ? second : first);
        QCOMPARE(harness.appController->m_playerController, multi->primaryController());
        QCOMPARE(first->player(), firstBackend);
        QCOMPARE(second->player(), secondBackend);
        QCOMPARE(first->m_catchupSession.m_catchupTimelinePositionSeconds, 600.0);
        QCOMPARE(second->m_catchupSession.m_catchupTimelinePositionSeconds, 1200.0);
        QVERIFY(first->m_userPausedManually);
        QVERIFY(first->m_pauseToggleRequested);
        if (i == 0) {
            QVERIFY(multi->primaryController()->muted());
            multi->primaryController()->toggleMute();
        }
        QCOMPARE(multi->primaryController()->volume(), 37.0);
        QVERIFY(!multi->toggleGrid());
    }
    QCOMPARE(firstTune.count(), 0);
    QCOMPARE(secondTune.count(), 0);
    // Internal catch-up backend cutover must update the small tile too.
    first->setSharedPlaybackPlayer(&first->m_catchupStandbyPlayer, false);
    QCOMPARE(multi->tiles()[1].toMap().value(QStringLiteral("playerObject")).value<QObject *>(),
        static_cast<QObject *>(&first->m_catchupStandbyPlayer));
    second->returnToLiveFromCatchup();
    QCOMPARE(second->currentPlaybackUrl(), channels[1].streamUrl);
    QVERIFY(first->inCatchupMode());
    QVERIFY(!multi->toggleGrid()); // Catch-up is now in the small window.
    multi->focusTile(0);
    QVERIFY(multi->togglePictureInPicture(-1));
    QCOMPARE(multi->layoutMode(), QStringLiteral("off"));
    QVERIFY(!first->currentChannelValue().has_value());
    QCOMPARE(second->currentPlaybackUrl(), channels[1].streamUrl);
    QVERIFY(multi->togglePictureInPicture(-1));
    auto *reopened = multi->prepareCatchupPictureInPicture();
    QCOMPARE(reopened, first);
    reopened->playChannel(channels[0]);
    QVERIFY(multi->swapPrimaryWithPictureInPicture());
    QCOMPARE(multi->primaryController(), first);
    QVERIFY(multi->swapPrimaryWithPictureInPicture());
    QCOMPARE(multi->primaryController(), second);
    QVERIFY(multi->toggleGrid()); // Both sessions are live again.
    multi->focusTile(1);
    multi->assignChannelToFocusedTile(channels[0].id);
    QVERIFY(multi->fullPromoteAndExit());
    QCOMPARE(multi->primaryController(), second);
    QCOMPARE(second->currentChannelValue()->id, channels[0].id);
    QVERIFY(second->player() != nullptr);
}

void AppModelTests::catchupPipGuideStartsSecondArchiveAndTracksBothBookmarks()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);
    for (auto &channel : channels) {
        channel.catchupSupported = true;
        channel.catchupWindowHours = 48;
        channel.catchupMode = QStringLiteral("append");
        channel.catchupSourceTemplate = QStringLiteral("utc={utc}&lutc={lutc}");
    }
    harness.channelListModel->setChannels(channels, {});
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-7200);
    const auto stop = start.addSecs(3600);
    EpgEntry a;
    a.channelId = channels[0].tvgId;
    a.title = QStringLiteral("First Archive");
    a.start = start;
    a.stop = stop;
    auto b = a;
    b.channelId = channels[1].tvgId;
    b.title = QStringLiteral("Second Archive");
    harness.appController->playCatchup(toVariantMap(channels[0]), toVariantMap(a));
    auto *first = harness.playerController.get();
    QVERIFY(first->inCatchupMode());
    const auto firstUrl = first->currentPlaybackUrl();
    QVERIFY(harness.multiViewController->togglePictureInPicture(-1));
    harness.appController->playCatchup(toVariantMap(channels[1]), toVariantMap(b));
    auto *multi = harness.multiViewController.get();
    auto *second = qobject_cast<PlayerController *>(multi->pipControllerObject());
    QVERIFY(second);
    QVERIFY(second->inCatchupMode());
    QCOMPARE(first->currentPlaybackUrl(), firstUrl);
    QCOMPARE(second->catchupCurrentProgram().value(QStringLiteral("title")).toString(), b.title);
    emit first->catchupProgressObserved({channels[0], start, stop, start.addSecs(600), false});
    emit second->catchupProgressObserved({channels[1], start, stop, start.addSecs(1200), false});
    QCOMPARE(harness.appController->catchupActionState(toVariantMap(channels[0]), toVariantMap(a)).value("resumeSeconds").toInt(), 600);
    QCOMPARE(harness.appController->catchupActionState(toVariantMap(channels[1]), toVariantMap(b)).value("resumeSeconds").toInt(), 1200);
    QCOMPARE(harness.appController->m_observedCatchupSession.value("channelId").toInt(), channels[0].id);
    QVERIFY(multi->swapPrimaryWithPictureInPicture());
    emit second->catchupProgressObserved({channels[1], start, stop, start.addSecs(1260), false});
    emit first->catchupProgressObserved({channels[0], start, stop, start.addSecs(660), false});
    QCOMPARE(harness.appController->m_observedCatchupSession.value("channelId").toInt(), channels[1].id);
    // Two programmes from the same channel are valid independent sessions too.
    b.channelId = channels[1].tvgId;
    b.start = start.addSecs(-3600);
    b.stop = start;
    multi->focusTile(1);
    harness.appController->playCatchup(toVariantMap(channels[1]), toVariantMap(b));
    QVERIFY(first->inCatchupMode());
    QCOMPARE(first->m_catchupSession.m_catchupProgramStartUtc, b.start);
    QVERIFY(second->inCatchupMode());
    multi->focusTile(1);
    multi->exitMultiView();
    QCOMPARE(multi->primaryController(), first);
    QVERIFY(first->inCatchupMode());
    QVERIFY(!second->currentChannelValue().has_value());
}

void AppModelTests::catchupPipConvertsExistingLivePip()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);
    harness.playerController->playChannel(channels[0]);
    auto *multi = harness.multiViewController.get();
    QVERIFY(multi->togglePictureInPicture(channels[1].id));
    QVERIFY(multi->swapPrimaryWithPictureInPicture());
    auto *second = multi->prepareCatchupPictureInPicture();
    QVERIFY(second);
    second->playCatchupChannel(channels[0], QStringLiteral("http://127.0.0.1/archive"), QStringLiteral("Archive"));
    QVERIFY(multi->swapPrimaryWithPictureInPicture());
    QCOMPARE(multi->primaryController(), second);
    QVERIFY(second->inCatchupMode());
    multi->focusTile(0);
    multi->exitMultiView();
    QVERIFY(second->inCatchupMode());
}

void AppModelTests::multiviewControllerAllowsCatchupPipButBlocksGrid()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();

    PlayerController playerController;
    MultiViewController multiViewController(&settings, nullptr, &playerController);

    Channel channel;
    channel.id = 3190;
    channel.name = QStringLiteral("Catch-up Multiview Block Channel");
    channel.profileId = QUuid::createUuid();
    channel.streamUrl = QStringLiteral("http://provider.example/live/user/pass/3190.ts");
    playerController.playCatchupChannel(
        channel,
        QStringLiteral("http://127.0.0.1/catchup3190"),
        QStringLiteral("Past Show"),
        QDateTime::currentDateTimeUtc().addSecs(-1200),
        QDateTime::currentDateTimeUtc().addSecs(900),
        QStringLiteral("http://provider.example/timeshift/user/pass/61/2026-05-18:12-00/3190.ts"));
    QVERIFY(QMetaObject::invokeMethod(playerController.player(), "fileLoaded", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        playerController.player(),
        "pauseStateChanged",
        Qt::DirectConnection,
        Q_ARG(bool, false)));

    QVERIFY(multiViewController.togglePictureInPicture(-1));
    QVERIFY(!multiViewController.toggleGrid());
    QCOMPARE(multiViewController.layoutMode(), QStringLiteral("pip"));
    multiViewController.setLayoutMode(QStringLiteral("grid2x2"));
    QCOMPARE(multiViewController.layoutMode(), QStringLiteral("pip"));
    QVERIFY(multiViewController.togglePictureInPicture(-1));
    QCOMPARE(multiViewController.layoutMode(), QStringLiteral("off"));
    QCOMPARE(playerController.playbackMode(), QStringLiteral("catchup"));
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

void AppModelTests::multiviewPromotedPipCanPauseAfterRepeatedSwaps()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);
    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    auto *multi = harness.multiViewController.get();
    QVERIFY(multi->togglePictureInPicture(channels.last().id));
    auto *original = multi->primaryController()->player();
    auto *secondary = qobject_cast<OKILTV::Player::MpvPlayer *>(
        multi->tiles().at(1).toMap().value(QStringLiteral("playerObject")).value<QObject *>());
    QVERIFY(secondary);
    QSignalSpy originalLoads(original, &OKILTV::Player::MpvPlayer::fileLoaded);
    QSignalSpy secondaryLoads(secondary, &OKILTV::Player::MpvPlayer::fileLoaded);

    for (int swapIndex = 0; swapIndex < 4; ++swapIndex) {
        QVERIFY(multi->swapPrimaryWithPictureInPicture());
        auto *controller = multi->primaryController();
        auto *active = controller->player();
        auto *inactive = active == original ? secondary : original;
        QCOMPARE(active, swapIndex % 2 == 0 ? secondary : original);
        // Supply observed backend state without requiring a provider connection.
        active->m_cachedTelemetry.pauseState = false;
        emit active->pauseStateChanged(false);
        QVERIFY(controller->isPlaying());

        controller->togglePause();
        QVERIFY(controller->m_pauseToggleRequested);
        QVERIFY(controller->m_userPausedManually);
        QVERIFY(controller->isPlaying()); // Wait for backend acknowledgement.
        active->m_cachedTelemetry.pauseState = true;
        emit active->pauseStateChanged(true);
        QVERIFY(!controller->isPlaying());
        emit inactive->pauseStateChanged(false);
        QVERIFY(!controller->isPlaying());

        controller->togglePause();
        QVERIFY(controller->m_pauseToggleRequested);
        QVERIFY(!controller->m_userPausedManually);
        QVERIFY(!controller->isPlaying());
        active->m_cachedTelemetry.pauseState = false;
        emit active->pauseStateChanged(false);
        QVERIFY(controller->isPlaying());
    }
    QCOMPARE(originalLoads.count(), 0);
    QCOMPARE(secondaryLoads.count(), 0);
}

void AppModelTests::multiviewPrimaryTileReflectsPlaybackPlayerObjectChanges()
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

    QTRY_VERIFY_WITH_TIMEOUT(!harness.multiViewController->tiles().isEmpty(), 1000);
    const auto initialTiles = harness.multiViewController->tiles();
    QCOMPARE(
        initialTiles.at(0).toMap().value(QStringLiteral("playerObject")).value<QObject *>(),
        static_cast<QObject *>(harness.playerController->player()));

    harness.playerController->setSharedPlaybackPlayer(&harness.playerController->m_catchupStandbyPlayer, false);

    const auto sharedTiles = harness.multiViewController->tiles();
    QCOMPARE(
        sharedTiles.at(0).toMap().value(QStringLiteral("playerObject")).value<QObject *>(),
        static_cast<QObject *>(&harness.playerController->m_catchupStandbyPlayer));

    harness.playerController->setSharedPlaybackPlayer(nullptr, false);

    const auto restoredTiles = harness.multiViewController->tiles();
    QCOMPARE(
        restoredTiles.at(0).toMap().value(QStringLiteral("playerObject")).value<QObject *>(),
        static_cast<QObject *>(harness.playerController->player()));
}

void AppModelTests::mpvVideoItemSharedPlayerDetachDoesNotClearOtherRenderTarget()
{
    OKILTV::Player::MpvPlayer sharedPlayer;
    OKILTV::Player::MpvVideoItem firstItem;
    OKILTV::Player::MpvVideoItem secondItem;

    firstItem.setObjectName(QStringLiteral("firstSharedItem"));
    secondItem.setObjectName(QStringLiteral("secondSharedItem"));

    firstItem.setPlayerObject(&sharedPlayer);
    secondItem.setPlayerObject(&sharedPlayer);
    sharedPlayer.setRenderUpdateTarget(&secondItem);
    QCOMPARE(sharedPlayer.m_updateTarget.data(), static_cast<QObject *>(&secondItem));

    firstItem.setPlayerObject(nullptr);
    QCOMPARE(sharedPlayer.m_updateTarget.data(), static_cast<QObject *>(&secondItem));
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

void AppModelTests::appControllerActivatingPipChannelSwapsWithoutRetune()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);

    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    auto *multi = harness.multiViewController.get();
    QVERIFY(multi->togglePictureInPicture(channels.last().id));
    multi->focusTile(0);
    const auto initialTiles = multi->tiles();
    auto *primaryPlayer = initialTiles.at(0).toMap().value(QStringLiteral("playerObject")).value<QObject *>();
    auto *pipPlayer = initialTiles.at(1).toMap().value(QStringLiteral("playerObject")).value<QObject *>();
    QVERIFY(primaryPlayer);
    QVERIFY(pipPlayer);
    QVERIFY(primaryPlayer != pipPlayer);
    QSignalSpy assignment(multi, &MultiViewController::primaryTileAssignmentRequested);

    QVERIFY(harness.channelListModel->activateById(channels.last().id));
    QCOMPARE(multi->layoutMode(), QStringLiteral("pip"));
    QCOMPARE(multi->primaryController()->currentChannelValue()->id, channels.last().id);
    const auto swappedTiles = multi->tiles();
    QCOMPARE(swappedTiles.at(1).toMap().value(QStringLiteral("channelId")).toInt(), channels.first().id);
    QCOMPARE(swappedTiles.at(0).toMap().value(QStringLiteral("playerObject")).value<QObject *>(), pipPlayer);
    QCOMPARE(swappedTiles.at(1).toMap().value(QStringLiteral("playerObject")).value<QObject *>(), primaryPlayer);
    QCOMPARE(assignment.count(), 0);
    QCOMPARE(multi->focusedTileIndex(), 0);

    // Selecting the other channel swaps the same two sessions back.
    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    const auto restoredTiles = multi->tiles();
    QCOMPARE(multi->primaryController()->currentChannelValue()->id, channels.first().id);
    QCOMPARE(restoredTiles.at(1).toMap().value(QStringLiteral("channelId")).toInt(), channels.last().id);
    QCOMPARE(restoredTiles.at(0).toMap().value(QStringLiteral("playerObject")).value<QObject *>(), primaryPlayer);
    QCOMPARE(restoredTiles.at(1).toMap().value(QStringLiteral("playerObject")).value<QObject *>(), pipPlayer);
    QCOMPARE(assignment.count(), 0);
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

    // Unexpected EOF starts automatic recovery, so another activation must
    // not replace the reconnect already in flight.
    QVERIFY(harness.playerController->channelSwitchInProgress());
    QVERIFY(harness.channelListModel->activateById(channelId));
    QCOMPARE(playbackActivationSpy.count(), 0);

    harness.playerController->stop();
    QVERIFY(!harness.playerController->isPlaying());
    QVERIFY(!harness.playerController->channelSwitchInProgress());

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

void AppModelTests::appControllerPlayCatchupRejectsRunningProgramBeforeSourceMargin_data()
{
    QTest::addColumn<int>("marginMinutes");
    QTest::newRow("default-three-minutes") << 3;
    QTest::newRow("custom-five-minutes") << 5;
    QTest::newRow("custom-twelve-minutes") << 12;
}

void AppModelTests::appControllerPlayCatchupRejectsRunningProgramBeforeSourceMargin()
{
    QFETCH(int, marginMinutes);
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

    if (marginMinutes != 3) {
        auto profile = harness.settings->activeProfile().value();
        profile.catchupSafetyMinutes = marginMinutes;
        QVERIFY(harness.settings->replaceProfile(profile.id, profile));
    }
    const auto programStart = QDateTime::currentDateTimeUtc().addSecs(-(marginMinutes * 60 - 1));
    const auto programStop = QDateTime::currentDateTimeUtc().addSecs(20 * 60);
    const auto channelVariant = toVariantMap(channels.first());
    const QVariantMap programVariant {
        { QStringLiteral("channelId"), channels.first().tvgId },
        { QStringLiteral("title"), QStringLiteral("Running Show") },
        { QStringLiteral("start"), programStart.toString(Qt::ISODateWithMs) },
        { QStringLiteral("stop"), programStop.toString(Qt::ISODateWithMs) }
    };

    harness.appController->playCatchup(channelVariant, programVariant);

    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("live"));
    QVERIFY(harness.appController->statusText().contains(QStringLiteral("after %1 minutes").arg(marginMinutes)));
    const auto action = harness.appController->catchupActionState(channelVariant, programVariant);
    QVERIFY(action.value(QStringLiteral("visible")).toBool());
    QVERIFY(!action.value(QStringLiteral("enabled")).toBool());
    QCOMPARE(action.value(QStringLiteral("safetySeconds")).toInt(), marginMinutes * 60);
}

void AppModelTests::appControllerPlayCatchupAllowsRunningProgramAfterSourceMargin_data()
{
    appControllerPlayCatchupRejectsRunningProgramBeforeSourceMargin_data();
    QTest::newRow("zero-minutes") << 0;
    QTest::newRow("one-minute") << 1;
    QTest::newRow("two-minutes") << 2;
}

void AppModelTests::appControllerPlayCatchupAllowsRunningProgramAfterSourceMargin()
{
    QFETCH(int, marginMinutes);
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

    if (marginMinutes != 3) {
        auto profile = harness.settings->activeProfile().value();
        profile.catchupSafetyMinutes = marginMinutes;
        QVERIFY(harness.settings->replaceProfile(profile.id, profile));
    }
    const auto programStart = QDateTime::currentDateTimeUtc().addSecs(-(marginMinutes * 60 + 1));
    const auto programStop = QDateTime::currentDateTimeUtc().addSecs(20 * 60);
    const auto channelVariant = toVariantMap(channels.first());
    const QVariantMap programVariant {
        { QStringLiteral("channelId"), channels.first().tvgId },
        { QStringLiteral("title"), QStringLiteral("Running Show") },
        { QStringLiteral("start"), programStart.toString(Qt::ISODateWithMs) },
        { QStringLiteral("stop"), programStop.toString(Qt::ISODateWithMs) }
    };

    harness.appController->playCatchup(channelVariant, programVariant);

    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("catchup"));
    QCOMPARE(harness.playerController->m_catchupSession.m_catchupSafetySeconds, marginMinutes * 60);
    const auto action = harness.appController->catchupActionState(channelVariant, programVariant);
    QVERIFY(action.value(QStringLiteral("enabled")).toBool());
    QCOMPARE(action.value(QStringLiteral("safetySeconds")).toInt(), marginMinutes * 60);
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

void AppModelTests::startupRestoresCatchup_data()
{
    QTest::addColumn<int>("remainingSeconds");
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<bool>("resume");
    QTest::newRow("resume-without-epg") << 1800 << QString {} << true;
    QTest::newRow("paused") << 1800 << QStringLiteral("paused") << true;
    QTest::newRow("over-five-minutes") << 301 << QString {} << true;
    QTest::newRow("exactly-five-minutes") << 300 << QString {} << false;
    QTest::newRow("under-five-minutes-before-rounding") << 299 << QString {} << false;
    QTest::newRow("expired") << 1800 << QStringLiteral("expired") << false;
    QTest::newRow("disabled") << 1800 << QStringLiteral("disabled") << false;
    QTest::newRow("live-after-catchup") << 1800 << QStringLiteral("live") << false;
    QTest::newRow("changed-channel-identity") << 1800 << QStringLiteral("changed") << false;
}

void AppModelTests::startupRestoresCatchup()
{
    QFETCH(int, remainingSeconds);
    QFETCH(QString, scenario);
    QFETCH(bool, resume);
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    QFile playlist(harness.playlistPath);
    QVERIFY(playlist.open(QIODevice::WriteOnly | QIODevice::Truncate));
    playlist.write("#EXTM3U\n#EXTINF:-1 tvg-id=\"channel.one\" catchup=\"append\" catchup-days=\"3\" catchup-source=\"utc={utc}&lutc={lutc}\",Channel One\nhttp://127.0.0.1:1/channel-one\n");
    playlist.close();
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->epgRefreshInProgress(), 5000);
    const auto channel = harness.channelListModel->allChannels().first();
    QVERIFY(channel.catchupSupported);
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-7200);
    EpgEntry entry;
    entry.channelId = channel.tvgId;
    entry.title = QStringLiteral("Saved programme");
    entry.start = start;
    entry.stop = start.addSecs(3600);
    harness.appController->playCatchup(toVariantMap(channel), toVariantMap(entry));
    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("catchup"));
    emit harness.playerController->catchupProgressObserved(
        { channel, start, entry.stop, entry.stop.addSecs(-remainingSeconds), false });
    if (scenario == QStringLiteral("paused")) {
        harness.playerController->setIsPlaying(false);
    }
    if (scenario == QStringLiteral("live")) {
        harness.playerController->returnToLiveFromCatchup();
    }
    harness.appController->savePlaybackForApplicationExit();
    harness.playerController->shutdownForApplicationExit();
    harness.settings->load(); // Exercise the persisted JSON, not just an in-memory bookmark.
    auto &session = harness.settings->current().lastCatchupSession;
    QCOMPARE(session.isEmpty(), scenario == QStringLiteral("live"));
    if (scenario == QStringLiteral("expired")) {
        entry.start = start.addDays(-4);
        entry.stop = entry.start.addSecs(3600);
        session.insert(QStringLiteral("program"), QJsonObject::fromVariantMap(toVariantMap(entry)));
        session.insert(QStringLiteral("key"), CatchupProgress::keyFor(channel, entry.start));
    } else if (scenario == QStringLiteral("disabled")) {
        harness.settings->current().catchupEnabled = false;
    } else if (scenario == QStringLiteral("changed")) {
        session.insert(QStringLiteral("key"), QStringLiteral("different-stream"));
    }
    harness.appController.reset();
    harness.appController = std::make_unique<AppController>(
        harness.settings.get(), harness.database.get(), harness.network,
        harness.profilesModel.get(), harness.channelListModel.get(), harness.nowNextModel.get(),
        harness.playbackNowNextModel.get(), harness.epgGridModel.get(), harness.guideStateModel.get(),
        harness.shellController.get(), harness.multiViewController.get(), harness.playerController.get(),
        harness.dvrController.get(), harness.timeshiftController.get(), harness.settingsController.get(), harness.epgService.get());
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    QCOMPARE(harness.playerController->playbackMode(), resume ? QStringLiteral("catchup") : QStringLiteral("live"));
    QVERIFY(harness.playerController->currentChannelValue().has_value());
    QCOMPARE(harness.playerController->currentChannelValue()->id, channel.id);
    if (resume) {
        QCOMPARE(harness.playerController->m_catchupSession.m_catchupTimelinePositionSeconds,
            static_cast<double>(((3600 - remainingSeconds) / 60) * 60));
    } else {
        QCOMPARE(harness.playerController->currentPlaybackUrl(), channel.streamUrl);
    }
    QVERIFY(harness.appController->m_startupCatchupSession.isEmpty());
    if (resume) {
        harness.playerController->returnToLiveFromCatchup();
        harness.appController->refreshActiveProfile();
        QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
        QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("live"));
    }
}

void AppModelTests::startupRestoresEndlessXtreamCatchup()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->epgRefreshInProgress(), 5000);
    auto profile = harness.settings->activeProfile().value();
    profile.type = ProfileType::Xtream;
    profile.xtreamBaseUrl = QStringLiteral("http://127.0.0.1:1");
    profile.xtreamUsername = QStringLiteral("user");
    profile.xtreamPassword = QStringLiteral("pass");
    QVERIFY(harness.settings->replaceProfile(profile.id, profile));
    auto channel = harness.channelListModel->allChannels().first();
    channel.source = ChannelSource::Xtream;
    channel.catchupSupported = true;
    channel.catchupWindowHours = 72;
    channel.streamUrl = QStringLiteral("http://127.0.0.1:1/live/user/pass/952.ts");
    harness.channelListModel->setChannels({ channel }, {});
    EpgEntry program;
    program.channelId = channel.tvgId;
    program.title = QStringLiteral("Programme after session origin");
    program.start = QDateTime::currentDateTimeUtc().addSecs(-7200);
    program.stop = program.start.addSecs(3600);
    harness.epgService->loadFromEntries({ program });
    harness.appController->m_epgLoadedProfileId = channel.profileId;
    emit harness.playerController->catchupProgressObserved(
        { channel, program.start.addSecs(-3600), program.start, program.start.addSecs(1427), true });
    harness.appController->m_startupCatchupSession = harness.appController->m_observedCatchupSession;
    harness.epgService->loadFromEntries({}); // Startup has not downloaded EPG yet.
    QVERIFY(harness.appController->restoreStartupCatchup(channel));
    QTRY_COMPARE_WITH_TIMEOUT(harness.playerController->playbackMode(), QStringLiteral("catchup"), 5000);
    QVERIFY(harness.playerController->m_catchupSession.m_catchupEndless);
    QCOMPARE(harness.playerController->m_catchupSession.m_catchupStreamBaseOffsetSeconds, 1380.0);
    QCOMPARE(harness.playerController->m_catchupSession.m_catchupProgramStartUtc,
        QDateTime::fromSecsSinceEpoch((program.start.toSecsSinceEpoch() / 60) * 60, QTimeZone::UTC));
    QCOMPARE(harness.playerController->catchupCurrentProgram().value(QStringLiteral("title")).toString(), program.title);
    emit harness.playerController->catchupProgressObserved(
        { channel, program.start, program.stop, program.start.addSecs(1500), true });
    QCOMPARE(harness.appController->m_observedCatchupSession.value(QStringLiteral("positionMs")).toInteger(), 1500000);
    QVERIFY(!harness.appController->catchupProgramAt(channel, program.stop).has_value());
    harness.playerController->returnToLiveFromCatchup();
    QCOMPARE(harness.playerController->currentPlaybackUrl(), channel.streamUrl);
}

void AppModelTests::appControllerCatchupResumePersistsAndHonorsExplicitStart_data()
{
    QTest::addColumn<bool>("xtream");
    QTest::newRow("m3u") << false;
    QTest::newRow("xtream") << true;
}

void AppModelTests::appControllerCatchupResumePersistsAndHonorsExplicitStart()
{
    QFETCH(bool, xtream);
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->epgRefreshInProgress(), 5000);
    auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    auto &channel = channels[0];
    channel.catchupSupported = true;
    channel.catchupWindowHours = 72;
    channel.catchupMode = QStringLiteral("append");
    channel.catchupSourceTemplate = QStringLiteral("utc={utc}&lutc={lutc}");
    if (xtream) {
        auto profile = harness.settings->activeProfile().value();
        profile.type = ProfileType::Xtream;
        profile.xtreamBaseUrl = QStringLiteral("http://127.0.0.1:1");
        profile.xtreamUsername = QStringLiteral("user");
        profile.xtreamPassword = QStringLiteral("pass");
        QVERIFY(harness.settings->replaceProfile(profile.id, profile));
        channel.source = ChannelSource::Xtream;
        channel.id = 952;
        channel.streamUrl = QStringLiteral("http://127.0.0.1:1/live/user/pass/952.ts");
    }
    harness.channelListModel->setChannels(channels, {});
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-7200);
    const auto stop = start.addSecs(3600);
    const auto channelVariant = toVariantMap(channel);
    QVariantMap program {
        { QStringLiteral("channelId"), channel.tvgId },
        { QStringLiteral("title"), QStringLiteral("Programme") },
        { QStringLiteral("start"), start.toString(Qt::ISODateWithMs) },
        { QStringLiteral("stop"), stop.toString(Qt::ISODateWithMs) }
    };
    const auto observe = [&](qint64 seconds) {
        emit harness.playerController->catchupProgressObserved({ channel, start, stop, start.addSecs(seconds), false });
    };
    observe(1427);
    QCOMPARE(harness.appController->catchupActionState(channelVariant, program).value("resumeSeconds").toLongLong(), 1380);
    emit harness.playerController->catchupProgressFlushRequested();
    QCOMPARE(harness.database->loadCatchupProgress().first().positionMs, 1427000);

    // Reconstruct the controller against the same database, as on application restart.
    harness.appController.reset();
    harness.appController = std::make_unique<AppController>(
        harness.settings.get(), harness.database.get(), harness.network,
        harness.profilesModel.get(), harness.channelListModel.get(), harness.nowNextModel.get(),
        harness.playbackNowNextModel.get(), harness.epgGridModel.get(), harness.guideStateModel.get(),
        harness.shellController.get(), harness.multiViewController.get(), harness.playerController.get(),
        harness.dvrController.get(), harness.timeshiftController.get(), harness.settingsController.get(), harness.epgService.get());
    program[QStringLiteral("title")] = QStringLiteral("Renamed programme");
    QCOMPARE(harness.appController->catchupActionState(channelVariant, program).value("resumeSeconds").toLongLong(), 1380);
    harness.appController->resumeCatchup(channelVariant, program);
    QTRY_COMPARE_WITH_TIMEOUT(harness.playerController->playbackMode(), QStringLiteral("catchup"), 5000);
    QCOMPARE(harness.playerController->catchupTimelinePositionSeconds(), 1380.0);
    if (xtream) {
        QCOMPARE(harness.playerController->m_catchupSession.m_catchupStreamBaseOffsetSeconds, 1380.0);
        QVERIFY(!harness.playerController->m_catchupSession.m_catchupPendingInitialSeekSeconds.has_value());
        QVERIFY(harness.playerController->currentPlaybackUrl().contains(start.addSecs(1380).toString("yyyy-MM-dd:HH-mm")));
    } else {
        QCOMPARE(harness.playerController->m_catchupSession.m_catchupStreamBaseOffsetSeconds, 1380.0);
        QVERIFY(!harness.playerController->m_catchupSession.m_catchupPendingInitialSeekSeconds);
    }
    harness.playerController->stop();
    // A failed load, with no confirmed position, must retain the bookmark.
    QCOMPARE(harness.appController->catchupActionState(channelVariant, program).value("resumeSeconds").toLongLong(), 1380);
    harness.appController->playCatchup(channelVariant, program);
    QTRY_COMPARE_WITH_TIMEOUT(harness.playerController->playbackMode(), QStringLiteral("catchup"), 5000);
    QCOMPARE(harness.playerController->catchupTimelinePositionSeconds(), 0.0);
    QCOMPARE(harness.appController->catchupActionState(channelVariant, program).value("resumeSeconds").toLongLong(), 1380);
    observe(5); // Only successful playback replaces the old bookmark.
    QVERIFY(!harness.appController->catchupActionState(channelVariant, program).value("resumeAvailable").toBool());
    observe(1800);
    observe(647);
    QCOMPARE(harness.appController->catchupActionState(channelVariant, program).value("resumeSeconds").toLongLong(), 600);
    auto shifted = program;
    shifted[QStringLiteral("start")] = start.addSecs(60).toString(Qt::ISODateWithMs);
    QVERIFY(!harness.appController->catchupActionState(channelVariant, shifted).value("resumeAvailable").toBool());
    observe(3539);
    QVERIFY(harness.appController->catchupActionState(channelVariant, program).value("resumeAvailable").toBool());
    observe(3540);
    QVERIFY(!harness.appController->catchupActionState(channelVariant, program).value("resumeAvailable").toBool());
    harness.playerController->shutdownForApplicationExit();
    QVERIFY(harness.database->loadCatchupProgress().isEmpty());
    observe(1200);
    harness.appController->flushCatchupProgress();
    QVERIFY(harness.settings->removeProfile(channel.profileId));
    harness.appController->pruneCatchupProgress();
    QVERIFY(harness.database->loadCatchupProgress().isEmpty());
}

void AppModelTests::appControllerCatchupProgressFollowsEndlessProgramme()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->epgRefreshInProgress(), 5000);
    auto channel = harness.channelListModel->allChannels().first();
    channel.catchupSupported = true;
    channel.catchupWindowHours = 72;
    const auto origin = QDateTime::currentDateTimeUtc().addSecs(-10800);
    EpgEntry first;
    first.channelId = channel.tvgId;
    first.start = origin;
    first.stop = origin.addSecs(3600);
    auto second = first;
    second.start = first.stop;
    second.stop = second.start.addSecs(3600);
    harness.epgService->loadFromEntries({ first, second });
    harness.appController->m_epgLoadedProfileId = channel.profileId;
    const auto observe = [&](qint64 seconds) {
        emit harness.playerController->catchupProgressObserved({ channel, origin, first.stop, origin.addSecs(seconds), true });
    };
    observe(1800);
    // A delayed sample can skip the final minute of A entirely.
    observe(3600 + 1427);
    harness.appController->flushCatchupProgress();
    const auto entries = harness.database->loadCatchupProgress();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().programStartMs, second.start.toMSecsSinceEpoch());
    QCOMPARE(entries.first().positionMs, 1427000);
    const auto session = harness.appController->m_observedCatchupSession;
    QCOMPARE(session.value(QStringLiteral("positionMs")).toInteger(), 1427000);
    QVERIFY(session.value(QStringLiteral("endless")).toBool());
    QCOMPARE(session.value(QStringLiteral("program")).toObject().value(QStringLiteral("start")).toString(),
        second.start.toString(Qt::ISODateWithMs));
    observe(7500); // No EPG: never attribute the third programme to the second.
    QVERIFY(harness.appController->m_observedCatchupSession.isEmpty());
    harness.appController->flushCatchupProgress();
    QCOMPARE(harness.database->loadCatchupProgress().first().positionMs, 1427000);
    observe(3600 + 605); // Rewind within B uses its full EPG start, not the session origin.
    harness.appController->flushCatchupProgress();
    QCOMPARE(harness.database->loadCatchupProgress().first().positionMs, 605000);
    const auto key = entries.first().key;
    harness.appController->m_catchupProgress[key].expiresAtMs = 0;
    harness.appController->pruneCatchupProgress();
    QVERIFY(harness.database->loadCatchupProgress().isEmpty());
}

void AppModelTests::playerControllerCatchupResumeReportsRealPlayback_data()
{
    QTest::addColumn<bool>("xtream");
    QTest::newRow("native-m3u-seek") << false;
    QTest::newRow("owned-xtream-minute-anchor") << true;
}

void AppModelTests::playerControllerCatchupResumeReportsRealPlayback()
{
    QFETCH(bool, xtream);
    const auto ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        QSKIP("Requires ffmpeg for a synthetic catch-up playback fixture.");
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("archive.ts"));
    QProcess generator;
    generator.start(ffmpeg, {
        QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("testsrc2=size=64x48:rate=10:duration=120"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-preset"), QStringLiteral("ultrafast"),
        QStringLiteral("-g"), QStringLiteral("10"), QStringLiteral("-bf"), QStringLiteral("0"), path
    });
    QVERIFY(generator.waitForFinished(15000));
    QVERIFY2(generator.exitCode() == 0, generator.readAllStandardError().constData());
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto payload = file.readAll();
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (auto *socket = server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, socket, [socket, &payload]() {
                const auto request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n") || socket->property("sent").toBool()) {
                    return;
                }
                socket->setProperty("sent", true);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: "
                    + QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n");
                socket->write(payload);
                socket->disconnectFromHost();
            });
        }
    });
    const auto previousHeadless = qgetenv("OKILTV_HEADLESS_TEST");
    const auto restoreEnvironment = qScopeGuard([&]() {
        if (previousHeadless.isNull()) {
            qunsetenv("OKILTV_HEADLESS_TEST");
        } else {
            qputenv("OKILTV_HEADLESS_TEST", previousHeadless);
        }
    });
    qputenv("OKILTV_HEADLESS_TEST", "0");
    PlayerController player;
    player.m_player.configureOptions({
        { QStringLiteral("vo"), QStringLiteral("null") },
        { QStringLiteral("ao"), QStringLiteral("null") },
        { QStringLiteral("hwdec"), QStringLiteral("no") },
        { QStringLiteral("load-scripts"), QStringLiteral("no") }
    });
    Channel channel;
    channel.profileId = QUuid::createUuid();
    channel.source = xtream ? ChannelSource::Xtream : ChannelSource::M3U;
    channel.streamUrl = QStringLiteral("http://127.0.0.1/live");
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-7200);
    const auto canonical = QStringLiteral("http://127.0.0.1:%1/timeshift/user/pass/10/%2/1.ts")
        .arg(server.serverPort()).arg(start.toString(QStringLiteral("yyyy-MM-dd:HH-mm")));
    const auto url = xtream
        ? QStringLiteral("http://127.0.0.1:%1/timeshift/user/pass/9/%2/1.ts")
              .arg(server.serverPort()).arg(start.addSecs(60).toString(QStringLiteral("yyyy-MM-dd:HH-mm")))
        : QUrl::fromLocalFile(path).toString();
    QList<CatchupProgressSample> samples;
    connect(&player, &PlayerController::catchupProgressObserved, this, [&samples](const CatchupProgressSample &sample) {
        samples.append(sample);
    });
    player.playCatchupChannel(channel, url, QStringLiteral("Archive"), start, start.addSecs(600),
        xtream ? canonical : url, xtream ? std::nullopt : std::optional<double>(60.0),
        xtream ? std::optional<double>(60.0) : std::nullopt, 60.0);
    QTRY_VERIFY_WITH_TIMEOUT(!samples.isEmpty(), 10000);
    const auto position = start.secsTo(samples.first().watchedTime);
    QVERIFY2(position >= 50 && position < 80, qPrintable(QString::number(position)));
    player.togglePause();
    QTRY_VERIFY_WITH_TIMEOUT(!player.isPlaying(), 3000);
    player.stop();
}

void AppModelTests::playerControllerCatchupProgressIgnoresUnconfirmedTransport()
{
    PlayerController player;
    Channel channel;
    channel.profileId = QUuid::createUuid();
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-7200);
    player.m_currentChannel = channel;
    player.m_playbackMode = QStringLiteral("catchup");
    player.m_catchupSession.m_catchupProgramStartUtc = start;
    player.m_catchupSession.m_catchupProgramStopUtc = start.addSecs(3600);
    player.m_catchupSession.m_catchupStreamBaseOffsetSeconds = 1200;
    player.m_isPlaying = true;
    QList<CatchupProgressSample> samples;
    connect(&player, &PlayerController::catchupProgressObserved, this, [&samples](const CatchupProgressSample &sample) {
        samples.append(sample);
    });
    player.publishCatchupProgress(227);
    QVERIFY(samples.isEmpty()); // No file-loaded confirmation yet.
    player.m_catchupSession.m_catchupProgressTransportReady = true;
    player.publishCatchupProgress(227);
    QCOMPARE(samples.size(), 1);
    QCOMPARE(samples.last().watchedTime, start.addSecs(1427));
    player.m_catchupSession.m_catchupTimelinePositionSeconds = 2400; // Optimistic slider target is ignored.
    player.m_catchupSession.m_seekPhase = OKILTV::App::Playback::CatchupPlaybackSession::SeekPhase::WaitingStop;
    player.publishCatchupProgress(0);
    QCOMPARE(samples.size(), 1);
    player.m_catchupSession.cancelReload();
    player.m_catchupSession.m_catchupProgressSeekTargetSeconds = 1800;
    player.publishCatchupProgress(0); // Unapplied/failed native seek.
    QCOMPARE(samples.size(), 1);
    player.publishCatchupProgress(603);
    QCOMPARE(samples.size(), 2);
    QCOMPARE(samples.last().watchedTime, start.addSecs(1803));
    player.m_catchupSession.m_catchupReconnectResumeStreamRelativeSeconds = 603;
    player.publishCatchupProgress(0);
    QCOMPARE(samples.size(), 2);
    player.m_catchupSession.m_catchupReconnectResumeStreamRelativeSeconds.reset();
    player.m_catchupSession.setAlignmentActive(true);
    player.publishCatchupProgress(0);
    QCOMPARE(samples.size(), 2);
    player.m_catchupSession.setAlignmentActive(false);
    player.m_isPlaying = false;
    player.publishCatchupProgress(650);
    QCOMPARE(samples.size(), 2);
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

    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->epgRefreshInProgress(), 5000);
    EpgEntry liveProgram;
    liveProgram.channelId = channels.first().tvgId;
    liveProgram.title = QStringLiteral("Live Show");
    liveProgram.start = QDateTime::currentDateTimeUtc().addSecs(-600);
    liveProgram.stop = liveProgram.start.addSecs(3600);
    harness.epgService->loadFromEntries({liveProgram});
    harness.playbackNowNextModel->setChannel(channels.first());
    QTRY_COMPARE_WITH_TIMEOUT(harness.playbackNowNextModel->currentProgram().value(QStringLiteral("title")).toString(), liveProgram.title, 5000);

    harness.appController->playCatchup(channelVariant, programVariant);

    QCOMPARE(harness.playerController->catchupCurrentProgram().value(QStringLiteral("title")).toString(), QStringLiteral("Past Show"));
    QCOMPARE(harness.playerController->catchupCurrentProgram().value(QStringLiteral("progressPercent")).toDouble(), 0.0);
    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("catchup"));
    const QUrlQuery query(QUrl(harness.playerController->currentPlaybackUrl()));
    QCOMPARE(query.queryItemValue(QStringLiteral("utc")).toLongLong(), programStart.toSecsSinceEpoch());
    const auto edge = query.queryItemValue(QStringLiteral("lutc")).toLongLong();
    QVERIFY(edge > programStop.toSecsSinceEpoch());
    QVERIFY(std::abs(edge - QDateTime::currentDateTimeUtc().addSecs(-180).toSecsSinceEpoch()) <= 2);

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

    const auto programStart = QDateTime::currentDateTimeUtc().addSecs(-7200);
    const auto programStop = programStart.addSecs(3600);
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
    const QUrlQuery query(QUrl(harness.playerController->currentPlaybackUrl()));
    QCOMPARE(query.queryItemValue(QStringLiteral("utc")).toLongLong(), programStart.toSecsSinceEpoch());
    const auto edge = query.queryItemValue(QStringLiteral("lutc")).toLongLong();
    QVERIFY(edge > programStop.toSecsSinceEpoch());
    QVERIFY(std::abs(edge - QDateTime::currentDateTimeUtc().addSecs(-180).toSecsSinceEpoch()) <= 2);
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

    const auto programStartUtc = QDateTime::currentDateTimeUtc().addSecs(-7200);
    const auto programStopUtc = programStartUtc.addSecs(3600);
    const auto programStartOffset = programStartUtc.toOffsetFromUtc(2 * 3600);
    const auto programStopOffset = programStopUtc.toOffsetFromUtc(2 * 3600);
    const auto channelVariant = toVariantMap(channels.first());
    const QVariantMap programVariant {
        { QStringLiteral("channelId"), channels.first().tvgId },
        { QStringLiteral("title"), QStringLiteral("Past Show") },
        { QStringLiteral("start"), programStartOffset.toString(Qt::ISODate) },
        { QStringLiteral("stop"), programStopOffset.toString(Qt::ISODate) }
    };

    harness.appController->playCatchup(channelVariant, programVariant);

    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("catchup"));
    const QUrlQuery query(QUrl(harness.playerController->currentPlaybackUrl()));
    QCOMPARE(query.queryItemValue(QStringLiteral("utc")).toLongLong(), programStartUtc.toSecsSinceEpoch());
    const auto edge = query.queryItemValue(QStringLiteral("lutc")).toLongLong();
    QVERIFY(edge > programStopUtc.toSecsSinceEpoch());
    QVERIFY(std::abs(edge - QDateTime::currentDateTimeUtc().addSecs(-180).toSecsSinceEpoch()) <= 2);
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

    auto profile = harness.settings->activeProfile();
    QVERIFY(profile.has_value());
    profile->type = ProfileType::Xtream;
    profile->xtreamBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    profile->xtreamUsername = QStringLiteral("user");
    profile->xtreamPassword = QStringLiteral("pass");
    QVERIFY(harness.settings->replaceProfile(profile->id, profile.value()));

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
    QVERIFY(harness.playerController->m_catchupSession.m_catchupEndless);
}

void AppModelTests::catchupPipPendingRedirectIsCancelledWhenClosed()
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

    auto profile = harness.settings->activeProfile();
    QVERIFY(profile.has_value());
    profile->type = ProfileType::Xtream;
    profile->xtreamBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    profile->xtreamUsername = QStringLiteral("user");
    profile->xtreamPassword = QStringLiteral("pass");
    QVERIFY(harness.settings->replaceProfile(profile->id, profile.value()));

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

    auto *multi = harness.multiViewController.get();
    harness.playerController->playChannel(channels[0]);
    QVERIFY(multi->togglePictureInPicture(-1));
    harness.appController->playCatchup(channelVariant, programVariant);
    auto *secondary = qobject_cast<PlayerController *>(multi->pipControllerObject());
    QVERIFY(secondary);
    QSignalSpy activated(secondary, &PlayerController::playbackChannelActivated);
    multi->focusTile(0);
    multi->exitMultiView();
    QVERIFY(multi->togglePictureInPicture(-1));
    QCOMPARE(multi->pipControllerObject(), secondary);
    QTRY_VERIFY_WITH_TIMEOUT(harness.appController->m_backgroundTasks.futures().constLast().isFinished(), 5000);
    QCoreApplication::processEvents();
    QCOMPARE(activated.count(), 0);
    QVERIFY(!secondary->currentChannelValue().has_value());
    QCOMPARE(harness.playerController->playbackMode(), QStringLiteral("live"));
    QCOMPARE(harness.playerController->currentPlaybackUrl(), channels[0].streamUrl);
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

    auto profile = harness.settings->activeProfile();
    QVERIFY(profile.has_value());
    profile->type = ProfileType::Xtream;
    profile->xtreamBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    profile->xtreamUsername = QStringLiteral("user");
    profile->xtreamPassword = QStringLiteral("pass");
    QVERIFY(harness.settings->replaceProfile(profile->id, profile.value()));

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

    auto profile = harness.settings->activeProfile();
    QVERIFY(profile.has_value());
    profile->type = ProfileType::Xtream;
    profile->xtreamBaseUrl = QStringLiteral("http://127.0.0.1:9");
    profile->xtreamUsername = QStringLiteral("user");
    profile->xtreamPassword = QStringLiteral("pass");
    QVERIFY(harness.settings->replaceProfile(profile->id, profile.value()));

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

    CatchupUrlResolver resolver(profile.value());
    QString reason;
    const auto target = resolver.resolveWindow(channels.first(), programStart,
        QDateTime::currentDateTimeUtc().addSecs(-180), &reason);
    QVERIFY(target.has_value());

    harness.appController->playCatchup(channelVariant, programVariant);
    QTRY_COMPARE_WITH_TIMEOUT(harness.playerController->playbackMode(), QStringLiteral("catchup"), 5000);
    const auto after = resolver.resolveWindow(channels.first(), programStart,
        QDateTime::currentDateTimeUtc().addSecs(-180));
    QVERIFY(after);
    QVERIFY(harness.playerController->currentPlaybackUrl() == target->url
        || harness.playerController->currentPlaybackUrl() == after->url);
    QCOMPARE(harness.playerController->currentPlaybackUrl(), harness.playerController->m_catchupSession.m_catchupCanonicalPlaybackUrl);
}

void AppModelTests::appControllerPlayCatchupAtOffsetXtreamLiveProgramUsesOriginDurationDelta()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const auto port = server.serverPort();
    QVERIFY(QObject::connect(&server, &QTcpServer::newConnection, &server, [&server]() {
        while (server.hasPendingConnections()) {
            QTcpSocket *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
                const auto request = socket->readAll();
                Q_UNUSED(request);
                const QByteArray response(
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n");
                socket->write(response);
                socket->disconnectFromHost();
            });
        }
    }));

    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    auto profile = harness.settings->activeProfile();
    QVERIFY(profile.has_value());
    profile->type = ProfileType::Xtream;
    profile->xtreamBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    profile->xtreamUsername = QStringLiteral("user");
    profile->xtreamPassword = QStringLiteral("pass");
    QVERIFY(harness.settings->replaceProfile(profile->id, profile.value()));

    auto channels = harness.channelListModel->allChannels();
    QVERIFY(!channels.isEmpty());
    channels[0].source = ChannelSource::Xtream;
    channels[0].id = 964;
    channels[0].catchupSupported = true;
    channels[0].catchupWindowHours = 72;
    channels[0].streamUrl = QStringLiteral("http://127.0.0.1:%1/live/user/pass/964.ts").arg(port);
    harness.channelListModel->setChannels(channels, {
        { QStringLiteral("News"), QStringLiteral("News"), 0 },
        { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
    });
    harness.guideStateModel->setChannels(channels);

    const auto programStart = QDateTime::currentDateTimeUtc().addSecs(-40 * 60);
    const auto programStop = QDateTime::currentDateTimeUtc().addSecs(20 * 60);
    const auto targetSeconds = 8 * 60.0;
    const auto channelVariant = toVariantMap(channels.first());
    const QVariantMap programVariant {
        { QStringLiteral("channelId"), channels.first().tvgId },
        { QStringLiteral("title"), QStringLiteral("Live Show") },
        { QStringLiteral("start"), programStart.toString(Qt::ISODateWithMs) },
        { QStringLiteral("stop"), programStop.toString(Qt::ISODateWithMs) }
    };

    CatchupUrlResolver resolver(profile.value());
    const auto originTarget = resolver.resolve(channels.first(), EpgEntry {
        channels.first().tvgId,
        QStringLiteral("Live Show"),
        QString(),
        QString(),
        programStart,
        programStop,
        QString()
    });
    QVERIFY(originTarget.has_value());
    static const QRegularExpression xtreamPattern(
        QStringLiteral(R"(^(.*?/timeshift/[^/?#]+/[^/?#]+/)(\d+)/(\d{4}-\d{2}-\d{2}:\d{2}-\d{2})/([^/?#]+)([?#].*)?$)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto originMatch = xtreamPattern.match(originTarget->url);
    QVERIFY(originMatch.hasMatch());
    bool parsedDurationOk = false;
    const auto originDurationMinutes = originMatch.captured(2).toLongLong(&parsedDurationOk);
    QVERIFY(parsedDurationOk);

    harness.appController->playCatchupAtOffset(channelVariant, programVariant, targetSeconds);

    QTRY_COMPARE_WITH_TIMEOUT(harness.playerController->playbackMode(), QStringLiteral("catchup"), 5000);
    const auto effectiveUrl = harness.playerController->currentPlaybackUrl();
    const auto effectiveMatch = xtreamPattern.match(effectiveUrl);
    QVERIFY(effectiveMatch.hasMatch());
    bool parsedEffectiveOk = false;
    const auto effectiveDurationMinutes = effectiveMatch.captured(2).toLongLong(&parsedEffectiveOk);
    QVERIFY(parsedEffectiveOk);
    QCOMPARE(effectiveDurationMinutes, std::max<qint64>(1, originDurationMinutes - 8));
    auto expectedTimestamp = QDateTime::fromString(originMatch.captured(3), QStringLiteral("yyyy-MM-dd:HH-mm"));
    QVERIFY(expectedTimestamp.isValid());
    expectedTimestamp.setTimeZone(QTimeZone::UTC);
    const auto shiftedExpectedTimestamp = expectedTimestamp.addSecs(8 * 60).toString(QStringLiteral("yyyy-MM-dd:HH-mm"));
    QCOMPARE(effectiveMatch.captured(3), shiftedExpectedTimestamp);
    QVERIFY(effectiveUrl.contains(QStringLiteral("/964.ts")));
    QVERIFY(harness.playerController->m_catchupSession.m_catchupEndless);
    QVERIFY(!harness.playerController->m_currentLoadfileOptions.contains(QStringLiteral("length=")));
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

    const auto programStart = QDateTime::currentDateTimeUtc().addSecs(-7200);
    const auto programStop = programStart.addSecs(3600);
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

    const auto activeProfileId = harness.settings->current().activeProfileId.value_or(QUuid {});
    QVERIFY(!activeProfileId.isNull());
    auto profile = harness.settings->profileById(activeProfileId).value_or(ServerProfile {});
    QVERIFY(!profile.id.isNull());
    profile.type = ProfileType::Xtream;
    profile.name = QStringLiteral("Xtream TZ");
    profile.xtreamBaseUrl = QStringLiteral("https://xtream.example");
    profile.xtreamUsername = QStringLiteral("alice");
    profile.xtreamPassword = QStringLiteral("secret");
    profile.xtreamServerTimezone = QStringLiteral("UTC");
    profile.m3uFilePath.clear();
    profile.m3uUrl.clear();
    QVERIFY(harness.settings->replaceProfile(profile.id, profile));
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
    QCOMPARE(
        harness.settings->profileById(profile.id).value_or(ServerProfile {}).xtreamServerTimezone,
        QStringLiteral("Asia/Dubai"));

    network->setResponse(
        authUrl,
        { QByteArrayLiteral(R"json({
            "user_info": { "auth": 1 },
            "server_info": { }
        })json"), {}, 0 });
    profileLoadSpy.clear();
    harness.appController->loadProfile(profileId);
    QTRY_VERIFY_WITH_TIMEOUT(profileLoadSpy.count() > 0, 8000);
    QCOMPARE(
        harness.settings->profileById(profile.id).value_or(ServerProfile {}).xtreamServerTimezone,
        QStringLiteral("Asia/Dubai"));
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

    const auto profileId = harness.profilesModel->activeProfileId();
    QVERIFY(!profileId.isEmpty());
    QVERIFY(harness.profilesModel->replaceProfile(profileId, {
                { QStringLiteral("type"), static_cast<int>(ProfileType::M3UUrl) },
                { QStringLiteral("m3UUrl"), url.toString() },
                { QStringLiteral("m3UFilePath"), QString() },
                { QStringLiteral("autoRefreshIntervalHours"), 1 },
                { QStringLiteral("lastRefreshed"), QDateTime::currentDateTimeUtc().addSecs(-(60 * 60) + 1).toString(Qt::ISODateWithMs) }
            }));

    harness.appController->triggerScheduledSourceAutoRefresh();
    QTest::qWait(100);
    QCOMPARE(network->callCount(url), 0);

    QVERIFY(harness.profilesModel->replaceProfile(profileId, {
                { QStringLiteral("lastRefreshed"), QDateTime::currentDateTimeUtc().addSecs(-(60 * 60)).toString(Qt::ISODateWithMs) }
            }));
    harness.shellController->openOverlay(QStringLiteral("guide"));
    QSignalSpy profileLoadSpy(harness.appController.get(), &AppController::profileLoadFinished);
    harness.appController->triggerScheduledSourceAutoRefresh();

    QTRY_COMPARE_WITH_TIMEOUT(network->callCount(url), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!profileLoadSpy.isEmpty(), 5000);
    QVERIFY(profileLoadSpy.last().at(1).toBool());
    QCOMPARE(harness.shellController->activeOverlay(), QStringLiteral("guide"));
    QVERIFY(harness.shellController->overlaysVisible());
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

    const auto profileId = harness.profilesModel->activeProfileId();
    QVERIFY(!profileId.isEmpty());
    const auto previousRefresh = QDateTime::fromString(QStringLiteral("2026-05-19T10:00:00Z"), Qt::ISODate);
    QVERIFY(previousRefresh.isValid());
    QVERIFY(harness.profilesModel->replaceProfile(profileId, {
                { QStringLiteral("type"), static_cast<int>(ProfileType::M3UUrl) },
                { QStringLiteral("m3UUrl"), url.toString() },
                { QStringLiteral("m3UFilePath"), QString() },
                { QStringLiteral("lastRefreshed"), previousRefresh.toString(Qt::ISODateWithMs) }
            }));
    harness.profilesModel->reload();

    QSignalSpy profileLoadSpy(harness.appController.get(), &AppController::profileLoadFinished);
    harness.appController->loadProfile(profileId);
    QTRY_VERIFY_WITH_TIMEOUT(profileLoadSpy.count() > 0, 8000);

    QCOMPARE(
        harness.settings->profileById(parseGuid(profileId)).value_or(ServerProfile {}).lastRefreshed,
        previousRefresh);
    QVERIFY(harness.appController->statusText().contains(QStringLiteral("Using cached channels after refresh failure")));
}

void AppModelTests::profileRefreshPreservesSettingsDraft_data()
{
    QTest::addColumn<bool>("saveDraft");
    QTest::newRow("save") << true;
    QTest::newRow("discard") << false;
}

void AppModelTests::profileRefreshPreservesSettingsDraft()
{
    QFETCH(bool, saveDraft);
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    const auto savedTransparency = harness.settings->current().uiTransparency;
    const auto savedPreset = harness.settings->current().playerPicturePreset;
    harness.shellController->openOverlay(QStringLiteral("settings"));
    QSignalSpy profileLoadSpy(harness.appController.get(), &AppController::profileLoadFinished);
    harness.appController->refreshActiveProfile();
    harness.settingsController->setUiTransparency(35);
    harness.settingsController->setPicturePreset(QStringLiteral("warm"));
    QVERIFY(harness.settingsController->dirty());
    QTRY_VERIFY_WITH_TIMEOUT(!profileLoadSpy.isEmpty(), 5000);
    QVERIFY(profileLoadSpy.last().at(1).toBool());
    QCOMPARE(harness.shellController->activeOverlay(), QStringLiteral("settings"));
    QCOMPARE(harness.settingsController->uiTransparency(), 35);
    QCOMPARE(harness.settingsController->picturePreset(), QStringLiteral("warm"));
    QVERIFY(harness.settingsController->dirty());
    QCOMPARE(harness.settings->current().uiTransparency, savedTransparency);
    QCOMPARE(harness.settings->current().playerPicturePreset, savedPreset);
    if (saveDraft) {
        harness.settingsController->save();
        QCOMPARE(harness.settings->current().uiTransparency, 35);
        QCOMPARE(harness.settings->current().playerPicturePreset, QStringLiteral("warm"));
    } else {
        harness.settingsController->cancel();
        QCOMPARE(harness.settingsController->uiTransparency(), savedTransparency);
        QCOMPARE(harness.settingsController->picturePreset(), savedPreset);
    }
    QVERIFY(!harness.settingsController->dirty());
}

void AppModelTests::invalidPlaylistRefreshKeepsCachedChannels_data()
{
    QTest::addColumn<QByteArray>("payload");
    QTest::newRow("html") << QByteArray("<html>Service unavailable</html>");
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("truncated") << QByteArray("#EXTM3U\n#EXTINF:-1,Only one\nhttp://stream/one\n#EXTINF:-1,Missing URL\n");
}

void AppModelTests::invalidPlaylistRefreshKeepsCachedChannels()
{
    QFETCH(QByteArray, payload);
    auto network = std::make_shared<MockNetworkAccess>();
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt, network));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    const auto before = harness.channelListModel->allChannels();
    QCOMPARE(before.size(), 2);
    const auto profileId = harness.profilesModel->activeProfileId();
    const auto previousRefresh = QDateTime::currentDateTimeUtc().addDays(-1);
    const QUrl url(QStringLiteral("https://example.test/invalid.m3u"));
    QVERIFY(harness.profilesModel->replaceProfile(profileId, {
        { QStringLiteral("type"), static_cast<int>(ProfileType::M3UUrl) },
        { QStringLiteral("m3UUrl"), url.toString() },
        { QStringLiteral("lastRefreshed"), previousRefresh.toString(Qt::ISODateWithMs) }
    }));
    network->setResponse(url, {payload, {}, 0});
    QSignalSpy profileLoadSpy(harness.appController.get(), &AppController::profileLoadFinished);
    harness.appController->refreshActiveProfile();
    QTRY_VERIFY_WITH_TIMEOUT(!profileLoadSpy.isEmpty(), 5000);
    const auto stored = harness.database->loadChannels(parseGuid(profileId));
    QCOMPARE(stored.size(), before.size());
    QCOMPARE(harness.channelListModel->allChannels().size(), before.size());
    for (qsizetype i = 0; i < before.size(); ++i) {
        QCOMPARE(stored[i].name, before[i].name);
        QCOMPARE(stored[i].streamUrl, before[i].streamUrl);
    }
    QCOMPARE(harness.settings->profileById(parseGuid(profileId))->lastRefreshed, previousRefresh);
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
    const auto activeProfileId = harness.settings->current().activeProfileId.value_or(QUuid {});
    QVERIFY(!activeProfileId.isNull());
    auto profile = harness.settings->profileById(activeProfileId).value_or(ServerProfile {});
    QVERIFY(!profile.id.isNull());
    profile.type = ProfileType::M3UUrl;
    profile.m3uUrl = url.toString();
    profile.m3uFilePath.clear();
    QVERIFY(harness.settings->replaceProfile(profile.id, profile));
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
    QCOMPARE(loaded.first().id, 5); // Changed URL is a new identity, after the original two and imported three.
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

void AppModelTests::multiviewStartupCleanupPreservesPlaybackAndRunsOnce()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    const auto channels = harness.channelListModel->allChannels();
    QVERIFY(channels.size() >= 2);
    QVERIFY(harness.channelListModel->activateById(channels.first().id));
    QVERIFY(harness.multiViewController->togglePictureInPicture(channels.last().id));
    const auto tiles = harness.multiViewController->tiles();
    const auto channel = harness.playerController->currentChannel();
    auto *primary = harness.playerController->player();
    auto *multiView = harness.multiViewController.get();
    QSignalSpy layoutChanges(multiView, &MultiViewController::layoutModeChanged);

    multiView->runStartupPlayerCleanup();
    QCOMPARE(multiView->m_retiredPlayers.size(), std::size_t { 1 });
    QVERIFY(!multiView->m_retiredPlayers.front()->diagnostics().isEmpty());
    multiView->runStartupPlayerCleanup();
    QCOMPARE(multiView->m_retiredPlayers.size(), std::size_t { 1 });
    QTRY_VERIFY_WITH_TIMEOUT(multiView->m_retiredPlayers.empty(), 2000);
    const auto remainingTiles = multiView->tiles();
    QCOMPARE(remainingTiles.size(), tiles.size());
    for (qsizetype index = 0; index < tiles.size(); ++index) {
        QCOMPARE(remainingTiles.at(index).toMap().value(QStringLiteral("playerObject")),
            tiles.at(index).toMap().value(QStringLiteral("playerObject")));
    }
    QCOMPARE(harness.playerController->currentChannel(), channel);
    QCOMPARE(harness.playerController->player(), primary);
    QCOMPARE(layoutChanges.count(), 0);
    multiView->runStartupPlayerCleanup();
    QVERIFY(multiView->m_retiredPlayers.empty());
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

void AppModelTests::dvrControllerWindowsStoppedJobWithoutFinishedFinalizes()
{
#if !defined(Q_OS_WIN)
    QSKIP("Windows-specific DVR job reconciliation path.");
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
    session->ingestProcess = std::make_unique<QProcess>();

    const auto sessionId = session->window.id;
    dvrController.m_sessions.insert_or_assign(sessionId, std::move(session));

    QSignalSpy stateSpy(&dvrController, &DvrController::stateChanged);

    dvrController.reconcileStoppingSession(sessionId, QStringLiteral("unit-test"));
    QVERIFY(dvrController.m_sessions.find(sessionId) == dvrController.m_sessions.end());
    QVERIFY(stateSpy.count() > 0);
#endif
}

void AppModelTests::dvrControllerWindowsWaitsForDescendants()
{
#if !defined(Q_OS_WIN)
    QSKIP("Windows-specific DVR job reconciliation path.");
#else
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    PlayerController playerController;
    DvrController controller(&settings, &playerController);
    controller.m_tickTimer.stop();

    auto session = std::make_unique<DvrController::Session>();
    session->window.id = QStringLiteral("owned-tree");
    session->state = DvrController::SessionState::Stopping;
    session->stopRequested = true;
    session->ingestProcess = std::make_unique<QProcess>();
    auto *process = session->ingestProcess.get();
    QString error;
    QVERIFY2(session->ingestJob.configure(*process, &error), qPrintable(error));
    process->start(QDir(QCoreApplication::applicationDirPath()).filePath(
                       QStringLiteral("OKILTVQtWindowsProcessJobTests.exe")),
                   { QStringLiteral("--tree-exit") });
    QVERIFY2(process->waitForStarted(5000), qPrintable(process->errorString()));
    QVERIFY(process->waitForReadyRead(5000));
    QVERIFY(process->waitForFinished(5000));
    // No finished signal is connected to the controller, and the direct PID is
    // gone. Its child still belongs to the job and must be stopped before removal.
    QVERIFY(session->ingestJob.hasActiveProcesses());
    const auto id = session->window.id;
    controller.m_sessions.insert_or_assign(id, std::move(session));
    controller.reconcileStoppingSession(id, QStringLiteral("test-parent-exit"));
    QVERIFY(controller.m_sessions.contains(id));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.m_sessions.contains(id), 5000);
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

void AppModelTests::channelListModelRestoresSavedGroup()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("settings.json"));
    const auto sourceA = QStringLiteral("source-a");
    const auto sourceB = QStringLiteral("source-b");
    const auto newsId = QStringLiteral("News");
    const auto sportsId = QStringLiteral("Sports");
    Channel news;
    news.id = 1;
    news.categoryId = newsId;
    Channel sports;
    sports.id = 2;
    sports.categoryId = sportsId;
    const QList<Channel> channels { news, sports };
    const QList<ChannelCategory> categories {
        { newsId, newsId, 0 }, { sportsId, sportsId, 0 }
    };
    {
        SettingsManager settings(path);
        ChannelListModel model(&settings);
        model.setActiveProfileId(sourceA);
        model.setChannels(channels, categories);
        model.setSelectedCategoryId(sportsId);
        model.clear(); // Source loading must not erase the saved selection.
        model.setActiveProfileId(sourceB);
        model.setChannels(channels, categories);
        QCOMPARE(model.selectedCategoryId(), QString {});
        model.setSelectedCategoryId(newsId);
    }
    SettingsManager settings(path);
    settings.load();
    ChannelListModel model(&settings);
    model.setActiveProfileId(sourceA);
    model.setChannels(channels, categories);
    QCOMPARE(model.selectedCategoryId(), sportsId);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.index(0).data(ChannelListModel::IdRole).toInt(), sports.id);
    model.setActiveProfileId(sourceB);
    model.setChannels(channels, categories);
    QCOMPARE(model.selectedCategoryId(), newsId);
    model.setSelectedCategoryId(QStringLiteral("__favourites__"));
    settings.load();
    model.clear();
    model.setChannels(channels, categories);
    QCOMPARE(model.selectedCategoryId(), QStringLiteral("__favourites__"));
    model.setSelectedCategoryId(QString {});
    settings.load();
    model.setChannels(channels, categories);
    QCOMPARE(model.selectedCategoryId(), QString {});
    model.setActiveProfileId(sourceA);
    model.setChannels({ news }, { categories.first() });
    QCOMPARE(model.selectedCategoryId(), QString {}); // Saved group was removed.
    model.setChannels(channels, categories);
    model.setSelectedCategoryId(sportsId);
    settings.current().hiddenGroupsByProfile[sourceA] = { sportsId };
    settings.save();
    settings.load();
    model.clear();
    model.setChannels(channels, categories);
    QCOMPARE(model.selectedCategoryId(), QString {});
    QVERIFY(settings.lastSaveError().isEmpty());
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

void AppModelTests::channelListModelWatchUpdatesPreserveFavouriteRows()
{
    QTemporaryDir tempDir;
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    ChannelListModel model(&settings);
    QList<Channel> channels;
    for (int id = 1; id <= 3; ++id) {
        Channel channel;
        channel.id = id;
        channel.name = QString::number(id);
        channel.sortOrder = id;
        channels.append(channel);
    }
    model.setChannels(channels, {});
    model.setWatchSeconds({ { 1, 30000 }, { 2, 25000 } });
    model.setSelectedCategoryId(QStringLiteral("__favourites__"));
    QVERIFY(model.selectById(2));
    QPersistentModelIndex selectedIndex(model.index(1, 0));
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy moves(&model, &QAbstractItemModel::rowsMoved);
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy selection(&model, &ChannelListModel::selectedChannelIdChanged);

    model.setWatchSeconds({ { 1, 30001 }, { 2, 25000 } });
    QCOMPARE(resets.count(), 0);
    QCOMPARE(moves.count(), 0);
    QCOMPARE(selectedIndex.row(), 1);

    model.setWatchSeconds({ { 1, 30001 }, { 2, 31000 }, { 3, 22000 } });
    QCOMPARE(resets.count(), 0);
    QCOMPARE(moves.count(), 1);
    QCOMPARE(inserted.count(), 1);
    QCOMPARE(model.filteredCount(), 3);
    QCOMPARE(selectedIndex.row(), 0);
    QCOMPARE(selectedIndex.data(ChannelListModel::IdRole).toInt(), 2);

    model.setWatchSeconds({ { 2, 31001 }, { 3, 22000 } });
    QCOMPARE(resets.count(), 0);
    QCOMPARE(removed.count(), 1);
    QCOMPARE(model.filteredCount(), 2);
    QCOMPARE(model.selectedChannelId(), 2);
    QCOMPARE(selectedIndex.data(ChannelListModel::IdRole).toInt(), 2);
    QCOMPARE(selection.count(), 0);
}

void AppModelTests::channelListModelReplacementIsConsistentDuringNotifications_data()
{
    QTest::addColumn<bool>("filtered");
    QTest::addColumn<bool>("emptyReplacement");
    QTest::newRow("all-channels") << false << false;
    QTest::newRow("filtered-channels") << true << false;
    QTest::newRow("empty-source") << false << true;
    QTest::newRow("filtered-empty-source") << true << true;
}

void AppModelTests::channelListModelReplacementIsConsistentDuringNotifications()
{
    QFETCH(bool, filtered);
    QFETCH(bool, emptyReplacement);
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    ChannelListModel model(&settings);

    const auto makeChannels = [](const int count, const int firstId) {
        QList<Channel> channels;
        for (auto i = 0; i < count; ++i) {
            Channel channel;
            channel.id = firstId + i;
            channel.name = QStringLiteral("Channel %1").arg(channel.id);
            channel.sortOrder = i + 1;
            channel.categoryId = i % 2 == 0 ? QStringLiteral("News") : QStringLiteral("Sports");
            channels.push_back(channel);
        }
        return channels;
    };
    const QList<ChannelCategory> categories {
        { QStringLiteral("News"), QStringLiteral("News"), 0 },
        { QStringLiteral("Sports"), QStringLiteral("Sports"), 0 }
    };
    model.setChannels(makeChannels(4096, 0), categories);
    if (filtered) {
        model.setSelectedCategoryId(QStringLiteral("News"));
    }
    QVERIFY(model.selectById(4094));
    const auto oldRowCount = filtered ? 2048 : 4096;
    QCOMPARE(model.rowCount(), oldRowCount);

    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy countSpy(&model, &ChannelListModel::filteredCountChanged);
    connect(&model, &QAbstractItemModel::modelAboutToBeReset, &model, [&]() {
        QCOMPARE(model.rowCount(), oldRowCount);
        QCOMPARE(model.rowForChannelId(4094), filtered ? 2047 : 4094);
        QCOMPARE(model.index(0, 0).data(ChannelListModel::IdRole).toInt(), 0);
    });

    const QList<int> expectedIds = emptyReplacement ? QList<int> {}
        : filtered ? QList<int> { 10000, 10002 } : QList<int> { 10000, 10001, 10002 };
    auto notificationCount = 0;
    const auto verifyReplacement = [&]() {
        ++notificationCount;
        QCOMPARE(model.rowCount(), static_cast<int>(expectedIds.size()));
        QCOMPARE(model.filteredCount(), static_cast<int>(expectedIds.size()));
        QCOMPARE(model.totalCount(), emptyReplacement ? 0 : 3);
        QCOMPARE(model.selectedChannelId(), -1);
        // QML asks for the selected row synchronously, even when selection is -1.
        QCOMPARE(model.rowForChannelId(model.selectedChannelId()), -1);
        QCOMPARE(model.rowForChannelId(4094), -1);
        for (auto row = 0; row < expectedIds.size(); ++row) {
            const auto id = expectedIds.at(row);
            QCOMPARE(model.index(row, 0).data(ChannelListModel::IdRole).toInt(), id);
            QCOMPARE(model.rowForChannelId(id), row);
        }
    };
    connect(&model, &QAbstractItemModel::modelReset, &model, verifyReplacement);
    connect(&model, &ChannelListModel::totalCountChanged, &model, verifyReplacement);
    connect(&model, &ChannelListModel::selectedChannelIdChanged, &model, verifyReplacement);
    connect(&model, &ChannelListModel::categoriesChanged, &model, verifyReplacement);
    connect(&model, &ChannelListModel::filteredCountChanged, &model, verifyReplacement);

    model.setChannels(makeChannels(emptyReplacement ? 0 : 3, 10000), categories);
    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(countSpy.count(), 1);
    QCOMPARE(notificationCount, 5);
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

void AppModelTests::startupGroupSyncDoesNotDecryptChannelUrls()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    const auto profile = harness.activeProfileId();
    const auto key = guidToString(profile);
    Channel channel;
    channel.id = 1;
    channel.profileId = profile;
    channel.name = QStringLiteral("Fixture");
    channel.categoryId = QStringLiteral("Sports");
    channel.streamUrl = QStringLiteral("https://fixture.invalid/secret");
    harness.database->upsertChannels({ channel });
    const auto connectionName = QStringLiteral("startup-group-metadata-fixture");
    {
        auto raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        raw.setDatabaseName(harness.database->databaseFilePath());
        QVERIFY(raw.open());
        QSqlQuery query(raw);
        QVERIFY(query.exec(QStringLiteral("UPDATE channels SET stream_url='okiltv-secret:v99:unavailable'")));
    }
    QSqlDatabase::removeDatabase(connectionName);
    // Keep the source inactive: startup must reconcile its groups without
    // loading any playback data, even when its cached URLs cannot be decrypted.
    harness.settings->setActiveProfileId(std::nullopt);
    harness.settings->current().hiddenGroupsByProfile[key] = { QStringLiteral("Sports") };
    harness.appController->initialize();
    QCOMPARE(harness.settings->current().groupOrderByProfile.value(key),
        (QStringList { QStringLiteral("__favourites__"), QStringLiteral("Sports") }));
    QCOMPARE(harness.settings->current().hiddenGroupsByProfile.value(key), QStringList { QStringLiteral("Sports") });
    QVERIFY(!harness.appController->isBusy());
}

void AppModelTests::sourceGroupsThreshold_data()
{
    QTest::addColumn<int>("count");
    QTest::addColumn<int>("selected");
    QTest::newRow("49 plus favourites") << 49 << 50;
    QTest::newRow("50 plus favourites") << 50 << 51;
    QTest::newRow("51 plus favourites") << 51 << 1;
}

void AppModelTests::sourceGroupsThreshold()
{
    QFETCH(int, count);
    QFETCH(int, selected);
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    QList<Channel> channels;
    for (int i = 0; i < count; ++i) {
        Channel channel;
        channel.id = i;
        channel.profileId = harness.activeProfileId();
        channel.name = QStringLiteral("Fixture");
        channel.streamUrl = QStringLiteral("http://127.0.0.1:1/fixture");
        // Include Ungrouped in the threshold, and duplicate one group below.
        channel.categoryId = i == 0 ? QString() : QString::number(i);
        channels.push_back(channel);
    }
    auto duplicate = channels.last();
    duplicate.id = count;
    channels.push_back(duplicate);
    harness.database->replaceChannelsForProfile(harness.activeProfileId(), channels);
    const auto id = guidToString(harness.activeProfileId());
    SourceGroupsModel model(harness.settings.get(), harness.database.get());
    model.setProfileId(id);
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.totalCount(), count + 1);
    QCOMPARE(model.selectedCount(), selected);
    const auto hidden = harness.settings->current().hiddenGroupsByProfile.value(id);
    // The controller and model must agree without reselecting old hidden groups.
    QVERIFY(!harness.appController->syncProfileGroupPreferences(harness.activeProfileId(), channels));
    QCOMPARE(harness.settings->current().hiddenGroupsByProfile.value(id), hidden);
    QVERIFY(model.setGroupSelected(QStringLiteral("__favourites__"), false));
    model.reload();
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.selectedCount(), selected - 1);
    SettingsManager reloaded(harness.settingsPath);
    reloaded.load();
    QCOMPARE(reloaded.current().hiddenGroupsByProfile.value(id),
        harness.settings->current().hiddenGroupsByProfile.value(id));
}

void AppModelTests::sourceGroupsRefreshPreservesDrafts()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    const auto id = guidToString(harness.activeProfileId());
    QList<Channel> channels;
    const auto appendGroup = [&]() {
        Channel channel;
        channel.id = static_cast<int>(channels.size());
        channel.profileId = harness.activeProfileId();
        channel.name = QStringLiteral("Fixture");
        channel.streamUrl = QStringLiteral("http://127.0.0.1:1/fixture");
        channel.categoryId = QString::number(channel.id);
        channels.push_back(channel);
        harness.database->replaceChannelsForProfile(harness.activeProfileId(), channels);
    };
    for (int i = 0; i < 49; ++i) {
        appendGroup();
    }
    SourceGroupsModel model(harness.settings.get(), harness.database.get());
    model.setProfileId(id);
    QTRY_VERIFY(!model.loading());
    model.setAutoPersist(false);
    QVERIFY(model.setGroupSelected(QStringLiteral("0"), false));
    QVERIFY(model.moveGroup(QStringLiteral("1"), 0));
    appendGroup(); // 50 source groups: the new group is selected.
    harness.appController->syncProfileGroupPreferences(harness.activeProfileId(), channels);
    model.reload();
    // A choice made while the reload is pending must also survive.
    QVERIFY(model.setGroupSelected(QStringLiteral("2"), false));
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.selectedCount(), 49); // 50 + Favourites - 2 manual exclusions.
    QCOMPARE(model.get(0).value(QStringLiteral("id")).toString(), QStringLiteral("1"));
    QVERIFY(model.dirty());
    appendGroup(); // 51 source groups: only the newly discovered group is hidden.
    harness.appController->syncProfileGroupPreferences(harness.activeProfileId(), channels);
    model.reload();
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.selectedCount(), 49);
    model.saveDraftChanges();
    const auto hidden = harness.settings->current().hiddenGroupsByProfile.value(id);
    QVERIFY(hidden.contains(QStringLiteral("0")));
    QVERIFY(hidden.contains(QStringLiteral("2")));
    QVERIFY(hidden.contains(QStringLiteral("50")));
    QVERIFY(!hidden.contains(QStringLiteral("49")));
    QCOMPARE(harness.settings->current().groupOrderByProfile.value(id).first(), QStringLiteral("1"));
    model.reload();
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.selectedCount(), 49);
    QVERIFY(!model.dirty());
}

void AppModelTests::sourceGroupImportNotices()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    const auto id = guidToString(harness.activeProfileId());
    const auto writePlaylist = [&](int count) {
        QFile file(harness.playlistPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        file.write("#EXTM3U\n");
        for (int i = 0; i < count; ++i) {
            file.write(QStringLiteral("#EXTINF:-1 group-title=\"Group %1\",Channel %1\nhttp://127.0.0.1:1/%1\n")
                .arg(i).toUtf8());
        }
        return true;
    };
    QSignalSpy finished(harness.appController.get(), &AppController::profileLoadFinished);
    QSignalSpy notices(harness.appController.get(), &AppController::groupAutoEnableNoticesChanged);
    QVERIFY(writePlaylist(50));
    harness.appController->loadProfile(id);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 8000);
    QVERIFY(harness.appController->groupAutoEnableNoticeProfileIds().isEmpty());
    QVERIFY(harness.settings->current().hiddenGroupsByProfile.value(id).isEmpty());
    QVERIFY(writePlaylist(51));
    harness.appController->loadProfile(id);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 8000);
    QCOMPARE(harness.appController->groupAutoEnableNoticeProfileIds(), QStringList { id });
    QCOMPARE(harness.settings->current().hiddenGroupsByProfile.value(id), QStringList { QStringLiteral("Group 50") });
    QCOMPARE(notices.count(), 1);

    const auto other = harness.profilesModel->addM3uFileProfile(QStringLiteral("Other"), harness.playlistPath);
    QVERIFY(!other.isEmpty());
    harness.appController->loadProfile(other);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 8000);
    QVERIFY(harness.appController->groupAutoEnableNoticeProfileIds().contains(other));
    SourceGroupsModel model(harness.settings.get(), harness.database.get());
    connect(&model, &SourceGroupsModel::selectionEdited,
        harness.appController.get(), &AppController::dismissGroupAutoEnableNotice);
    model.setProfileId(id);
    QTRY_VERIFY(!model.loading());
    model.setSearchText(QStringLiteral("Group"));
    model.setHideUnchecked(true);
    QVERIFY(model.moveGroup(QStringLiteral("Group 1"), 0));
    QVERIFY(harness.appController->groupAutoEnableNoticeProfileIds().contains(id));
    QVERIFY(!model.setGroupSelected(QStringLiteral("Group 0"), true));
    QVERIFY(harness.appController->groupAutoEnableNoticeProfileIds().contains(id));
    QVERIFY(model.selectAll());
    QVERIFY(!harness.appController->groupAutoEnableNoticeProfileIds().contains(id));
    QVERIFY(harness.appController->groupAutoEnableNoticeProfileIds().contains(other));
    model.setProfileId(other);
    QTRY_VERIFY(!model.loading());
    QVERIFY(model.setGroupSelected(QStringLiteral("Group 0"), true));
    QVERIFY(harness.appController->groupAutoEnableNoticeProfileIds().isEmpty());

    harness.appController->loadProfile(id); // No new groups: do not restore the notice.
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 4, 8000);
    QVERIFY(harness.appController->groupAutoEnableNoticeProfileIds().isEmpty());
    QVERIFY(QFile::remove(harness.playlistPath));
    harness.appController->loadProfile(id); // Cached fallback is not a successful import.
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 5, 8000);
    QVERIFY(harness.appController->groupAutoEnableNoticeProfileIds().isEmpty());
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
    QVERIFY(settings.addProfile(profileA));
    QVERIFY(settings.addProfile(profileB));

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
    for (int index = 0; index < 51; ++index) {
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
    QTRY_COMPARE_WITH_TIMEOUT(model.totalCount(), 52, 3000);
    QCOMPARE(model.get(0).value(QStringLiteral("id")).toString(), QStringLiteral("__favourites__"));
    QCOMPARE(model.selectedCount(), 1);
    QCOMPARE(settings.current().hiddenGroupsByProfile.value(profileBKey).size(), 51);
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
    QVERIFY(settings.addProfile(profile));

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

    const auto originalOrder = settings.current().groupOrderByProfile.value(profileKey);
    model.setHideUnchecked(true);
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
    QCOMPARE(settings.current().groupOrderByProfile.value(profileKey), originalOrder);
    model.discardDraftChanges();
    QTRY_COMPARE_WITH_TIMEOUT(model.loading(), false, 3000);
    QVERIFY(!model.dirty());
    QCOMPARE(model.get(0).value(QStringLiteral("id")).toString(), originalOrder.first());
    model.setHideUnchecked(true);
    QVERIFY(model.reorderVisibleGroups({
        QStringLiteral("Movies"), QStringLiteral("__favourites__"), QStringLiteral("News")
    }));
    model.saveDraftChanges();
    QVERIFY(!model.dirty());

    const auto persistedOrder = settings.current().groupOrderByProfile.value(profileKey);
    QCOMPARE(persistedOrder.value(0), QStringLiteral("Movies"));
    QCOMPARE(persistedOrder.value(1), QStringLiteral("__favourites__"));
    QCOMPARE(persistedOrder.value(2), QStringLiteral("News"));
    QCOMPARE(persistedOrder.value(3), QStringLiteral("Sports"));
    QCOMPARE(persistedOrder.value(4), QStringLiteral("Kids"));
    model.reload();
    QTRY_COMPARE_WITH_TIMEOUT(model.loading(), false, 3000);
    QCOMPARE(model.visibleGroupIds(), QStringList({
        QStringLiteral("Movies"), QStringLiteral("__favourites__"), QStringLiteral("News")
    }));
    QVERIFY(!model.dirty());
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
    QVERIFY(settings.addProfile(profile));

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

void AppModelTests::epgGridModelRefreshPreservesViewport()
{
    EpgService epg;
    EpgGridModel model(&epg);
    QList<Channel> channels;
    QList<EpgEntry> entries;
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-900);
    const auto profile = QUuid::createUuid();
    for (int i = 0; i < 80; ++i) {
        Channel channel;
        channel.id = i;
        channel.profileId = profile;
        channel.name = QString::number(i);
        channel.tvgId = channel.name;
        channels.append(channel);
        entries.append(EpgEntry { channel.tvgId, QStringLiteral("Before"), {}, {}, start, start.addSecs(7200) });
    }
    epg.loadFromEntries(entries);
    model.rebuild(channels, 6, 24);
    model.setSelectedChannelId(2); // Selection deliberately outside the scrolled viewport.
    model.setSelectedProgramStart(start.toString(Qt::ISODateWithMs));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("gridModel"), &model);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        Flickable {
            width: 800; height: 400; contentWidth: 5000
            flickableDirection: Flickable.HorizontalFlick
            property alias rows: rows
            ListView {
                id: rows
                width: parent.contentWidth; height: parent.height
                model: gridModel
                cacheBuffer: 1600
                delegate: Item { width: 5000; height: 76 }
            }
        }
    )", QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> view(component.create());
    QVERIFY(view);
    auto *rows = qvariant_cast<QObject *>(view->property("rows"));
    QVERIFY(rows);
    QVERIFY(QMetaObject::invokeMethod(rows, "forceLayout"));
    view->setProperty("contentX", 1250.0);
    rows->setProperty("contentY", 2307.0); // Partial row offset must survive too.
    QCoreApplication::processEvents();
    const auto beforeY = rows->property("contentY").toDouble();
    QVERIFY(beforeY > 2000.0);
    QPersistentModelIndex persistent(model.index(30));
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy changeSpy(&model, &QAbstractItemModel::dataChanged);

    entries[30].title = QStringLiteral("After");
    channels[30].name = QStringLiteral("Updated channel");
    epg.loadFromEntries(entries);
    model.rebuildAsync(channels, 6, 24);
    QTRY_VERIFY(!changeSpy.isEmpty());
    QVERIFY(QMetaObject::invokeMethod(rows, "forceLayout"));
    QCoreApplication::processEvents();
    QCOMPARE(resetSpy.count(), 0);
    QVERIFY(persistent.isValid());
    QCOMPARE(rows->property("contentY").toDouble(), beforeY);
    QCOMPARE(view->property("contentX").toDouble(), 1250.0);
    QCOMPARE(model.selectedChannelId(), 2);
    QCOMPARE(model.selectedProgramStart(), start.toString(Qt::ISODateWithMs));
    QCOMPARE(model.data(persistent, EpgGridModel::ChannelNameRole).toString(), QStringLiteral("Updated channel"));
    QCOMPARE(model.programForChannelAtTimestamp(30, start.toString(Qt::ISODateWithMs)).value(QStringLiteral("title")).toString(), QStringLiteral("After"));

    // A different source can reuse channel IDs, but must not reuse row identity.
    for (auto &channel : channels) {
        channel.profileId = QUuid::createUuid();
    }
    model.rebuild(channels, 6, 24);
    QCOMPARE(resetSpy.count(), 1);
    QVERIFY(!persistent.isValid());
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
    for (int hour = -999; hour < 999; ++hour) {
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
    model.rebuildAsync({ channel }, 999, 999);
    QTRY_COMPARE(model.windowSpanMinutes(), 1998 * 60);
    QCOMPARE(model.rowCount(), 1);
    model.setVisibleRowRange(0, 0);
    model.setRenderViewport(998 * 60, 90);

    const auto modelIndex = model.index(0, 0);
    const auto initialPrograms = model.data(modelIndex, EpgGridModel::ProgramsRole).toList();
    QVERIFY(!initialPrograms.isEmpty());
    QVERIFY(initialPrograms.size() < entries.size());
    QVERIFY(initialPrograms.size() <= 7);
    QVERIFY(model.visibleTimeSlots().size() <= 9);

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
    QVERIFY(hasTitle(leftEdgePrograms, QStringLiteral("Slot -999")));
    QVERIFY(leftEdgePrograms.size() <= 7);
    QVERIFY(!hasTitle(leftEdgePrograms, QStringLiteral("Slot 6")));

    model.setRenderViewport(1006 * 60, 90);
    const auto shiftedPrograms = model.data(modelIndex, EpgGridModel::ProgramsRole).toList();
    QVERIFY(!shiftedPrograms.isEmpty());
    QVERIFY(hasTitle(shiftedPrograms, QStringLiteral("Slot 7")));
    QVERIFY(!hasTitle(shiftedPrograms, QStringLiteral("Slot -6")));
    model.setRenderViewport(1997 * 60, 60);
    const auto rightEdgePrograms = model.data(modelIndex, EpgGridModel::ProgramsRole).toList();
    QVERIFY(hasTitle(rightEdgePrograms, QStringLiteral("Slot 998")));
    QVERIFY(rightEdgePrograms.size() <= 7);
}

void AppModelTests::appControllerGuideRebuildUsesConfiguredPastAndFutureRanges()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));

    harness.settings->current().guidePastHours = 4;
    harness.settings->current().epgLookAheadHours = 10;

    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);

    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->guidePastHours(), 4, 5000);
    QCOMPARE(harness.epgGridModel->lookAheadHours(), 10);
    QCOMPARE(harness.epgGridModel->windowSpanMinutes(), (4 + 10) * 60);

    harness.settingsController->setGuidePastHours(168);
    harness.settingsController->setEpgLookAheadHours(240);
    harness.settingsController->save();

    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->guidePastHours(), 168, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->lookAheadHours(), 240, 5000);
    QCOMPARE(harness.epgGridModel->windowSpanMinutes(), (168 + 240) * 60);
}

void AppModelTests::guideGridFilteringStaysIndependentFromLiveSearch()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));

    harness.appController->initialize();

    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->epgRefreshInProgress(), 5000);
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

    harness.channelListModel->setSelectedCategoryId(QStringLiteral("Sports"));
    QCOMPARE(harness.guideStateModel->selectedGroupId(), QStringLiteral("Sports"));
    QVERIFY(harness.epgGridModel->rebuildPending());
    QTRY_VERIFY_WITH_TIMEOUT(!harness.epgGridModel->rebuildPending(), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->rowCount(), 1, 2000);
    QCOMPARE(harness.epgGridModel->channelIdAt(0), sportsChannel->id);

    harness.channelListModel->setSelectedCategoryId(QStringLiteral("News"));
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->rowCount(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->channelIdAt(0), newsChannel->id, 2000);

    harness.channelListModel->setSearchText(QStringLiteral("Channel Two"));
    QCOMPARE(harness.channelListModel->filteredCount(), 0);
    QCOMPARE(harness.epgGridModel->rowCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->channelIdAt(0), newsChannel->id, 2000);

    harness.channelListModel->setWatchSeconds({
        { sportsChannel->id, 6 * 60 * 60 }
    });
    harness.channelListModel->setSelectedCategoryId(QStringLiteral("__favourites__"));
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->channelIdAt(0), sportsChannel->id, 2000);

    harness.channelListModel->setSelectedCategoryId(QString {});
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->rowCount(), 2, 2000);
    QVERIFY(harness.epgGridModel->rowIndexForChannelId(newsChannel->id) >= 0);
    QVERIFY(harness.epgGridModel->rowIndexForChannelId(sportsChannel->id) >= 0);
    QCOMPARE(harness.channelListModel->filteredCount(), 1);

    QVERIFY(harness.channelListModel->setCategoryHidden(QStringLiteral("News"), true));
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->rowCount(), 1, 2000);
    QCOMPARE(harness.epgGridModel->channelIdAt(0), sportsChannel->id);

    harness.channelListModel->setSelectedCategoryId(QStringLiteral("Missing"));
    QTRY_COMPARE_WITH_TIMEOUT(harness.epgGridModel->rowCount(), 0, 2000);
    QVERIFY(!harness.epgGridModel->rebuildPending());

    harness.channelListModel->setSelectedCategoryId(QStringLiteral("Sports"));
    harness.channelListModel->setSelectedCategoryId(QStringLiteral("News"));
    harness.channelListModel->setSelectedCategoryId(QStringLiteral("Sports"));
    QTRY_VERIFY_WITH_TIMEOUT(!harness.epgGridModel->rebuildPending(), 2000);
    QCOMPARE(harness.epgGridModel->rowCount(), 1);
    QCOMPARE(harness.epgGridModel->channelIdAt(0), sportsChannel->id);
    QVERIFY(harness.playerController->currentChannel().isEmpty());
}

void AppModelTests::guideGridInvalidatesQueuedResultsBeforeReplacement()
{
    EpgService epg;
    EpgGridModel model(&epg);
    Channel first;
    first.id = 1;
    first.tvgId = QStringLiteral("first");
    Channel second;
    second.id = 2;
    second.tvgId = QStringLiteral("second");
    const auto now = QDateTime::currentDateTimeUtc();
    epg.loadFromEntries({
        EpgEntry { first.tvgId, QStringLiteral("First"), {}, {}, now.addSecs(-60), now.addSecs(3600) },
        EpgEntry { second.tvgId, QStringLiteral("Second"), {}, {}, now.addSecs(-60), now.addSecs(3600) }
    });
    model.rebuild({ first }, 6, 24);
    model.rebuildAsync({ second }, 6, 24);
    model.invalidateRebuild();
    // Let the old worker finish before submitting its replacement: its queued
    // result must not become visible during the controller's coalescing delay.
    QVERIFY(QThreadPool::globalInstance()->waitForDone(2000));
    QCoreApplication::processEvents();
    QVERIFY(model.rebuildPending());
    QCOMPARE(model.channelIdAt(0), first.id);
    model.rebuildAsync({}, 6, 24);
    QTRY_VERIFY_WITH_TIMEOUT(!model.rebuildPending(), 2000);
    QCOMPARE(model.rowCount(), 0);
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

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    NowNextModel model(&epg, &settings);
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

void AppModelTests::nowNextModelArchiveHistory()
{
    EpgService epg;
    const auto now = QDateTime::currentDateTimeUtc();
    const auto entry = [&now](const QString &title, int start, int stop) {
        return EpgEntry { QStringLiteral("archive"), title, {}, {}, now.addSecs(start), now.addSecs(stop) };
    };
    const auto previous = entry(QStringLiteral("Previous"), -7200, -3600);
    epg.loadFromEntries({
        entry(QStringLiteral("Expired"), -26 * 3600, -25 * 3600),
        entry(QStringLiteral("Overlaps oldest edge"), -25 * 3600, -23 * 3600),
        entry(QStringLiteral("Older than Guide history"), -22 * 3600, -21 * 3600),
        previous, previous,
        entry(QStringLiteral("Current"), -3600, 3600),
        entry(QStringLiteral("Next"), 3600, 7200),
        entry(QStringLiteral("Future"), 7200, 10800)
    });
    QTemporaryDir tempDir;
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    Channel channel;
    channel.id = 1;
    channel.profileId = QUuid::createUuid();
    channel.tvgId = QStringLiteral("archive");
    channel.catchupSupported = true;
    channel.catchupWindowHours = 24;
    NowNextModel model(&epg, &settings);
    model.setChannel(channel);
    QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 3000);
    QCOMPARE(model.pastPrograms().size(), 2);
    QCOMPARE(model.pastPrograms().first().toMap().value(QStringLiteral("title")).toString(),
        QStringLiteral("Older than Guide history"));
    QCOMPARE(model.pastPrograms().last().toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Previous"));
    QCOMPARE(model.currentProgram().value(QStringLiteral("title")).toString(), QStringLiteral("Current"));
    QCOMPARE(model.nextProgram().value(QStringLiteral("title")).toString(), QStringLiteral("Next"));
    QCOMPARE(model.upcomingPrograms().size(), 1);

    channel.catchupSupported = false;
    model.setChannel(channel);
    QVERIFY(model.pastPrograms().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 3000);
    QVERIFY(model.pastPrograms().isEmpty());
    channel.catchupSupported = true;
    channel.catchupWindowHours = 0;
    model.setChannel(channel);
    QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 3000);
    QVERIFY(model.pastPrograms().isEmpty());
}

void AppModelTests::nowNextModelHistoryDoesNotLeakAcrossChannels()
{
    EpgService epg;
    const auto now = QDateTime::currentDateTimeUtc();
    epg.loadFromEntries({ EpgEntry { QStringLiteral("shared.id"), QStringLiteral("History"), {}, {},
        now.addSecs(-7200), now.addSecs(-3600) } });
    QTemporaryDir tempDir;
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    Channel first;
    first.id = 1;
    first.profileId = QUuid::createUuid();
    first.tvgId = QStringLiteral("shared.id");
    first.catchupSupported = true;
    first.catchupWindowHours = 24;
    NowNextModel model(&epg, &settings);
    model.setChannel(first);
    QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 3000);
    QCOMPARE(model.pastPrograms().size(), 1);
    QVERIFY(model.currentProgram().isEmpty());
    QVERIFY(model.nextProgram().isEmpty());

    Channel second = first;
    second.profileId = QUuid::createUuid();
    epg.clear();
    model.setChannel(second);
    // Same tvgId in another source must not hit the previous profile's result cache.
    QVERIFY(model.pastPrograms().isEmpty());
    QCOMPARE(model.channel().value(QStringLiteral("profileId")), toVariantMap(second).value(QStringLiteral("profileId")));
    QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 3000);
    QVERIFY(model.pastPrograms().isEmpty());

    model.setChannel(first);
    model.setChannel(second);
    model.setChannel(std::nullopt);
    QTRY_VERIFY_WITH_TIMEOUT(!model.m_refreshInFlight, 3000);
    QVERIFY(model.channel().isEmpty());
    QVERIFY(model.pastPrograms().isEmpty());
    QVERIFY(model.currentProgram().isEmpty());
}

void AppModelTests::nowNextModelUsesConfiguredLookAhead()
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
            QStringLiteral("Beyond 24 hours"),
            QString {},
            QString {},
            now.addSecs(25 * 60 * 60),
            now.addSecs(26 * 60 * 60)
        }
    });

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    settings.current().epgLookAheadHours = 30;

    Channel channel;
    channel.id = 1;
    channel.name = QStringLiteral("Channel One");
    channel.tvgId = QStringLiteral("channel.one");

    NowNextModel model(&epg, &settings);
    model.setChannel(channel);

    QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), 3000);
    const auto upcoming = model.upcomingPrograms();
    QCOMPARE(upcoming.size(), 1);
    QCOMPARE(upcoming.first().toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Beyond 24 hours"));
}

void AppModelTests::appControllerRefreshesNowNextWhenLookAheadIsSaved()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();

    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->epgRefreshInProgress(), 5000);
    const auto channel = harness.channelListModel->channelById(0);
    QVERIFY(channel.has_value());

    const auto now = QDateTime::currentDateTimeUtc();
    harness.epgService->loadFromEntries({
        EpgEntry {
            channel->tvgId,
            QStringLiteral("Current"),
            QString {},
            QString {},
            now.addSecs(-20 * 60),
            now.addSecs(20 * 60)
        },
        EpgEntry {
            channel->tvgId,
            QStringLiteral("Next"),
            QString {},
            QString {},
            now.addSecs(20 * 60),
            now.addSecs(50 * 60)
        },
        EpgEntry {
            channel->tvgId,
            QStringLiteral("Beyond 24 hours"),
            QString {},
            QString {},
            now.addSecs(25 * 60 * 60),
            now.addSecs(26 * 60 * 60)
        }
    });

    harness.nowNextModel->setChannel(channel);
    QTRY_VERIFY_WITH_TIMEOUT(!harness.nowNextModel->loading(), 3000);
    QVERIFY(harness.nowNextModel->upcomingPrograms().isEmpty());

    harness.settingsController->setEpgLookAheadHours(30);
    harness.settingsController->save();

    QTRY_VERIFY_WITH_TIMEOUT(!harness.nowNextModel->loading(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.nowNextModel->upcomingPrograms().size(), 1, 3000);
    QCOMPARE(
        harness.nowNextModel->upcomingPrograms().first().toMap().value(QStringLiteral("title")).toString(),
        QStringLiteral("Beyond 24 hours"));
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

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    NowNextModel model(&epg, &settings);
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

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    NowNextModel model(&epg, &settings);
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

void AppModelTests::guideStateModelRefreshPreservesBrowsedProgram_data()
{
    QTest::addColumn<int>("offsetHours");
    QTest::newRow("past-outside-preview") << -4;
    QTest::newRow("future") << 4;
    QTest::newRow("future-outside-preview") << 30;
}

void AppModelTests::guideStateModelRefreshPreservesBrowsedProgram()
{
    const auto now = QDateTime::currentDateTimeUtc();
    QFETCH(int, offsetHours);
    EpgEntry current;
    current.channelId = QStringLiteral("channel.one");
    current.title = QStringLiteral("Current");
    current.start = now.addSecs(-600);
    current.stop = now.addSecs(600);
    auto browsed = current;
    browsed.title = QStringLiteral("Browsed");
    browsed.start = now.addSecs(offsetHours * 3600);
    browsed.stop = browsed.start.addSecs(1800);
    EpgService epg;
    epg.loadFromEntries({ current, browsed });

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    SettingsManager settings(tempDir.filePath(QStringLiteral("settings.json")));
    settings.load();
    GuideStateModel model(&epg, &settings);
    Channel channel;
    channel.id = 1;
    channel.tvgId = current.channelId;
    auto otherChannel = channel;
    otherChannel.id = 2;
    model.setChannels({ channel, otherChannel });
    QSignalSpy programsSpy(&model, &GuideStateModel::channelProgramsChanged);
    model.selectChannel(channel.id);
    QTRY_COMPARE_WITH_TIMEOUT(programsSpy.count(), 1, 3000);
    model.selectProgram(toVariantMap(browsed));
    model.setDetailsExpanded(false);

    // A refresh must update details without replacing the browsed programme with Now.
    browsed.description = QStringLiteral("Updated details");
    epg.loadFromEntries({ current, browsed });
    model.refresh();
    model.refresh(); // Also exercise coalesced refreshes.
    QTRY_COMPARE_WITH_TIMEOUT(programsSpy.count(), 2, 3000);
    QCOMPARE(model.selectedProgram(), toVariantMap(browsed));
    QCOMPARE(model.selectedChannelId(), channel.id);
    QVERIFY(!model.detailsExpanded());

    // A temporary EPG omission must not move explicit selection either.
    epg.loadFromEntries({ current });
    model.refresh();
    QTRY_COMPARE_WITH_TIMEOUT(programsSpy.count(), 3, 3000);
    QCOMPARE(model.selectedProgram(), toVariantMap(browsed));

    // Changing channels still selects that channel's current programme.
    model.selectChannel(otherChannel.id);
    QTRY_COMPARE_WITH_TIMEOUT(programsSpy.count(), 4, 3000);
    QCOMPARE(model.selectedProgram().value(QStringLiteral("start")), toVariantMap(current).value(QStringLiteral("start")));
    QCOMPARE(model.selectedProgram().value(QStringLiteral("title")).toString(), current.title);
}

void AppModelTests::m3uDiscoversEpgAndRespectsOverride()
{
    auto network = std::make_shared<MockNetworkAccess>();
    const QUrl overrideUrl(QStringLiteral("https://example.test/override.xml"));
    network->setResponse(overrideUrl, {xmltvPayload(QStringLiteral("Override")), {}, 0});
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt, network));
    const auto writeGuide = [&](const QString &title) {
        QFile file(harness.tempDir.filePath(QStringLiteral("guide.xml")));
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(xmltvPayload(title)) > 0;
    };
    QVERIFY(writeGuide(QStringLiteral("Discovered")));
    QFile playlist(harness.playlistPath);
    QVERIFY(playlist.open(QIODevice::ReadWrite));
    auto bytes = playlist.readAll();
    bytes.replace("#EXTM3U", "#EXTM3U url-tvg=\"guide.xml\"");
    QVERIFY(playlist.resize(0));
    QCOMPARE(playlist.write(bytes), bytes.size());
    playlist.close();
    harness.appController->initialize();
    QTRY_VERIFY_WITH_TIMEOUT(!harness.appController->isBusy() && !harness.appController->epgRefreshInProgress(), 5000);
    QTRY_COMPARE(harness.epgService->totalEntries(), 1);
    QCOMPARE(harness.epgService->allEntries().first().title, QStringLiteral("Discovered"));
    const auto saved = harness.settings->activeProfile();
    QVERIFY(saved);
    QCOMPARE(saved->discoveredXmltvUrls.size(), 1);
    QVERIFY(saved->xmltvUrl.isEmpty());
    QVERIFY(writeGuide(QStringLiteral("Refreshed")));
    harness.appController->refreshActiveEpg();
    QTRY_VERIFY(!harness.appController->epgRefreshInProgress());
    QCOMPARE(harness.epgService->allEntries().first().title, QStringLiteral("Refreshed"));
    auto profile = *saved;
    profile.xmltvUrl = overrideUrl.toString();
    QVERIFY(harness.settings->replaceProfile(profile.id, profile));
    harness.appController->refreshActiveEpg();
    QTRY_VERIFY(!harness.appController->epgRefreshInProgress());
    QCOMPARE(harness.epgService->allEntries().first().title, QStringLiteral("Override"));
    QCOMPARE(network->callCount(overrideUrl), 1);
    // An explicit EPG override has the same meaning for XC profiles.
    profile.type = ProfileType::Xtream;
    QVERIFY(harness.settings->replaceProfile(profile.id, profile));
    harness.appController->refreshActiveEpg();
    QTRY_VERIFY(!harness.appController->epgRefreshInProgress());
    QCOMPARE(network->callCount(overrideUrl), 2);
    QCOMPARE(harness.epgService->allEntries().first().title, QStringLiteral("Override"));
}

void AppModelTests::m3uRefreshRetainsChannelIdentity()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    harness.appController->initialize();
    QTRY_VERIFY(!harness.appController->isBusy());
    const auto profile = harness.activeProfileId();
    const auto key = guidToString(profile);
    QVERIFY(harness.channelListModel->toggleFavorite(0));
    harness.settings->current().lastWatchedChannelId[key] = 0;
    harness.database->incrementWatchSeconds(profile, 0, 123);
    QFile playlist(harness.playlistPath);
    QVERIFY(playlist.open(QIODevice::WriteOnly | QIODevice::Truncate));
    playlist.write("#EXTM3U\n#EXTINF:-1,New\nhttp://127.0.0.1/new\n"
                   "#EXTINF:-1,Two\nhttp://127.0.0.1/channel-two\n"
                   "#EXTINF:-1,One\nhttp://127.0.0.1/channel-one\n");
    playlist.close();
    harness.appController->refreshActiveProfile();
    QTRY_VERIFY(!harness.appController->isBusy());
    const auto channels = harness.database->loadChannels(profile);
    QCOMPARE(channels.size(), 3);
    QCOMPARE(channels[0].id, 2);
    QCOMPARE(channels[1].id, 1);
    QCOMPARE(channels[2].id, 0);
    QCOMPARE(channels[2].sortOrder, 3);
    QVERIFY(harness.channelListModel->isFavorite(0));
    QVERIFY(!harness.channelListModel->isFavorite(2));
    QCOMPARE(harness.database->loadWatchSecondsByProfile(profile).value(0), 123);
    QCOMPARE(harness.settings->current().lastWatchedChannelId.value(key), 0);
    // Even an empty refresh and reopening the database must not recycle IDs.
    harness.database->replaceChannelsForProfile(profile, {});
    DatabaseService reopened(harness.database->databaseFilePath());
    QCOMPARE(reopened.nextM3uChannelId(profile), 3);
    QVERIFY(playlist.open(QIODevice::WriteOnly | QIODevice::Truncate));
    playlist.write("#EXTM3U\n#EXTINF:-1,Replacement\nhttp://127.0.0.1/replacement\n");
    playlist.close();
    harness.appController->refreshActiveProfile();
    QTRY_VERIFY(!harness.appController->isBusy());
    QCOMPARE(harness.database->loadChannels(profile).first().id, 3);
    QVERIFY(!harness.channelListModel->isFavorite(3));
    harness.playerController->stop();
}

void AppModelTests::m3uSourcesSaveArchiveSafetyMargin()
{
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    const auto urlId = harness.profilesModel->addM3uUrlProfile(QStringLiteral("URL"),
        QStringLiteral("https://example.test/list.m3u"), {}, 12, 0);
    const auto fileId = harness.profilesModel->addM3uFileProfile(QStringLiteral("File"), harness.playlistPath, {}, 17);
    QVERIFY(!urlId.isEmpty());
    QVERIFY(!fileId.isEmpty());
    QCOMPARE(harness.settings->profileById(parseGuid(urlId))->catchupSafetyMinutes, 0);
    QCOMPARE(harness.settings->profileById(parseGuid(fileId))->catchupSafetyMinutes, 17);
    harness.settings->load();
    QCOMPARE(harness.settings->profileById(parseGuid(fileId))->catchupSafetyMinutes, 17);
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
    const auto profile = harness.settings->activeProfile();
    QVERIFY(profile.has_value());
    EpgCacheService::CacheData data;
    data.profileId = profile->id;
    data.sourceFingerprint = EpgCacheService::sourceFingerprint(profile.value());
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
    const auto profile = harness.settings->activeProfile();
    QVERIFY(profile.has_value());
    EpgCacheService::CacheData data;
    data.profileId = profile->id;
    data.sourceFingerprint = EpgCacheService::sourceFingerprint(profile.value());
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
    const auto profile = harness.settings->activeProfile();
    QVERIFY(profile.has_value());
    EpgCacheService::CacheData data;
    data.profileId = profile->id;
    data.sourceFingerprint = EpgCacheService::sourceFingerprint(profile.value());
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
    const auto profile = harness.settings->activeProfile();
    QVERIFY(profile.has_value());
    EpgCacheService::CacheData data;
    data.profileId = profile->id;
    data.sourceFingerprint = EpgCacheService::sourceFingerprint(profile.value());
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

void AppModelTests::playerControllerQueuedProgrammeStopCannotStopNewTune()
{
    PlayerController controller;
    controller.m_positionTimer.stop();
    controller.m_liveDeliveryTimer.stop();
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-7200);
    controller.m_playbackMode = QStringLiteral("catchup");
    controller.m_catchupSession.setProgramStartUtc(start);
    controller.m_catchupSession.setProgramStopUtc(start.addSecs(3600));
    controller.m_catchupSession.setTimelinePositionSeconds(3600.0);
    QVERIFY(controller.maybeStopCatchupAtProgrammeBoundary(3600.0, 0.0, QStringLiteral("test")));
    Channel next;
    next.id = 9281;
    next.streamUrl = QStringLiteral("http://127.0.0.1:1/new.ts");
    controller.playChannel(next);
    QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
    QVERIFY(controller.currentChannelValue());
    QCOMPARE(controller.currentChannelValue()->id, next.id);
    QCOMPARE(controller.currentPlaybackUrl(), next.streamUrl);
    controller.stop();
}

void AppModelTests::playerControllerRecoveryRespectsPauseAndPendingReload()
{
    PlayerController controller;
    controller.m_positionTimer.stop();
    controller.m_liveDeliveryTimer.stop();
    Channel channel;
    channel.id = 9282;
    channel.streamUrl = QStringLiteral("http://127.0.0.1:1/live.ts");
    controller.m_currentChannel = channel;
    controller.m_userPausedManually = true;
    controller.startReconnectLoop(QStringLiteral("late-error"));
    QVERIFY(!controller.m_recovery.active());
    QVERIFY(controller.m_userPausedManually);
    controller.m_userPausedManually = false;
    controller.m_playbackMode = QStringLiteral("catchup");
    controller.m_currentPlaybackUrl = channel.streamUrl;
    QVERIFY(controller.m_catchupSession.beginReload(120.0, QStringLiteral("archive"), 120.0));
    controller.startReconnectLoop(QStringLiteral("teardown-error"));
    QVERIFY(!controller.m_recovery.active());
    QCOMPARE(controller.m_catchupSession.reloadUrl(), QStringLiteral("archive"));
    controller.stop();
}

void AppModelTests::catchupDownloadValidation()
{
    if (!ffmpegToolsAvailable())
        QSKIP("ffmpeg/ffprobe unavailable");
    StartupHarness harness;
    QVERIFY(harness.initialize(std::nullopt));
    Channel channel;
    channel.id = 7;
    channel.profileId = harness.activeProfileId();
    channel.source = ChannelSource::M3U;
    channel.streamUrl = QStringLiteral("http://example.invalid/live");
    channel.tvgId = QStringLiteral("fixture");
    channel.catchupSupported = true;
    channel.catchupWindowHours = 24;
    channel.catchupMode = QStringLiteral("default");
    channel.catchupSourceTemplate = QStringLiteral("http://example.invalid/archive?utc={utc}&duration={duration}");
    harness.channelListModel->setChannels({channel}, {});
    EpgEntry program;
    program.channelId = channel.tvgId;
    program.title = QStringLiteral("Download fixture");
    program.start = QDateTime::currentDateTimeUtc().addSecs(-3600);
    program.stop = program.start.addSecs(600);
    auto state = [&] { return harness.appController->catchupDownloadActionState(toVariantMap(channel), toVariantMap(program)); };
    QVERIFY(state().value(QStringLiteral("visible")).toBool());
    QVERIFY(state().value(QStringLiteral("enabled")).toBool());
    const auto stop = program.stop;
    program.stop = QDateTime::currentDateTimeUtc().addSecs(-30);
    QVERIFY(state().value(QStringLiteral("visible")).toBool());
    QVERIFY(!state().value(QStringLiteral("enabled")).toBool());
    program.stop = QDateTime::currentDateTimeUtc().addSecs(60);
    QVERIFY(!state().value(QStringLiteral("visible")).toBool());
    QVERIFY(!state().value(QStringLiteral("enabled")).toBool());
    QVERIFY(!harness.appController->enqueueCatchupDownload(toVariantMap(channel), toVariantMap(program),
        QUrl::fromLocalFile(harness.tempDir.filePath(QStringLiteral("ongoing.mkv")))).isEmpty());
    QCOMPARE(harness.appController->downloadController()->rowCount(), 0);
    program.stop = stop;
    program.channelId = QStringLiteral("wrong-channel");
    QVERIFY(!state().value(QStringLiteral("enabled")).toBool());
    program.channelId = channel.tvgId;
    harness.settings->current().catchupEnabled = false;
    QVERIFY(!state().value(QStringLiteral("enabled")).toBool());
    harness.settings->current().catchupEnabled = true;
    program.start = QDateTime::currentDateTimeUtc().addDays(-2);
    QVERIFY(!state().value(QStringLiteral("enabled")).toBool());
    program.start = stop.addSecs(-600);
    const auto profile = channel.profileId;
    channel.profileId = QUuid::createUuid();
    QVERIFY(!state().value(QStringLiteral("enabled")).toBool());
    channel.profileId = profile;
    // A dialog result must revalidate after its selected channel disappears.
    harness.channelListModel->setChannels({}, {});
    QVERIFY(!harness.appController->enqueueCatchupDownload(toVariantMap(channel), toVariantMap(program),
        QUrl::fromLocalFile(harness.tempDir.filePath(QStringLiteral("download.mkv")))).isEmpty());
    QCOMPARE(harness.appController->downloadController()->rowCount(), 0);
}

QTEST_MAIN(AppModelTests)

#include "tst_app_models.moc"
