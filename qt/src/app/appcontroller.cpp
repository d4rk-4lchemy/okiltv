#include "appcontroller.h"
#include "catchupdownloadcontroller.h"
#include "catchupdownloadtransfer.h"
#include "../core/sourcegrouppreferences.h"

#include "channellistmodel.h"
#include "dvrcontroller.h"
#include "epggridmodel.h"
#include "guidestatemodel.h"
#include "multiviewcontroller.h"
#include "nownextmodel.h"
#include "playercontroller.h"
#include "profilesmodel.h"
#include "settingscontroller.h"
#include "shellcontroller.h"
#include "timeshiftcontroller.h"

#include "../core/xtreamservice.h"
#include "../core/appdatapaths.h"
#include "../core/catchupurlresolver.h"
#include "../core/debuglogger.h"
#include "../core/redaction.h"

#include <QtConcurrent>
#include <QDebug>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryFile>
#include <QGuiApplication>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSet>
#include <QSysInfo>
#include <QTimer>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <utility>

namespace OKILTV::App {

using namespace Core;

namespace {

constexpr auto kFavouritesCategoryId = "__favourites__";
constexpr int kWatchStatsFlushIntervalMs = 60 * 1000;

struct LoadProfileResult
{
    bool ok { false };
    bool sourceRefreshSucceeded { false };
    bool groupAutoEnableSkipped { false };
    QString statusText;
    QString errorText;
    ServerProfile profile;
    QList<Channel> channels;
    QList<ChannelCategory> categories;
    QHash<int, qint64> watchSecondsByChannelId;
    std::optional<int> lastWatchedChannelId;
};

struct CatchupValidation
{
    bool visible { false };
    bool enabled { false };
    QString reason;
    std::optional<Channel> channel;
    std::optional<EpgEntry> program;
    std::optional<ServerProfile> profile;
};

QString formatDateTimeUtc(const QDateTime &value)
{
    return value.isValid() ? value.toUTC().toString(Qt::ISODateWithMs) : QStringLiteral("<none>");
}

QString graphicsApiName(const QSGRendererInterface::GraphicsApi api)
{
    switch (api) {
    case QSGRendererInterface::Unknown:
        return QStringLiteral("Unknown");
    case QSGRendererInterface::Software:
        return QStringLiteral("Software");
    case QSGRendererInterface::OpenVG:
        return QStringLiteral("OpenVG");
    case QSGRendererInterface::OpenGL:
        return QStringLiteral("OpenGL");
    case QSGRendererInterface::Direct3D11:
        return QStringLiteral("Direct3D11");
    case QSGRendererInterface::Vulkan:
        return QStringLiteral("Vulkan");
    case QSGRendererInterface::Metal:
        return QStringLiteral("Metal");
    case QSGRendererInterface::Null:
        return QStringLiteral("Null");
    }

    return QStringLiteral("Unrecognized");
}

QString launchModeName(const LaunchMode mode)
{
    return mode == LaunchMode::Portable ? QStringLiteral("Portable") : QStringLiteral("Standard");
}

QDateTime parseIsoUtc(const QString &value)
{
    auto parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(value, Qt::ISODate);
    }
    return parsed.isValid() ? parsed.toUTC() : QDateTime {};
}

QString catchupProgramLabel(const EpgEntry &program, const DateTimeFormatOptions options)
{
    auto title = program.title.trimmed();
    auto timeRange = epgEntryTimeRange(program, options);
    if (title.isEmpty()) {
        return timeRange;
    }
    if (timeRange.isEmpty()) {
        return title;
    }
    return QStringLiteral("%1  %2").arg(timeRange, title);
}

QString formatEpoch(const QDateTime &value)
{
    return value.isValid() ? QString::number(value.toUTC().toSecsSinceEpoch()) : QStringLiteral("<invalid>");
}

QString resolveCatchupRedirectIfPresent(const QString &url, const int timeoutMs, bool *redirectApplied, QString *errorText)
{
    if (redirectApplied != nullptr) {
        *redirectApplied = false;
    }
    if (errorText != nullptr) {
        errorText->clear();
    }

    const QUrl sourceUrl(url.trimmed());
    if (!sourceUrl.isValid() || sourceUrl.isEmpty()) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("invalid URL");
        }
        return url;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(sourceUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto *reply = manager.head(request);
    QEventLoop loop;
    QTimer timer;
    bool timedOut = false;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        reply->abort();
        loop.quit();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    timer.stop();

    const auto redirectTarget = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const auto replyError = reply->error();
    const auto replyErrorText = reply->errorString();
    reply->deleteLater();

    if (timedOut) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("timeout");
        }
        return url;
    }
    if (replyError != QNetworkReply::NoError) {
        if (errorText != nullptr) {
            *errorText = replyErrorText.trimmed().isEmpty() ? QStringLiteral("network error") : replyErrorText;
        }
        return url;
    }
    if (!redirectTarget.isValid() || redirectTarget.isEmpty()) {
        return url;
    }

    const auto resolvedUrl = sourceUrl.resolved(redirectTarget);
    if (!resolvedUrl.isValid() || resolvedUrl.isEmpty()) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("invalid redirect target");
        }
        return url;
    }

    if (redirectApplied != nullptr) {
        *redirectApplied = (resolvedUrl != sourceUrl);
    }
    return resolvedUrl.toString();
}

CatchupValidation validateCatchupRequest(
    const SettingsManager *settingsManager,
    const AppSettings &settings,
    const ChannelListModel *channelListModel,
    const QVariantMap &channelVariant,
    const QVariantMap &programVariant)
{
    auto normalizedId = [](const QString &value) {
        return value.trimmed().toLower();
    };

    CatchupValidation validation;
    validation.visible = true;

    if (!settings.catchupEnabled) {
        validation.reason = QStringLiteral("Catch-up playback is disabled.");
        return validation;
    }

    auto channelIdOk = false;
    const auto channelId = channelVariant.value(QStringLiteral("id")).toInt(&channelIdOk);
    const auto profileId = parseGuid(channelVariant.value(QStringLiteral("profileId")).toString());
    if (!channelIdOk || channelId < 0 || profileId.isNull() || channelListModel == nullptr) {
        validation.reason = QStringLiteral("Channel identity is invalid.");
        return validation;
    }

    const auto resolvedChannel = channelListModel->channelById(channelId);
    if (!resolvedChannel.has_value() || resolvedChannel->profileId != profileId) {
        validation.reason = QStringLiteral("Channel is no longer available.");
        return validation;
    }
    validation.channel = resolvedChannel;

    EpgEntry program;
    program.channelId = programVariant.value(QStringLiteral("channelId")).toString().trimmed();
    program.title = programVariant.value(QStringLiteral("title")).toString();
    program.subTitle = programVariant.value(QStringLiteral("subTitle")).toString();
    program.description = programVariant.value(QStringLiteral("description")).toString();
    program.episodeNum = programVariant.value(QStringLiteral("episodeNum")).toString();
    program.start = parseIsoUtc(programVariant.value(QStringLiteral("start")).toString());
    program.stop = parseIsoUtc(programVariant.value(QStringLiteral("stop")).toString());
    validation.program = program;

    if (!program.start.isValid() || !program.stop.isValid() || program.stop <= program.start) {
        validation.reason = QStringLiteral("Programme timing is invalid.");
        return validation;
    }
    const auto programChannelId = normalizedId(program.channelId);
    const auto resolvedChannelTvgId = normalizedId(resolvedChannel->tvgId);
    if (!programChannelId.isEmpty()
        && !resolvedChannelTvgId.isEmpty()
        && programChannelId != resolvedChannelTvgId) {
        validation.reason = QStringLiteral("Programme does not belong to the selected channel.");
        return validation;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    validation.visible = program.start < now;
    if (program.start >= now) {
        validation.reason = QStringLiteral("Catch-up becomes available after the programme starts.");
        return validation;
    }

    if (!resolvedChannel->catchupSupported) {
        validation.reason = QStringLiteral("This channel does not provide archive playback.");
        return validation;
    }
    if (resolvedChannel->catchupWindowHours <= 0) {
        validation.reason = QStringLiteral("This channel does not expose a usable archive window.");
        return validation;
    }

    const auto oldestAllowed = now.addSecs(-resolvedChannel->catchupWindowHours * 3600LL);
    if (program.start < oldestAllowed) {
        validation.reason = QStringLiteral("This programme is outside the provider archive window.");
        return validation;
    }

    if (settingsManager != nullptr) {
        validation.profile = settingsManager->profileById(resolvedChannel->profileId);
    }
    const auto safetySeconds = 60 * std::clamp(validation.profile.has_value() ? validation.profile->catchupSafetyMinutes : 3, 0, 30);
    if (CatchupUrlResolver::availableEdge(program.stop, safetySeconds, now) <= program.start) {
        validation.reason = QStringLiteral("Catch-up becomes available after %1 minutes of programme runtime (source archive safety margin).")
                                .arg(safetySeconds / 60);
        return validation;
    }

    validation.enabled = true;
    return validation;
}

} // namespace

AppController::~AppController()
{
    m_stopping = true;
    EpgCacheService::cancel(m_epgImportCancellation);
    flushCatchupProgress();
    flushTrackedWatchSeconds();
    for (auto future : m_backgroundTasks.futures()) {
        try { future.waitForFinished(); }
        catch (...) {
            Core::DebugLogger::instance().log(QStringLiteral("app"), QStringLiteral("Background task failed during shutdown."));
        }
    }
    // Every worker is finished; prevent the synchronizer destructor rethrowing.
    m_backgroundTasks.clearFutures();
}

AppController::AppController(
    SettingsManager *settings,
    DatabaseService *database,
    std::shared_ptr<NetworkAccess> network,
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
    EpgService *epgService,
    QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_database(database)
    , m_network(std::move(network))
    , m_profilesModel(profilesModel)
    , m_channelListModel(channelListModel)
    , m_nowNextModel(nowNextModel)
    , m_playbackNowNextModel(playbackNowNextModel)
    , m_epgGridModel(epgGridModel)
    , m_guideStateModel(guideStateModel)
    , m_shellController(shellController)
    , m_multiViewController(multiViewController)
    , m_playerController(playerController)
    , m_dvrController(dvrController)
    , m_timeshiftController(timeshiftController)
    , m_settingsController(settingsController)
    , m_epgService(epgService)
    , m_iconCacheService(*database, m_network)
{
    m_downloadController = new CatchupDownloadController(settings, this);
    connect(m_timeshiftController, &TimeshiftController::stateChanged, this, &AppController::refreshTimeshiftProgram);
    connect(this, &AppController::epgRefreshStateChanged, this, &AppController::refreshTimeshiftProgram);
    connect(m_multiViewController, &MultiViewController::focusedPlaybackChanged, this, &AppController::refreshTimeshiftProgram);
    const auto updateDateTimeFormat = [this]() {
        const auto options = m_settingsController->dateTimeFormatter()->options();
        m_epgGridModel->setDateTimeFormat(options);
        emit m_nowNextModel->dataChanged();
        emit m_playbackNowNextModel->dataChanged();
        emit m_guideStateModel->selectedProgramChanged();
        emit m_guideStateModel->channelProgramsChanged();
        if (m_channelListModel->rowCount() > 0) {
            emit m_channelListModel->dataChanged(m_channelListModel->index(0),
                m_channelListModel->index(m_channelListModel->rowCount() - 1),
                { ChannelListModel::CurrentProgramTimeRangeRole });
        }
        emit epgRefreshStateChanged();
        if (m_statusText.startsWith(QStringLiteral("EPG refreshed at "))) {
            setStatusText(QStringLiteral("EPG refreshed at %1.").arg(epgLastRefreshText()));
        }
    };
    connect(m_settingsController->dateTimeFormatter(), &DateTimeFormatter::formatChanged,
        this, updateDateTimeFormat);
    updateDateTimeFormat();
    m_selectedNowNextRefreshTimer.setSingleShot(true);
    m_selectedNowNextRefreshTimer.setInterval(120);
    connect(&m_selectedNowNextRefreshTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingSelectedNowNextChannel.has_value()) {
            m_nowNextModel->setChannel(m_pendingSelectedNowNextChannel);
        } else {
            m_nowNextModel->clear();
        }
    });
    m_selectedGuideRefreshTimer.setSingleShot(true);
    m_selectedGuideRefreshTimer.setInterval(120);
    connect(&m_selectedGuideRefreshTimer, &QTimer::timeout, this, [this]() {
        const auto selectedId = m_pendingSelectedGuideChannelId;
        const auto guideOverlayVisible = m_shellController->activeOverlay() == QStringLiteral("guide");
        if (guideOverlayVisible && (m_epgGridModel->rebuildPending()
            || m_epgGridModel->rowIndexForChannelId(selectedId) < 0)) {
            return;
        }
        if (selectedId >= 0) {
            m_guideStateModel->selectChannel(selectedId);
            if (guideOverlayVisible) {
                m_epgGridModel->setSelectedChannelId(selectedId);
            }
            return;
        }

        m_guideStateModel->clear();
        if (guideOverlayVisible) {
            m_epgGridModel->setSelectedChannelId(-1);
            m_epgGridModel->setSelectedProgramStart(QString {});
        }
    });

    connect(m_profilesModel, &ProfilesModel::profileSelectionRequested, this, &AppController::loadProfile);
    connect(m_channelListModel, &ChannelListModel::channelActivated, this, &AppController::activateChannel);
    connect(&m_epgUiTimer, &QTimer::timeout, m_nowNextModel, &NowNextModel::refresh);
    connect(&m_epgUiTimer, &QTimer::timeout, m_playbackNowNextModel, &NowNextModel::refresh);
    connect(&m_epgUiTimer, &QTimer::timeout, m_guideStateModel, &GuideStateModel::refresh);
    connect(&m_epgUiTimer, &QTimer::timeout, this, [this]() {
        rebuildGuideGridAsync();
    });
    connect(&m_epgUiTimer, &QTimer::timeout, this, &AppController::updateChannelProgrammeMetadata);
    connect(&m_epgUiTimer, &QTimer::timeout, this, &AppController::triggerScheduledSourceAutoRefresh);
    connect(m_channelListModel, &ChannelListModel::selectedChannelIdChanged, this, [this]() {
        const auto selectedId = m_channelListModel->selectedChannelId();
        if (selectedId >= 0) {
            m_pendingSelectedNowNextChannel = m_channelListModel->channelById(selectedId);
            m_selectedNowNextRefreshTimer.start();
        } else {
            m_pendingSelectedNowNextChannel = std::nullopt;
            m_selectedNowNextRefreshTimer.start();
        }
        m_pendingSelectedGuideChannelId = selectedId;
        m_selectedGuideRefreshTimer.start();
    });
    connect(m_channelListModel, &ChannelListModel::selectedCategoryIdChanged, this, [this]() {
        m_guideStateModel->setSelectedGroupId(m_channelListModel->selectedCategoryId());
        rebuildGuideGridAsync();
    });
    connect(m_channelListModel, &ChannelListModel::categoriesChanged, this, [this]() {
        rebuildGuideGridAsync();
    });
    connect(m_guideStateModel, &GuideStateModel::selectedProgramChanged, this, [this]() {
        if (m_shellController->activeOverlay() != QStringLiteral("guide")) {
            return;
        }

        m_epgGridModel->setSelectedProgramStart(
            m_guideStateModel->selectedProgram().value(QStringLiteral("start")).toString());
    });
    connect(m_settingsController, &SettingsController::saved, this, [this]() {
        updateRefreshTimer();
        const auto &settings = m_settings->current();
        m_guideStateModel->setPreviewEnabled(settings.guidePreviewEnabled);
        // A presentation-only save must not move the Guide's time window.
        if (m_epgGridModel->lookAheadHours() != normalizeGuideHours(settings.epgLookAheadHours)) {
            m_nowNextModel->refresh();
            m_playbackNowNextModel->refresh();
            m_guideStateModel->refresh();
        }
        if (m_epgGridModel->guidePastHours() != normalizeGuideHours(settings.guidePastHours)
            || m_epgGridModel->lookAheadHours() != normalizeGuideHours(settings.epgLookAheadHours)) {
            rebuildGuideGridAsync();
        }
    });
    connect(&m_refreshTimer, &QTimer::timeout, this, &AppController::triggerScheduledEpgRefresh);
    connect(&m_guideRebuildTimer, &QTimer::timeout, this, [this]() {
        if (!m_guideRebuildAsyncRequested) {
            return;
        }

        m_epgGridModel->rebuildAsync(
            guideChannels(),
            m_settings->current().guidePastHours,
            m_settings->current().epgLookAheadHours);
        m_guideRebuildAsyncRequested = false;
    });
    connect(m_shellController, &ShellController::activeOverlayChanged, this, [this]() {
        if (m_shellController->activeOverlay() != QStringLiteral("guide")) {
            return;
        }

        m_epgGridModel->setSelectedChannelId(m_guideStateModel->selectedChannelId());
        m_epgGridModel->setSelectedProgramStart(
            m_guideStateModel->selectedProgram().value(QStringLiteral("start")).toString());
    });
    connect(m_timeshiftController, &TimeshiftController::statusMessageRequested, this, &AppController::setStatusText);
    connect(m_multiViewController, &MultiViewController::statusMessageRequested, this, &AppController::setStatusText);
    connect(m_multiViewController, &MultiViewController::primaryTileAssignmentRequested, this, [this](const int channelId) {
        const auto channel = m_channelListModel->channelById(channelId);
        if (!channel.has_value()) {
            return;
        }
        activatePrimaryChannel(channel.value());
    });
    connect(&m_catchupProgressFlushTimer, &QTimer::timeout, this, &AppController::flushCatchupProgress);
    connect(&m_epgUiTimer, &QTimer::timeout, this, &AppController::pruneCatchupProgress);
    connect(m_settingsController, &SettingsController::saved, this, &AppController::pruneCatchupProgress);
    try {
        const auto savedProgress = m_database->loadCatchupProgress();
        for (const auto &progress : savedProgress) {
            m_catchupProgress.insert(progress.key, progress);
        }
        pruneCatchupProgress();
    } catch (const std::exception &error) {
        DebugLogger::instance().log(QStringLiteral("catchup.progress"),
            QStringLiteral("Cannot load progress: %1").arg(QString::fromUtf8(error.what())));
    }
    connectPlaybackSession(m_playerController);
    connect(m_multiViewController, &MultiViewController::playbackSessionCreated, this, &AppController::connectPlaybackSession);
    connect(m_multiViewController, &MultiViewController::primaryControllerChanged, this, [this]() {
        flushTrackedWatchSeconds();
        m_playerController = m_multiViewController->primaryController();
        m_observedCatchupSession = {};
        m_dvrController->setPlayerController(m_playerController);
        m_timeshiftController->setPlayerController(m_playerController);
        m_playbackNowNextModel->setChannel(m_multiViewController->focusedController()->currentChannelValue());
        beginWatchTrackingForCurrentChannel();
    });
    connect(m_multiViewController, &MultiViewController::focusedPlaybackChanged, this, [this]() {
        m_playbackNowNextModel->setChannel(m_multiViewController->focusedController()->currentChannelValue());
    });
    m_catchupProgressFlushTimer.start(10000);
    connect(m_playbackNowNextModel, &NowNextModel::dataChanged, this, [this]() {
        if (m_epgCacheBootstrapPending && !m_playbackNowNextModel->loading()) {
            setEpgCacheBootstrapPending(false);
        }
    });
    connect(m_dvrController, &DvrController::recordingChannelsChanged, this, [this](const QString &profileId, const QList<int> &channelIds) {
        m_channelListModel->setDvrRecordingChannelsForProfile(profileId, channelIds);
    });
    connect(&m_watchStatsFlushTimer, &QTimer::timeout, this, &AppController::flushTrackedWatchSeconds);

    m_epgUiTimer.setInterval(60 * 1000);
    m_refreshTimer.setSingleShot(true);
    m_guideRebuildTimer.setSingleShot(true);
    m_guideRebuildTimer.setInterval(16);
    m_watchStatsFlushTimer.setInterval(kWatchStatsFlushIntervalMs);
    Core::DebugLogger::instance().log(QStringLiteral("app"), QStringLiteral("AppController constructed."));
}

void AppController::connectPlaybackSession(PlayerController *controller)
{
    auto *formatter = m_settingsController->dateTimeFormatter();
    controller->setDateTimeFormat(formatter->options());
    connect(formatter, &DateTimeFormatter::formatChanged,
        controller, [formatter, controller]() {
            controller->setDateTimeFormat(formatter->options());
        });

    connect(controller, &PlayerController::playbackError, this, [this, controller](const QString &message) {
        if (controller != m_playerController) {
            return;
        }
        Core::DebugLogger::instance().log(QStringLiteral("app"), QStringLiteral("Playback error: %1").arg(message));
        if (m_playerController->inCatchupMode()) {
            Core::DebugLogger::instance().log(
                QStringLiteral("catchup.play.error"),
                QStringLiteral("Catch-up playback failed: %1").arg(message));
        }
        setStatusText(message);
    });
    const auto refreshTimeshift = [this, controller]() {
        if (controller == m_playerController) refreshTimeshiftProgram();
    };
    connect(controller, &PlayerController::positionTextChanged, this, refreshTimeshift);
    connect(controller, &PlayerController::currentChannelChanged, this, refreshTimeshift);
    connect(controller, &PlayerController::catchupPlaybackTimeChanged, this, [this, controller](const QDateTime &time) {
        const auto channel = controller->currentChannelValue();
        if (!channel.has_value()) {
            return;
        }
        controller->updateCatchupProgramme(catchupProgramAt(channel.value(), time, controller->validatedCatchupProgramme()));
    });
    connect(controller, &QObject::destroyed, this, [this, controller] { m_pendingCatchupSamples.remove(controller); });
    connect(controller, &PlayerController::playbackChannelActivated, this, [this, controller]() {
        m_pendingCatchupSamples.remove(controller);
        if (controller != m_playerController) {
            return;
        }
        m_observedCatchupSession = {};
    });
    connect(controller, &PlayerController::catchupProgressObserved, this,
        [this, controller](const CatchupProgressSample &sample) { recordCatchupProgress(sample, controller); });
    connect(controller, &PlayerController::catchupProgressFlushRequested, this, &AppController::flushCatchupProgress);
    connect(controller, &PlayerController::isPlayingChanged, this, [this, controller]() {
        if (controller != m_playerController) {
            return;
        }
        if (!m_playerController->isPlaying()) {
            flushCatchupProgress();
        }
    });
    connect(controller, &PlayerController::playbackChannelActivated, this, [this, controller](const int channelId) {
        if (controller != m_playerController) {
            return;
        }
        flushTrackedWatchSeconds();
        const auto channel = m_channelListModel->channelById(channelId);
        if (!channel.has_value()) {
            m_watchTrackingProfileId = QUuid {};
            m_watchTrackingChannelId = -1;
            m_watchTrackingActive = false;
            return;
        }

        m_watchTrackingProfileId = channel->profileId;
        m_watchTrackingChannelId = channel->id;
        m_watchTrackingActive = false;
    });
    connect(controller, &PlayerController::isPlayingChanged, this, [this, controller]() {
        if (controller != m_playerController) {
            return;
        }
        if (!m_playerController->isPlaying()) {
            flushTrackedWatchSeconds();
            m_watchTrackingActive = false;
            m_watchStatsFlushTimer.stop();
            return;
        }

        beginWatchTrackingForCurrentChannel();
    });
}

QStringList AppController::groupAutoEnableNoticeProfileIds() const
{
    return m_groupAutoEnableNoticeProfileIds;
}

void AppController::dismissGroupAutoEnableNotice(const QString &profileId)
{
    if (m_groupAutoEnableNoticeProfileIds.removeAll(profileId) > 0) {
        emit groupAutoEnableNoticesChanged();
    }
}

QString AppController::statusText() const
{
    return m_statusText;
}

bool AppController::isBusy() const
{
    return m_isBusy;
}

QString AppController::activeProfileId() const
{
    const auto &activeProfileId = m_settings->current().activeProfileId;
    return activeProfileId.has_value() ? guidToString(*activeProfileId) : QString {};
}

QString AppController::epgLastRefreshText() const
{
    if (!m_epgFetchedAt.isValid()) {
        return QStringLiteral("Never");
    }

    return formatDisplayDateTime(m_epgFetchedAt, m_settingsController->dateTimeFormatter()->dateTimePattern());
}

bool AppController::epgRefreshInProgress() const
{
    return m_epgRefreshInProgress;
}

bool AppController::epgCacheBootstrapPending() const
{
    return m_epgCacheBootstrapPending;
}

void AppController::initialize()
{
    m_startupCatchupSession = m_settings->current().lastCatchupSession;
    Core::DebugLogger::instance().log(
        QStringLiteral("app"),
        QStringLiteral("Initializing on %1 with Qt %2, platform=%3, graphicsApi=%4.")
            .arg(QSysInfo::prettyProductName(), QString::fromUtf8(qVersion()), QGuiApplication::platformName(), graphicsApiName(QQuickWindow::graphicsApi())));
    updateRefreshTimer();
    if (!m_epgUiTimer.isActive()) {
        m_epgUiTimer.start();
    }

    m_playerController->applySettings(
        m_settings->current().mpvDllPath,
        m_settings->current().mpvOptions,
        m_settings->current().playerWaitForStreamSeconds,
        m_settings->current().playerDeinterlaceEnabled,
        m_settings->current().playerBufferSeconds,
        m_settings->current().playerUserAgent,
        m_settings->current().remuxRecordingsToMkv,
        m_settings->current().playerImageSmoothingEnabled,
        m_settings->current().playerPicturePreset);

    m_playerController->setVolume(m_settings->current().playerVolume);

    QElapsedTimer groupSyncTimer;
    groupSyncTimer.start();
    Core::DebugLogger::instance().log(QStringLiteral("app"), QStringLiteral("Synchronizing saved source groups (metadata only)."));
    auto preferencesChanged = false;
    for (const auto &summary : m_settings->sourceSummaries()) {
        if (!m_settings->profileById(summary.id).has_value()) continue;
        preferencesChanged = syncProfileGroupIds(summary.id, m_database->loadChannelGroupIds(summary.id))
            || preferencesChanged;
    }
    if (preferencesChanged) {
        m_settings->save();
        m_profilesModel->reload();
    }
    Core::DebugLogger::instance().log(QStringLiteral("app"),
        QStringLiteral("Saved source groups synchronized in %1 ms.").arg(groupSyncTimer.elapsed()));

    const auto profile = m_settings->activeProfile();
    if (profile.has_value()) {
        loadProfile(guidToString(profile->id));
    } else if (!m_settings->lastLoadError().isEmpty()) {
        setStatusText(m_settings->lastLoadError());
        m_shellController->openOverlay(QStringLiteral("settings"), QStringLiteral("sources"));
    }
}

void AppController::loadProfile(const QString &profileId)
{
    flushTrackedWatchSeconds();

    const auto parsedId = parseGuid(profileId);
    const auto profile = m_settings->profileById(parsedId);
    if (!profile.has_value()) {
        setStatusText(m_settings->lastLoadError().isEmpty()
            ? QStringLiteral("Profile not found.") : m_settings->lastLoadError());
        return;
    }

    if (parsedId != m_epgLoadedProfileId) {
        EpgCacheService::cancel(m_epgImportCancellation);
        ++m_epgLoadGeneration;
    }

    const auto currentChannel = m_playerController->currentChannelValue();
    const auto *pipSession = qobject_cast<PlayerController *>(m_multiViewController->pipControllerObject());
    const auto activeSourceId = m_settings->current().activeProfileId;
    const auto crossProfileActivationFromCatchup = currentChannel.has_value()
        && currentChannel->profileId != profile->id
        && (m_playerController->inCatchupMode() || (pipSession && pipSession->inCatchupMode()))
        && activeSourceId.has_value()
        && activeSourceId.value() == profile->id;
    if (crossProfileActivationFromCatchup) {
        ++m_catchupPlayGeneration;
        Core::DebugLogger::instance().log(
            QStringLiteral("catchup.play.stop"),
            QStringLiteral("Stopping catch-up before activating source %1 (%2).")
                .arg(profile->name, guidToString(profile->id)));
        setStatusText(QStringLiteral("Stopping catch-up before switching to %1...").arg(profile->name));
        m_multiViewController->focusTile(0);
        m_multiViewController->exitMultiView();
        m_playerController->stop();
    }

    const auto settingsSnapshot = m_settings->current();
    const auto generation = ++m_profileLoadGeneration;
    const auto importToken = DatabaseService::beginChannelImport(profile->id);

    setBusy(true);
    setStatusText(QStringLiteral("Loading %1...").arg(profile->name));
    Core::DebugLogger::instance().log(
        QStringLiteral("profile"),
        QStringLiteral("Loading profile %1 (%2).").arg(profile->name, guidToString(profile->id)));

    m_backgroundTasks.addFuture(QtConcurrent::run([this, generation, importToken, profile = profile.value(), settingsSnapshot]() {
        LoadProfileResult result;
        result.profile = profile;

        try {
            if (profile.type == ProfileType::Xtream) {
                XtreamService xtream(m_network);
                xtream.setProfile(profile);
                const auto authInfo = xtream.authenticate();
                if (!authInfo.authenticated) {
                    throw std::runtime_error("Xtream authentication failed.");
                }
                if (!authInfo.serverTimezone.trimmed().isEmpty()) {
                    result.profile.xtreamServerTimezone = authInfo.serverTimezone.trimmed();
                }
                result.categories = xtream.getLiveCategories();
                result.channels = xtream.getLiveStreams();
                {
                    QHash<QString, QString> nameById;
                    for (const auto &cat : result.categories) {
                        nameById.insert(normalizeChannelCategoryId(cat.id), cat.name);
                    }
                    for (auto &channel : result.channels) {
                        const auto nid = normalizeChannelCategoryId(channel.categoryId);
                        channel.categoryName = nameById.value(nid, displayNameForCategoryId(nid));
                    }
                }
            } else if (profile.type == ProfileType::M3UUrl) {
                M3UService m3u(m_network);
                result.channels = m3u.loadFromUrl(QUrl(profile.m3uUrl), profile.id, &result.profile.discoveredXmltvUrls);
                result.categories = buildM3uCategories(result.channels);
            } else if (profile.type == ProfileType::M3UFile) {
                M3UService m3u(m_network);
                result.channels = m3u.loadFromFile(profile.m3uFilePath, profile.id, &result.profile.discoveredXmltvUrls);
                result.categories = buildM3uCategories(result.channels);
            }

            if (!m_database->publishChannels(profile.id, importToken, result.channels,
                    profile.type != ProfileType::Xtream))
                throw std::runtime_error("Source import superseded.");
            result.watchSecondsByChannelId = m_database->loadWatchSecondsByProfile(profile.id);
            result.profile.lastRefreshed = QDateTime::currentDateTimeUtc();
            result.sourceRefreshSucceeded = true;
            result.ok = true;
            result.statusText = QStringLiteral("%1 channels loaded").arg(result.channels.size());
        } catch (const std::exception &error) {
            result.profile = profile;
            result.errorText = QString::fromUtf8(error.what());
            try {
                result.channels = m_database->loadChannels(profile.id);
                if (!result.channels.isEmpty()) {
                    result.categories = buildM3uCategories(result.channels);
                    result.watchSecondsByChannelId = m_database->loadWatchSecondsByProfile(profile.id);
                    result.ok = true;
                    result.statusText = QStringLiteral("Using cached channels after refresh failure: %1")
                        .arg(result.errorText);
                }
            } catch (const std::exception &cacheError) {
                result.errorText += QStringLiteral("; cached channels unavailable: %1")
                    .arg(QString::fromUtf8(cacheError.what()));
            }
        }

        const auto key = guidToString(profile.id);
        if (result.sourceRefreshSucceeded) {
            QStringList discoveredIds;
            QSet<QString> seen;
            for (const auto &channel : result.channels) {
                const auto id = normalizeChannelCategoryId(channel.categoryId);
                if (!seen.contains(id)) {
                    seen.insert(id);
                    discoveredIds.push_back(id);
                }
            }
            result.groupAutoEnableSkipped = reconcileSourceGroups(
                discoveredIds, { settingsSnapshot.hiddenGroupsByProfile.value(key),
                settingsSnapshot.groupOrderByProfile.value(key) }).autoEnableSkipped;
        }
        if (settingsSnapshot.lastWatchedChannelId.contains(key)) {
            result.lastWatchedChannelId = settingsSnapshot.lastWatchedChannelId.value(key);
        }

        QMetaObject::invokeMethod(
            this,
            [this, generation, importToken, result]() {
                if (generation != m_profileLoadGeneration) return;
                if (!DatabaseService::channelImportCurrent(result.profile.id, importToken)) {
                    setBusy(false);
                    setStatusText(QStringLiteral("Source changed or was removed; channel import discarded."));
                    emit profileLoadFinished(guidToString(result.profile.id), false);
                    return;
                }

                if (!result.ok) {
                    setBusy(false);
                    setStatusText(QStringLiteral("Error: %1").arg(result.errorText));
                    Core::DebugLogger::instance().log(
                        QStringLiteral("profile"),
                        QStringLiteral("Profile load failed for %1: %2").arg(result.profile.name, result.errorText));
                    emit profileLoadFinished(guidToString(result.profile.id), false);
                    return;
                }

                syncProfileGroupPreferences(result.profile.id, result.channels);
                const auto noticeProfileId = guidToString(result.profile.id);
                if (result.groupAutoEnableSkipped && !m_groupAutoEnableNoticeProfileIds.contains(noticeProfileId)) {
                    m_groupAutoEnableNoticeProfileIds.push_back(noticeProfileId);
                    emit groupAutoEnableNoticesChanged();
                }
                const auto activeProfileBeforeSave = activeProfileId();
                if (result.sourceRefreshSucceeded) {
                    if (result.profile.type != ProfileType::Xtream) {
                        auto current = m_settings->profileById(result.profile.id);
                        if (current && current->m3uUrl == result.profile.m3uUrl
                            && current->m3uFilePath == result.profile.m3uFilePath
                            && current->discoveredXmltvUrls != result.profile.discoveredXmltvUrls) {
                            current->discoveredXmltvUrls = result.profile.discoveredXmltvUrls;
                            m_settings->replaceProfile(current->id, *current);
                        }
                    }
                    m_settings->setProfileLastRefreshed(result.profile.id, result.profile.lastRefreshed);
                    m_settings->setProfileGroupCount(result.profile.id, static_cast<int>(result.categories.size()));
                }
                if (!result.profile.xtreamServerTimezone.trimmed().isEmpty()) {
                    m_settings->setProfileXtreamServerTimezone(
                        result.profile.id,
                        result.profile.xtreamServerTimezone.trimmed());
                }

                if (!m_settings->current().activeProfileId.has_value()
                    && m_settings->sourceSummaries().size() == 1) {
                    m_settings->setActiveProfileId(result.profile.id);
                }

                m_settings->save();
                const auto activeProfileAfterSave = activeProfileId();
                if (activeProfileBeforeSave != activeProfileAfterSave) {
                    emit activeProfileIdChanged();
                }
                m_profilesModel->reload();
                // A source refresh can finish while the Settings overlay is being
                // edited. Only Save or an explicit discard may replace that draft.
                if (!m_settingsController->dirty()) {
                    m_settingsController->reload();
                }
                const auto loadedProfileId = guidToString(result.profile.id);
                const auto loadedProfileIsActive = activeProfileAfterSave == loadedProfileId;
                if (loadedProfileIsActive) {
                    m_loadedChannels = result.channels;
                    m_watchSecondsByChannelId = result.watchSecondsByChannelId;
                    m_channelListModel->setActiveProfileId(loadedProfileId);
                    m_channelListModel->setChannels(result.channels, result.categories);
                    m_channelListModel->setDvrRecordingChannelsForProfile(
                        loadedProfileId,
                        m_dvrController->recordingChannelIdsForProfile(loadedProfileId));
                    m_channelListModel->setWatchSeconds(m_watchSecondsByChannelId);
                    m_guideStateModel->setChannels(result.channels);
                    m_guideStateModel->setSelectedGroupId(m_channelListModel->selectedCategoryId());

                    auto shouldResumePlayback = false;
                    if (result.lastWatchedChannelId.has_value()) {
                        shouldResumePlayback = m_channelListModel->selectById(result.lastWatchedChannelId.value());
                        const auto currentChannel = m_playerController->currentChannelValue();
                        if (currentChannel.has_value()
                            && currentChannel->profileId == result.profile.id
                            && currentChannel->id == result.lastWatchedChannelId.value()) {
                            shouldResumePlayback = false;
                        }
                        if (!shouldResumePlayback && !result.channels.isEmpty()
                            && m_channelListModel->selectedChannelId() < 0) {
                            m_channelListModel->selectById(result.channels.first().id);
                        }
                    } else if (!result.channels.isEmpty()) {
                        m_channelListModel->selectById(result.channels.first().id);
                    } else {
                        m_nowNextModel->clear();
                    }

                    if (shouldResumePlayback) {
                        const auto channel = m_channelListModel->channelById(result.lastWatchedChannelId.value());
                        if (!channel.has_value() || !restoreStartupCatchup(channel.value())) {
                            activateChannel(result.lastWatchedChannelId.value());
                        }
                    }

                    m_startupCatchupSession = {};
                    prefetchIconsAsync(result.channels, importToken);
                    if (m_epgLoadedProfileId != result.profile.id) {
                        clearEpg(result.profile.id);
                    }
                    loadEpgAsync(result.profile);
                    updateChannelProgrammeMetadata();
                    rebuildGuideGridAsync();
                }

                updateRefreshTimer();
                setBusy(false);
                setStatusText(result.statusText);
                Core::DebugLogger::instance().log(
                    QStringLiteral("profile"),
                    QStringLiteral("Profile %1 loaded with %2 channels.")
                        .arg(result.profile.name)
                        .arg(result.channels.size()));
                emit profileLoadFinished(guidToString(result.profile.id), true);
            },
            Qt::QueuedConnection);
    }));
}

void AppController::refreshActiveProfile()
{
    const auto active = activeProfileId();
    if (!active.isEmpty()) {
        Core::DebugLogger::instance().log(QStringLiteral("profile"), QStringLiteral("Refreshing active profile %1.").arg(active));
        loadProfile(active);
    }
}

void AppController::refreshActiveEpg()
{
    if (m_epgRefreshInProgress) {
        return;
    }

    const auto profile = m_settings->activeProfile();
    if (!profile.has_value()) {
        setStatusText(QStringLiteral("No active source available for EPG refresh."));
        return;
    }

    if (EpgCacheService::sourceFingerprint(profile.value()).isEmpty()) {
        setStatusText(QStringLiteral("Active source has no EPG feed configured."));
        return;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("epg"),
        QStringLiteral("Running manual EPG refresh for %1.").arg(profile->name));
    m_manualEpgRefreshPending = true;
    setStatusText(QStringLiteral("Refreshing EPG for %1...").arg(profile->name));
    loadEpgAsync(profile.value(), true);
}

void AppController::dumpDebugReport()
{
    const auto path = Core::DebugLogger::instance().writeDump(buildDebugSummary());
    if (path.isEmpty()) {
        setStatusText(QStringLiteral("Failed to save debug dump."));
        return;
    }

    Core::DebugLogger::instance().log(QStringLiteral("app"), QStringLiteral("Debug dump written to %1").arg(path));
    setStatusText(QStringLiteral("Debug dump saved: %1").arg(path));
}

QString AppController::buildDebugSummary() const
{
    const auto &settings = m_settings->current();
    const auto profile = m_settings->activeProfile();
    const auto channel = m_playerController->currentChannelValue();
    const auto runtimeContext = Core::AppDataPaths::runtimeContext();
    QStringList lines;
    lines << QStringLiteral("Qt version: %1").arg(QString::fromUtf8(qVersion()));
    lines << QStringLiteral("Platform: %1").arg(QGuiApplication::platformName());
    lines << QStringLiteral("Graphics API: %1").arg(graphicsApiName(QQuickWindow::graphicsApi()));
    lines << QStringLiteral("OS: %1").arg(QSysInfo::prettyProductName());
    lines << QStringLiteral("CPU architecture: %1").arg(QSysInfo::currentCpuArchitecture());
    lines << QStringLiteral("Launch mode: %1").arg(launchModeName(runtimeContext.launchMode));
    lines << QStringLiteral("Portable bootstrap file: %1")
                 .arg(runtimeContext.portableBootstrapPath.isEmpty() ? QStringLiteral("<none>") : runtimeContext.portableBootstrapPath);
    lines << QStringLiteral("Custom data root active: %1").arg(runtimeContext.dataRootOverride.isEmpty() ? QStringLiteral("false") : QStringLiteral("true"));
    lines << QStringLiteral("Status text: %1").arg(redactSensitiveText(m_statusText));
    lines << QStringLiteral("Busy: %1").arg(m_isBusy ? QStringLiteral("true") : QStringLiteral("false"));
    lines << QStringLiteral("Data directory: %1").arg(Core::AppDataPaths::dataDirectory());
    lines << QStringLiteral("Settings file: %1").arg(m_settings->settingsFilePath());
    lines << QStringLiteral("Settings load error: %1")
                 .arg(m_settings->lastLoadError().trimmed().isEmpty()
                          ? QStringLiteral("<none>")
                          : redactSensitiveText(m_settings->lastLoadError()));
    lines << QStringLiteral("Settings save error: %1")
                 .arg(m_settings->lastSaveError().trimmed().isEmpty()
                          ? QStringLiteral("<none>")
                          : redactSensitiveText(m_settings->lastSaveError()));
    lines << QStringLiteral("Database file: %1").arg(Core::AppDataPaths::databaseFile());
    const auto sessionLogPath = Core::DebugLogger::instance().sessionLogPath();
    lines << QStringLiteral("Session log: %1")
                 .arg(sessionLogPath.isEmpty() ? QStringLiteral("<disabled>") : sessionLogPath);
    lines << QStringLiteral("Loaded channel count: %1").arg(m_loadedChannels.size());
    lines << QStringLiteral("Active profile id: %1").arg(activeProfileId());
    lines << QStringLiteral("Active profile name: %1").arg(profile.has_value() ? profile->name : QStringLiteral("<none>"));
    lines << QStringLiteral("Current channel: %1").arg(channel.has_value() ? channel->name : QStringLiteral("<none>"));
    lines << QStringLiteral("Current stream url: %1")
                 .arg(channel.has_value() ? redactSensitiveUrl(channel->streamUrl) : QStringLiteral("<none>"));
    lines << QStringLiteral("Player diagnostics: %1")
                 .arg(redactSensitiveText(m_playerController->player()->diagnostics()));
    lines << QStringLiteral("mpv DLL path: %1")
                 .arg(settings.mpvDllPath.isEmpty() ? QStringLiteral("<bundled default>") : settings.mpvDllPath);
    lines << QStringLiteral("Player wait-for-stream seconds: %1")
                 .arg(settings.playerWaitForStreamSeconds, 0, 'f', 1);
    lines << QStringLiteral("Player image smoothing enabled: %1")
                 .arg(settings.playerImageSmoothingEnabled ? QStringLiteral("true") : QStringLiteral("false"));
    lines << QStringLiteral("Player deinterlace enabled: %1")
                 .arg(settings.playerDeinterlaceEnabled ? QStringLiteral("true") : QStringLiteral("false"));
    lines << QStringLiteral("Player buffer seconds: %1")
                 .arg(settings.playerBufferSeconds, 0, 'f', 1);
    lines << QStringLiteral("Player user-agent: %1")
                 .arg(settings.playerUserAgent.trimmed().isEmpty()
                          ? QStringLiteral("<default>")
                          : redactSensitiveText(settings.playerUserAgent.trimmed()));
    lines << QStringLiteral("mpv option count: %1").arg(settings.mpvOptions.size());
    for (auto it = settings.mpvOptions.cbegin(); it != settings.mpvOptions.cend(); ++it) {
        lines << QStringLiteral("  mpv.%1=%2").arg(it.key(), redactSensitiveText(it.value()));
    }
    lines << QStringLiteral("Auto refresh EPG: %1").arg(settings.autoRefreshEpg ? QStringLiteral("true") : QStringLiteral("false"));
    lines << QStringLiteral("Refresh interval minutes: %1").arg(settings.refreshIntervalMinutes);
    lines << QStringLiteral("Guide past hours: %1").arg(settings.guidePastHours);
    lines << QStringLiteral("EPG lookahead hours: %1").arg(settings.epgLookAheadHours);
    lines << QStringLiteral("EPG cache file: %1")
                 .arg(profile.has_value() ? Core::EpgCacheService::manifestFile(profile->id) : QStringLiteral("<none>"));
    const auto epgSnapshot = m_epgService->snapshot();
    lines << QStringLiteral("EPG programmes: %1; query cache bytes: %2 (budget 33554432)")
        .arg(epgSnapshot->totalEntries).arg(epgSnapshot->store ? epgSnapshot->store->cachedBytes() : 0);
    if (epgSnapshot->store) lines << QStringLiteral("EPG store: %1").arg(epgSnapshot->store->diagnostics());
    lines << QStringLiteral("EPG loaded profile id: %1")
                 .arg(m_epgLoadedProfileId.isNull() ? QStringLiteral("<none>") : guidToString(m_epgLoadedProfileId));
    lines << QStringLiteral("EPG fetched at UTC: %1").arg(formatDateTimeUtc(m_epgFetchedAt));
    lines << QStringLiteral("EPG next refresh at UTC: %1").arg(formatDateTimeUtc(m_epgNextRefreshAt));
    lines << QStringLiteral("EPG age seconds: %1")
                 .arg(Core::EpgCacheService::ageSeconds(m_epgFetchedAt, QDateTime::currentDateTimeUtc()));
    lines << QStringLiteral("EPG refresh due in seconds: %1")
                 .arg(m_epgNextRefreshAt.isValid()
                          ? std::max<qint64>(0, QDateTime::currentDateTimeUtc().secsTo(m_epgNextRefreshAt))
                          : -1);
    lines << QStringLiteral("EPG last refresh error: %1")
                 .arg(m_epgLastRefreshError.isEmpty() ? QStringLiteral("<none>") : redactSensitiveText(m_epgLastRefreshError));
    lines << QStringLiteral("Environment QT_OPENGL=%1")
                 .arg(QProcessEnvironment::systemEnvironment().value(QStringLiteral("QT_OPENGL"), QStringLiteral("<unset>")));
    lines << QStringLiteral("Environment QSG_RHI_BACKEND=%1")
                 .arg(QProcessEnvironment::systemEnvironment().value(QStringLiteral("QSG_RHI_BACKEND"), QStringLiteral("<unset>")));
    return lines.join(u'\n');
}

QString AppController::debugSummary() const
{
    return buildDebugSummary();
}

void AppController::setStatusText(const QString &value)
{
    if (m_statusText == value) {
        return;
    }

    m_statusText = value;
    Core::DebugLogger::instance().log(QStringLiteral("status"), value);
    emit statusTextChanged();
}

void AppController::setBusy(const bool value)
{
    if (m_isBusy == value) {
        return;
    }

    m_isBusy = value;
    emit isBusyChanged();
}

void AppController::setEpgCacheBootstrapPending(const bool value)
{
    if (m_epgCacheBootstrapPending == value) {
        return;
    }

    m_epgCacheBootstrapPending = value;
    emit epgRefreshStateChanged();
}

void AppController::triggerScheduledSourceAutoRefresh()
{
    if (m_isBusy) {
        return;
    }

    const auto nowUtc = QDateTime::currentDateTimeUtc();
    QStringList dueProfileIds;
    const auto sourceSummaries = m_settings->sourceSummaries();
    dueProfileIds.reserve(sourceSummaries.size());

    for (const auto &summary : sourceSummaries) {
        if (summary.type == ProfileType::M3UFile) {
            continue;
        }

        const auto intervalHours = std::max(0, summary.autoRefreshIntervalHours);
        if (intervalHours == 0) {
            continue;
        }

        if (!summary.lastRefreshed.isValid()) {
            dueProfileIds.push_back(guidToString(summary.id));
            continue;
        }

        const auto elapsedSeconds = summary.lastRefreshed.toUTC().secsTo(nowUtc);
        if (elapsedSeconds < 0) {
            continue;
        }

        const auto intervalSeconds = static_cast<qint64>(intervalHours) * 60 * 60;
        if (elapsedSeconds >= intervalSeconds) {
            dueProfileIds.push_back(guidToString(summary.id));
        }
    }

    if (dueProfileIds.isEmpty()) {
        return;
    }

    qsizetype selectedDueProfileIndex = 0;
    const auto lastDueProfileIndex = dueProfileIds.indexOf(m_lastAutoRefreshProfileId);
    if (lastDueProfileIndex >= 0) {
        selectedDueProfileIndex = (static_cast<qsizetype>(lastDueProfileIndex) + 1) % dueProfileIds.size();
    }

    const auto profileId = dueProfileIds.at(static_cast<int>(selectedDueProfileIndex));
    m_lastAutoRefreshProfileId = profileId;
    Core::DebugLogger::instance().log(
        QStringLiteral("profile"),
        QStringLiteral("Running scheduled source refresh for profile %1.").arg(profileId));
    loadProfile(profileId);
}

void AppController::updateRefreshTimer()
{
    const auto &settings = m_settings->current();
    const auto profile = m_settings->activeProfile();
    if (!settings.autoRefreshEpg
        || settings.refreshIntervalMinutes <= 0
        || !profile.has_value()
        || EpgCacheService::sourceFingerprint(profile.value()).isEmpty()
        || m_epgLoadedProfileId != profile->id
        || !m_epgNextRefreshAt.isValid()) {
        m_refreshTimer.stop();
        return;
    }

    if (m_epgFetchedAt.isValid() && m_epgLastRefreshError.isEmpty()) {
        m_epgNextRefreshAt = EpgCacheService::nextRefreshAt(m_epgFetchedAt, settings.refreshIntervalMinutes);
    }

    const auto delayMs = std::max<qint64>(0, QDateTime::currentDateTimeUtc().msecsTo(m_epgNextRefreshAt));
    m_refreshTimer.start(static_cast<int>(delayMs));
}

void AppController::triggerScheduledEpgRefresh()
{
    const auto profile = m_settings->activeProfile();
    if (!profile.has_value()) {
        return;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("epg"),
        QStringLiteral("Running scheduled EPG refresh for %1.").arg(profile->name));
    loadEpgAsync(profile.value());
}

void AppController::applyEpgSnapshot(
    const quint64 generation,
    const QUuid &profileId,
    const std::shared_ptr<const EpgService::Snapshot> &snapshot,
    const QDateTime &fetchedAt,
    const QString &errorText,
    const bool scheduleRefresh,
    const bool setRefreshError,
    const bool finishRefreshState,
    const bool cacheBootstrapPending)
{
    if (generation != m_epgLoadGeneration) {
        return;
    }

    m_catchupEpgWindows.clear();
    m_pendingCatchupSamples.clear();
    m_epgService->applySnapshot(snapshot);
    m_epgLoadedProfileId = profileId;
    m_epgFetchedAt = fetchedAt;
    m_epgNextRefreshAt = scheduleRefresh
        ? EpgCacheService::nextRefreshAt(fetchedAt, m_settings->current().refreshIntervalMinutes)
        : QDateTime {};
    if (setRefreshError) {
        m_epgLastRefreshError = errorText;
    }
    if (m_manualEpgRefreshPending && finishRefreshState) {
        if (errorText.isEmpty()) {
            setStatusText(QStringLiteral("EPG refreshed at %1.").arg(epgLastRefreshText()));
        } else {
            setStatusText(QStringLiteral("EPG refresh failed: %1").arg(errorText));
        }
        m_manualEpgRefreshPending = false;
    }
    if (finishRefreshState && m_epgRefreshInProgress) {
        m_epgRefreshInProgress = false;
    }
    setEpgCacheBootstrapPending(cacheBootstrapPending);
    emit epgRefreshStateChanged();
    updateRefreshTimer();
    m_nowNextModel->refresh();
    m_playbackNowNextModel->refresh();
    m_guideStateModel->refresh();

    QMetaObject::invokeMethod(
        this,
        [this, generation]() {
            if (generation != m_epgLoadGeneration) {
                return;
            }

            rebuildGuideGridAsync();
            updateChannelProgrammeMetadata();
        },
        Qt::QueuedConnection);
}

void AppController::clearEpg(const QUuid &profileId, const QString &errorText)
{
    m_epgService->clear();
    m_epgLoadedProfileId = profileId;
    m_epgFetchedAt = {};
    m_epgNextRefreshAt = {};
    m_epgLastRefreshError = errorText;
    if (m_manualEpgRefreshPending) {
        setStatusText(errorText.isEmpty()
            ? QStringLiteral("No EPG data is available for the active source.")
            : QStringLiteral("EPG refresh failed: %1").arg(errorText));
        m_manualEpgRefreshPending = false;
    }
    if (m_epgRefreshInProgress) {
        m_epgRefreshInProgress = false;
    }
    setEpgCacheBootstrapPending(false);
    emit epgRefreshStateChanged();
    updateRefreshTimer();
    m_nowNextModel->refresh();
    m_playbackNowNextModel->refresh();
    m_guideStateModel->refresh();

    QMetaObject::invokeMethod(
        this,
        [this]() {
            rebuildGuideGridAsync();
            updateChannelProgrammeMetadata();
        },
        Qt::QueuedConnection);
}

void AppController::updateChannelProgrammeMetadata()
{
    if (m_programInfoRefreshInFlight) {
        m_programInfoRefreshQueued = true;
        return;
    }

    m_programInfoRefreshInFlight = true;
    const auto generation = ++m_programInfoGeneration;
    const auto channels = m_loadedChannels;

    m_backgroundTasks.addFuture(QtConcurrent::run(EpgService::readPool(), [this, generation, channels, snapshot = m_epgService->snapshot()]() {
        EpgService reader; reader.applySnapshot(snapshot);
        QStringList ids;
        for (const auto &channel : channels) ids.push_back(channel.tvgId);
        const auto now = QDateTime::currentDateTimeUtc();
        const auto programmes = reader.programsForChannels(ids, now, now.addMSecs(1), 1, true);
        QHash<int, QVariantMap> infoByChannelId;
        for (const auto &channel : channels) {
            if (channel.tvgId.trimmed().isEmpty()) {
                continue;
            }

            const auto entries = programmes.value(channel.tvgId.trimmed().toLower());
            if (entries.isEmpty()) {
                continue;
            }

            infoByChannelId.insert(
                channel.id,
                toVariantMap(entries.first()));
        }

        QMetaObject::invokeMethod(
            this,
            [this, generation, infoByChannelId]() {
                if (generation == m_programInfoGeneration) {
                    m_channelListModel->setCurrentProgramInfo(infoByChannelId);
                }

                m_programInfoRefreshInFlight = false;
                if (!m_programInfoRefreshQueued) {
                    return;
                }

                m_programInfoRefreshQueued = false;
                QMetaObject::invokeMethod(
                    this,
                    [this]() {
                        updateChannelProgrammeMetadata();
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }));
}

QList<Channel> AppController::guideChannels() const
{
    QList<Channel> filteredChannels;
    filteredChannels.reserve(m_loadedChannels.size());
    const auto groupId = m_channelListModel->selectedCategoryId();
    const auto hiddenGroups = m_settings->current().hiddenGroupsByProfile.value(activeProfileId());
    for (const auto &channel : m_loadedChannels) {
        const auto categoryId = normalizeChannelCategoryId(channel.categoryId);
        if (groupId == QString::fromUtf8(kFavouritesCategoryId)) {
            if (m_channelListModel->isFavorite(channel.id)) {
                filteredChannels.push_back(channel);
            }
            continue;
        }

        if (groupId.isEmpty()) {
            if (!hiddenGroups.contains(categoryId)) {
                filteredChannels.push_back(channel);
            }
            continue;
        }

        if (categoryId == groupId) {
            filteredChannels.push_back(channel);
        }
    }

    return filteredChannels;
}

bool AppController::syncProfileGroupPreferences(const QUuid &profileId, const QList<Channel> &channels)
{
    QStringList discoveredGroups;
    QSet<QString> seenGroups;
    discoveredGroups.reserve(channels.size());

    for (const auto &channel : channels) {
        const auto groupId = normalizeChannelCategoryId(channel.categoryId);
        if (!seenGroups.contains(groupId)) {
            seenGroups.insert(groupId);
            discoveredGroups.push_back(groupId);
        }
    }

    return syncProfileGroupIds(profileId, std::move(discoveredGroups));
}

bool AppController::syncProfileGroupIds(const QUuid &profileId, QStringList discoveredGroups)
{
    const auto profileKey = guidToString(profileId);
    auto &hiddenGroups = m_settings->current().hiddenGroupsByProfile[profileKey];
    auto &groupOrder = m_settings->current().groupOrderByProfile[profileKey];
    const auto favouritesGroupId = QString::fromUtf8(kFavouritesCategoryId);
    if (!discoveredGroups.isEmpty() && !discoveredGroups.contains(favouritesGroupId)) {
        discoveredGroups.prepend(favouritesGroupId);
    }

    const auto reconciled = reconcileSourceGroups(discoveredGroups, { hiddenGroups, groupOrder });
    const auto settingsChanged = hiddenGroups != reconciled.hiddenGroups || groupOrder != reconciled.groupOrder;
    hiddenGroups = reconciled.hiddenGroups;
    groupOrder = reconciled.groupOrder;
    return settingsChanged;
}

void AppController::beginWatchTrackingForCurrentChannel()
{
    const auto currentChannel = m_playerController->currentChannelValue();
    if (!currentChannel.has_value()) {
        m_watchTrackingProfileId = QUuid {};
        m_watchTrackingChannelId = -1;
        m_watchTrackingActive = false;
        m_watchStatsFlushTimer.stop();
        return;
    }

    m_watchTrackingProfileId = currentChannel->profileId;
    m_watchTrackingChannelId = currentChannel->id;
    m_watchTrackingElapsed.restart();
    m_watchTrackingActive = true;
    if (!m_watchStatsFlushTimer.isActive()) {
        m_watchStatsFlushTimer.start();
    }
}

void AppController::flushTrackedWatchSeconds()
{
    if (m_watchTrackingActive && m_watchTrackingChannelId >= 0
        && !m_watchTrackingProfileId.isNull() && m_watchTrackingElapsed.isValid()) {
        const auto elapsedSeconds = std::max<qint64>(0, m_watchTrackingElapsed.elapsed() / 1000);
        if (elapsedSeconds > 0) {
            m_pendingWatchSeconds[m_watchTrackingProfileId][m_watchTrackingChannelId] += elapsedSeconds;
        }
        if (m_playerController->isPlaying()) {
            m_watchTrackingElapsed.restart();
        } else {
            m_watchTrackingActive = false;
        }
    }

    for (auto profile = m_pendingWatchSeconds.begin(); profile != m_pendingWatchSeconds.end();) {
        for (auto channel = profile->begin(); channel != profile->end();) {
            try {
                m_database->incrementWatchSeconds(profile.key(), channel.key(), channel.value());
            } catch (const std::exception &error) {
                Core::DebugLogger::instance().log(QStringLiteral("watch-stats"),
                    QStringLiteral("Watch time save deferred: %1").arg(QString::fromUtf8(error.what())));
                // Retain the original source/channel even across playback changes.
                return;
            }
            if (guidToString(profile.key()) == m_channelListModel->activeProfileId()) {
                m_watchSecondsByChannelId[channel.key()] += channel.value();
                m_channelListModel->setWatchSeconds(m_watchSecondsByChannelId);
                if (m_channelListModel->selectedCategoryId() == QString::fromUtf8(kFavouritesCategoryId)) {
                    rebuildGuideGridAsync();
                }
            }
            channel = profile->erase(channel);
        }
        profile = m_pendingWatchSeconds.erase(profile);
    }
}

void AppController::rebuildGuideGrid()
{
    m_guideRebuildTimer.stop();
    m_guideRebuildAsyncRequested = false;
    m_epgGridModel->rebuild(
        guideChannels(),
        m_settings->current().guidePastHours,
        m_settings->current().epgLookAheadHours);
}

void AppController::rebuildGuideGridAsync()
{
    scheduleGuideGridRebuild(true);
}

void AppController::scheduleGuideGridRebuild(const bool asyncRequested)
{
    // Invalidate immediately, including the coalescing interval before the next worker starts.
    m_epgGridModel->invalidateRebuild();
    m_guideRebuildAsyncRequested = m_guideRebuildAsyncRequested || asyncRequested;
    if (!m_guideRebuildTimer.isActive()) {
        m_guideRebuildTimer.start();
    }
}

bool AppController::activatePreviousChannel()
{
    if (m_previousPlaybackChannelId < 0) {
        return false;
    }
    activateChannel(m_previousPlaybackChannelId);
    return true;
}

void AppController::savePlaybackForApplicationExit()
{
    m_playerController->checkpointCatchupProgress();
    auto &settings = m_settings->current();
    settings.playerVolume = m_playerController->volume();
    settings.lastCatchupSession = {};
    const auto channel = m_playerController->currentChannelValue();
    if (channel.has_value()) {
        // Keep startup on the playback source even if another source is being browsed.
        m_settings->setActiveProfileId(channel->profileId);
        settings.lastWatchedChannelId[guidToString(channel->profileId)] = channel->id;
        if (m_playerController->inCatchupMode()
            && m_observedCatchupSession.value(QStringLiteral("profileId")).toString() == guidToString(channel->profileId)
            && m_observedCatchupSession.value(QStringLiteral("channelId")).toInt(-1) == channel->id) {
            settings.lastCatchupSession = m_observedCatchupSession;
        }
    }
    m_settings->save();
}

bool AppController::restoreStartupCatchup(const Core::Channel &channel)
{
    const auto session = std::exchange(m_startupCatchupSession, QJsonObject {});
    if (session.value(QStringLiteral("profileId")).toString() != guidToString(channel.profileId)
        || session.value(QStringLiteral("channelId")).toInt(-1) != channel.id) {
        return false;
    }
    const auto program = session.value(QStringLiteral("program")).toObject().toVariantMap();
    const auto channelVariant = toVariantMap(channel);
    const auto validation = validateCatchupRequest(m_settings, m_settings->current(), m_channelListModel, channelVariant, program);
    const auto entry = validation.program;
    const auto positionMs = session.value(QStringLiteral("positionMs")).toInteger(-1);
    if (!validation.enabled || !entry.has_value() || positionMs < 0
        || entry->start.msecsTo(entry->stop) - positionMs <= 5LL * 60 * 1000
        || session.value(QStringLiteral("key")).toString() != CatchupProgress::keyFor(channel, entry->start)
        || !CatchupUrlResolver(validation.profile).resolve(channel, entry.value()).has_value()) {
        return false;
    }
    // Use the same minute precision as Guide resume, but test the five-minute
    // threshold against the unrounded, observed position.
    m_restoredCatchupProgram = entry;
    m_restoredCatchupProgramKey = CatchupProgress::keyFor(channel, entry->start);
    const auto roundedSeconds = (positionMs / 60000) * 60;
    playCatchupAtOffsetInternal(channelVariant, program, static_cast<double>(roundedSeconds));
    return true;
}

void AppController::setTimeshiftProgram(const QVariantMap &program)
{
    const auto start = program.value(QStringLiteral("start")).toDateTime();
    const auto canRestart = start.isValid()
        && m_timeshiftController->containsPlaybackTime(start.toMSecsSinceEpoch());
    const auto title = program.value(QStringLiteral("title")).toString();
    const auto titleChanged = m_timeshiftProgramTitle != title;
    const auto programChanged = m_timeshiftProgram != program || m_timeshiftProgramCanRestartLocally != canRestart;
    m_timeshiftProgram = program;
    m_timeshiftProgramCanRestartLocally = canRestart;
    m_timeshiftProgramTitle = title;
    if (titleChanged) emit timeshiftProgramTitleChanged();
    if (programChanged) emit timeshiftProgramChanged();
}

bool AppController::restartTimeshiftProgramme()
{
    refreshTimeshiftProgram();
    const auto program = m_timeshiftProgram;
    const auto channel = m_playerController->currentChannelValue();
    if (program.isEmpty() || !channel) return false;
    const auto start = program.value(QStringLiteral("start")).toDateTime();
    if (start.isValid() && m_timeshiftController->seekToPlaybackTime(start.toMSecsSinceEpoch())) return true;
    // Revalidate provider eligibility at activation, including expired archives.
    playCatchup(toVariantMap(*channel), program);
    return false;
}

void AppController::refreshTimeshiftProgram()
{
    const auto channel = m_playerController->currentChannelValue();
    const auto epochMs = m_timeshiftController->currentPlaybackEpochMs();
    if (!m_timeshiftController->isActive() || epochMs <= 0 || !channel
        || channel->tvgId.trimmed().isEmpty() || channel->profileId != m_epgLoadedProfileId
        || m_multiViewController->focusedController() != m_playerController) {
        m_timeshiftEpgKey.clear();
        m_timeshiftEpgWindow = {};
        setTimeshiftProgram({});
        return;
    }

    const auto key = guidToString(channel->profileId) + u':' + QString::number(channel->id)
        + u':' + channel->tvgId.trimmed().toLower();
    const auto time = QDateTime::fromMSecsSinceEpoch(epochMs, QTimeZone::UTC);
    const auto snapshot = m_epgService->snapshot();
    if (key == m_timeshiftEpgKey && snapshot == m_timeshiftEpgWindow.snapshot
        && m_timeshiftEpgWindow.from <= time && time < m_timeshiftEpgWindow.to) {
        for (const auto &entry : m_timeshiftEpgWindow.entries) {
            if (entry.start <= time && time < entry.stop) {
                setTimeshiftProgram(toVariantMap(entry));
                return;
            }
        }
        setTimeshiftProgram({});
        return;
    }

    // Never retain a title from a different channel, snapshot or watched range.
    setTimeshiftProgram({});
    if (m_timeshiftEpgReadInFlight) return;
    m_timeshiftEpgReadInFlight = true;
    m_backgroundTasks.addFuture(QtConcurrent::run(EpgService::readPool(),
        [this, key, tvgId = channel->tvgId, time, snapshot] {
            CatchupEpgWindow window {snapshot, time.addSecs(-1800), time.addSecs(7200), {}};
            try {
                EpgService reader;
                reader.applySnapshot(snapshot);
                window.entries = reader.programsForChannels({tvgId}, window.from, window.to, -1, true)
                    .value(tvgId.trimmed().toLower());
            } catch (const std::exception &error) {
                DebugLogger::instance().log(QStringLiteral("epg.read"), QString::fromUtf8(error.what()));
            }
            QMetaObject::invokeMethod(this, [this, key, window = std::move(window)] {
                m_timeshiftEpgReadInFlight = false;
                m_timeshiftEpgKey = key;
                m_timeshiftEpgWindow = window;
                // Re-sample identity, EPG generation and position: a seek or stop
                // may have happened while this immutable window was being read.
                refreshTimeshiftProgram();
            }, Qt::QueuedConnection);
        }));
}

std::optional<Core::EpgEntry> AppController::catchupProgramAt(const Core::Channel &channel, const QDateTime &time,
    const std::optional<Core::EpgEntry> &validatedProgram) const
{
    if (m_epgLoadedProfileId == channel.profileId) {
        QList<EpgEntry> candidates;
        const auto snapshot = m_epgService->snapshot();
        if (snapshot->store) {
            const auto key = guidToString(channel.profileId) + u':' + channel.tvgId.trimmed().toLower();
            const auto cached = m_catchupEpgWindows.constFind(key);
            if (cached != m_catchupEpgWindows.cend() && cached->snapshot == snapshot
                && cached->from <= time && time < cached->to) candidates = cached->entries;
            else const_cast<AppController *>(this)->requestCatchupPrograms(channel, time);
        } else candidates = m_epgService->programsInRange(channel.tvgId, time, time.addMSecs(1));
        for (const auto &program : candidates)
            if (program.start <= time && time < program.stop) return program;
    }
    if (validatedProgram && validatedProgram->start <= time && time < validatedProgram->stop) {
        return validatedProgram;
    }
    // Startup must not depend on downloading EPG again. Retain only the saved
    // programme, and stop using it as soon as the media clock leaves its bounds.
    const auto &restored = m_restoredCatchupProgram;
    if (restored.has_value() && restored->start <= time && time < restored->stop
        && m_restoredCatchupProgramKey == CatchupProgress::keyFor(channel, restored->start)) {
        return restored;
    }
    return std::nullopt;
}

void AppController::requestCatchupPrograms(const Core::Channel &channel, const QDateTime &time)
{
    const auto key = guidToString(channel.profileId) + u':' + channel.tvgId.trimmed().toLower();
    if (m_catchupEpgPending.contains(key)) return;
    m_catchupEpgPending.insert(key);
    const auto snapshot = m_epgService->snapshot();
    m_backgroundTasks.addFuture(QtConcurrent::run(EpgService::readPool(), [this, key, channel, time, snapshot] {
        CatchupEpgWindow window {snapshot, time.addSecs(-1800), time.addSecs(7200), {}};
        try { window.entries = snapshot->store->range(channel.tvgId, window.from, window.to); }
        catch (const std::exception &error) { DebugLogger::instance().log(QStringLiteral("epg.read"), QString::fromUtf8(error.what())); }
        QMetaObject::invokeMethod(this, [this, key, window = std::move(window)] {
            m_catchupEpgPending.remove(key);
            if (window.snapshot != m_epgService->snapshot()) return;
            if (m_catchupEpgWindows.size() >= 8) m_catchupEpgWindows.clear();
            m_catchupEpgWindows.insert(key, window);
            const auto samples = std::exchange(m_pendingCatchupSamples, {});
            for (const auto &pending : samples) {
                const auto player = pending.first;
                const auto &sample = pending.second;
                if (!player || !player->inCatchupMode()) continue;
                const auto current = player->currentChannelValue();
                if (!current || current->profileId != sample.channel.profileId || current->id != sample.channel.id) continue;
                const auto watched = QDateTime::fromMSecsSinceEpoch(player->catchupTimelineStartEpochMs(), QTimeZone::UTC)
                    .addMSecs(qRound64(player->catchupTimelinePositionSeconds() * 1000));
                if (qAbs(watched.msecsTo(sample.watchedTime)) > 2000) continue;
                player->updateCatchupProgramme(catchupProgramAt(*current, watched, player->validatedCatchupProgramme()));
                recordCatchupProgress(sample, player);
            }
        }, Qt::QueuedConnection);
    }));
}

void AppController::recordCatchupProgress(const CatchupProgressSample &sample, PlayerController *source)
{
    source = source ? source : m_playerController;
    if (source == m_playerController) {
            m_observedCatchupSession = {};
        }
        auto &sessionKeyState = m_catchupProgressSessionKeys[source];
        auto &programmeKeyState = m_catchupProgressProgrammeKeys[source];
        if (!sample.watchedTime.isValid() || sample.channel.profileId.isNull()
            || sample.channel.catchupWindowHours <= 0) {
            return;
        }
        auto start = sample.programStart;
        auto stop = sample.programStop;
        auto programMetadata = source->catchupCurrentProgram();
        if (sample.endless) {
            const auto program = catchupProgramAt(sample.channel, sample.watchedTime, source->validatedCatchupProgramme());
            if (!program.has_value()) {
                if (m_epgService->snapshot()->store && m_epgLoadedProfileId == sample.channel.profileId)
                    m_pendingCatchupSamples.insert(source, {QPointer<PlayerController>(source), sample});
                if (source == m_playerController) {
                    m_observedCatchupSession = {};
                }
                return;
            }
            start = program->start;
            stop = program->stop;
            programMetadata = toVariantMap(program.value());
        }
        if (!start.isValid() || !stop.isValid() || stop <= start || sample.watchedTime < start) {
            return;
        }
        programMetadata.insert(QStringLiteral("channelId"), sample.channel.tvgId);
        programMetadata.insert(QStringLiteral("start"), start.toString(Qt::ISODateWithMs));
        programMetadata.insert(QStringLiteral("stop"), stop.toString(Qt::ISODateWithMs));
        if (source == m_playerController) {
        m_observedCatchupSession = {
            { QStringLiteral("profileId"), guidToString(sample.channel.profileId) },
            { QStringLiteral("channelId"), sample.channel.id },
            { QStringLiteral("key"), CatchupProgress::keyFor(sample.channel, start) },
            { QStringLiteral("program"), QJsonObject::fromVariantMap(programMetadata) },
            { QStringLiteral("positionMs"), start.msecsTo(sample.watchedTime) },
            { QStringLiteral("endless"), sample.endless }
        };
    }
    const auto key = CatchupProgress::keyFor(sample.channel, start);
    const auto sessionKey = CatchupProgress::keyFor(sample.channel, sample.programStart);
    bool completedPrevious = false;
    if (sample.endless && sessionKeyState == sessionKey && programmeKeyState != key) {
        auto previousProgramme = m_catchupProgress.find(programmeKeyState);
        if (previousProgramme != m_catchupProgress.end() && previousProgramme->programStopMs <= start.toMSecsSinceEpoch()
            && previousProgramme->positionMs > 0) {
            // A delayed UI tick can miss the final minute entirely. A forward
            // programme transition in the same endless session still completes A.
            previousProgramme->positionMs = 0;
            m_dirtyCatchupProgress.insert(previousProgramme.key());
            completedPrevious = true;
        }
    }
    sessionKeyState = sample.endless ? sessionKey : QString {};
    programmeKeyState = sample.endless ? key : QString {};
    const auto previous = m_catchupProgress.value(key);
    const auto durationMs = start.msecsTo(stop);
    const auto positionMs = std::clamp(start.msecsTo(sample.watchedTime), qint64 { 0 }, durationMs);
    CatchupProgress progress { key, sample.channel.profileId, start.toMSecsSinceEpoch(), stop.toMSecsSinceEpoch(),
        positionMs, start.addSecs(static_cast<qint64>(sample.channel.catchupWindowHours) * 3600).toMSecsSinceEpoch() };
    // Keep a zero-position entry in memory until flush so a completed programme
    // or a successful restart immediately overrides its older database record.
    if (progress.resumeSeconds(progress.programStopMs) == 0) {
        progress.positionMs = 0;
    }
    if (previous.positionMs == progress.positionMs && previous.programStopMs == progress.programStopMs
        && previous.expiresAtMs == progress.expiresAtMs) {
        if (completedPrevious) {
            emit catchupProgressChanged();
        }
        return;
    }
    m_catchupProgress.insert(key, progress);
    m_dirtyCatchupProgress.insert(key);
    if (completedPrevious || previous.resumeSeconds(stop.toMSecsSinceEpoch()) != progress.resumeSeconds(stop.toMSecsSinceEpoch())) {
        emit catchupProgressChanged();
    }
}

void AppController::flushCatchupProgress()
{
    const auto dirtyKeys = m_dirtyCatchupProgress;
    for (const auto &key : dirtyKeys) {
        try {
            m_database->saveCatchupProgress(m_catchupProgress.value(key));
            m_dirtyCatchupProgress.remove(key);
        } catch (const std::exception &error) {
            DebugLogger::instance().log(QStringLiteral("catchup.progress"),
                QStringLiteral("Cannot save progress: %1").arg(QString::fromUtf8(error.what())));
            break; // Retain dirty entries for the next checkpoint.
        }
    }
}

void AppController::pruneCatchupProgress()
{
    QSet<QUuid> profiles;
    const auto summaries = m_settings->sourceSummaries();
    for (const auto &profile : summaries) {
        profiles.insert(profile.id);
    }
    const auto nowMs = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    for (auto it = m_catchupProgress.begin(); it != m_catchupProgress.end();) {
        if (it->expiresAtMs > nowMs && profiles.contains(it->profileId)) {
            ++it;
            continue;
        }
        try {
            m_database->removeCatchupProgress(it.key());
            m_dirtyCatchupProgress.remove(it.key());
            it = m_catchupProgress.erase(it);
            changed = true;
        } catch (const std::exception &error) {
            DebugLogger::instance().log(QStringLiteral("catchup.progress"),
                QStringLiteral("Cannot prune progress: %1").arg(QString::fromUtf8(error.what())));
            break;
        }
    }
    if (changed) {
        emit catchupProgressChanged();
    }
}

QVariantMap AppController::catchupActionState(const QVariantMap &channel, const QVariantMap &program) const
{
    const auto validation = validateCatchupRequest(m_settings, m_settings->current(), m_channelListModel, channel, program);
    const auto safetySeconds = 60 * std::clamp(validation.profile.has_value() ? validation.profile->catchupSafetyMinutes : 3, 0, 30);
    qint64 resumeSeconds = 0;
    const auto resolvedChannel = validation.channel;
    const auto resolvedProgram = validation.program;
    if (validation.enabled && resolvedChannel.has_value() && resolvedProgram.has_value()) {
        const auto progress = m_catchupProgress.value(CatchupProgress::keyFor(resolvedChannel.value(), resolvedProgram->start));
        if (progress.expiresAtMs > QDateTime::currentMSecsSinceEpoch()) {
            const auto availableSeconds = std::max<qint64>(0, resolvedProgram->start.secsTo(
                CatchupUrlResolver::availableEdge(resolvedProgram->stop, safetySeconds)));
            resumeSeconds = std::min(progress.resumeSeconds(resolvedProgram->stop.toMSecsSinceEpoch()),
                (availableSeconds / 60) * 60);
        }
    }
    return {
        { QStringLiteral("resumeAvailable"), resumeSeconds > 0 },
        { QStringLiteral("resumeSeconds"), resumeSeconds },
        { QStringLiteral("visible"), validation.visible },
        { QStringLiteral("enabled"), validation.enabled },
        { QStringLiteral("safetySeconds"), safetySeconds },
        { QStringLiteral("reason"), validation.reason }
    };
}

QVariantMap AppController::catchupDownloadActionState(const QVariantMap &channel, const QVariantMap &program) const
{
    const auto stop = parseIsoUtc(program.value(QStringLiteral("stop")).toString());
    const bool visible = stop.isValid() && stop <= QDateTime::currentDateTimeUtc();
    const auto validation = validateCatchupRequest(m_settings, m_settings->current(), m_channelListModel, channel, program);
    QString reason = validation.reason;
    if (validation.enabled) {
        const auto profile = validation.profile;
        const auto entry = validation.program;
        const int safety = 60 * std::clamp(profile ? profile->catchupSafetyMinutes : 3, 0, 30);
        if (!profile || !entry)
            reason = QStringLiteral("Source or programme is unavailable.");
        else if (CatchupUrlResolver::availableEdge(entry->stop, safety) < entry->stop)
            reason = QStringLiteral("Download is available after the programme ends and the source archive margin (%1 minutes) has elapsed.").arg(safety / 60);
        else if (!Core::ffmpegToolsAvailable())
            reason = QStringLiteral("Downloading requires ffmpeg and ffprobe in the application directory or PATH.");
        else {
            const auto resolved = validation.channel;
            if (resolved) {
                const auto target = CatchupUrlResolver(profile).resolveDownload(*resolved, *entry, &reason);
                if (target && !CatchupDownloadTransfer::supportedUrl(QUrl(target->url), true))
                    reason = QStringLiteral("Downloads support HTTP/HTTPS media and finite HLS archives; DASH is not supported.");
            }
        }
    }
    return {{QStringLiteral("visible"), visible},
            {QStringLiteral("enabled"), validation.enabled && reason.isEmpty()},
            {QStringLiteral("reason"), reason}};
}

void AppController::resolveEpgDetails(const QVariantMap &channel, const QVariantMap &program,
    const std::function<void(QVariantMap, QString)> &completed)
{
    const auto snapshot = m_epgService->snapshot();
    if (!program.value(QStringLiteral("detailsPending")).toBool() || !snapshot->store) {
        QMetaObject::invokeMethod(this, [completed, program] { completed(program, {}); }, Qt::QueuedConnection);
        return;
    }
    const auto profileId = parseGuid(channel.value(QStringLiteral("profileId")).toString());
    const auto tvgId = channel.value(QStringLiteral("tvgId")).toString().trimmed().toLower();
    const auto start = QDateTime::fromString(program.value(QStringLiteral("start")).toString(), Qt::ISODateWithMs);
    m_backgroundTasks.addFuture(QtConcurrent::run(EpgService::readPool(), [this, snapshot, profileId, tvgId, start, program, completed] {
        auto resolved = program;
        QString error;
        try {
            bool found = false;
            if (snapshot->store->metadata().profileId == profileId) {
                for (const auto &entry : snapshot->store->range(tvgId, start, start.addMSecs(1))) {
                    if (entry.start != start) continue;
                    const auto stop = QDateTime::fromString(program.value(QStringLiteral("stop")).toString(), Qt::ISODateWithMs);
                    if (entry.stop != stop) continue;
                    resolved.insert(QStringLiteral("description"), entry.description);
                    resolved.insert(QStringLiteral("subTitle"), entry.subTitle);
                    resolved.insert(QStringLiteral("episodeNum"), entry.episodeNum);
                    resolved.insert(QStringLiteral("detailsPending"), false);
                    found = true; break;
                }
            }
            if (!found) error = QStringLiteral("The selected programme is no longer available.");
        } catch (const std::exception &) { error = QStringLiteral("Cannot read programme details."); }
        QMetaObject::invokeMethod(this, [this, snapshot, completed, resolved, error]() mutable {
            if (snapshot != m_epgService->snapshot()) error = QStringLiteral("The programme guide has changed. Select the programme again.");
            completed(resolved, error);
        }, Qt::QueuedConnection);
    }));
}

quint64 AppController::requestEpgDetails(const QVariantMap &channel, const QVariantMap &program)
{
    const auto id = ++m_epgDetailsRequest;
    resolveEpgDetails(channel, program, [this, id](const QVariantMap &resolved, const QString &error) {
        emit epgDetailsReady(id, resolved, error);
    });
    return id;
}

bool AppController::toggleEpgRecording(const QVariantMap &channel, const QVariantMap &program)
{
    if (!program.value(QStringLiteral("detailsPending")).toBool()) return m_dvrController->toggleProgramSchedule(channel, program);
    resolveEpgDetails(channel, program, [this, channel](const QVariantMap &resolved, const QString &error) {
        if (!error.isEmpty()) setStatusText(error);
        else m_dvrController->toggleProgramSchedule(channel, resolved);
    });
    return true;
}

QString AppController::enqueueCatchupDownload(const QVariantMap &channel, const QVariantMap &program, const QUrl &destination)
{
    if (program.value(QStringLiteral("detailsPending")).toBool()) {
        resolveEpgDetails(channel, program, [this, channel, destination](const QVariantMap &resolved, const QString &error) {
            const auto failure = error.isEmpty() ? enqueueCatchupDownload(channel, resolved, destination) : error;
            if (!failure.isEmpty()) { setStatusText(failure); emit m_downloadController->notification(failure); }
        });
        return {};
    }
    const auto state = catchupDownloadActionState(channel, program);
    if (!state.value(QStringLiteral("enabled")).toBool())
        return state.value(QStringLiteral("reason")).toString();
    const auto validation = validateCatchupRequest(m_settings, m_settings->current(), m_channelListModel, channel, program);
    const auto resolvedChannel = validation.channel;
    const auto resolvedProgram = validation.program;
    if (!validation.enabled || !resolvedChannel || !resolvedProgram)
        return QStringLiteral("The selected programme is no longer available.");
    return m_downloadController->enqueue(*resolvedChannel, *resolvedProgram, destination);
}

void AppController::resumeCatchup(const QVariantMap &channel, const QVariantMap &program)
{
    const auto state = catchupActionState(channel, program);
    const auto seconds = state.value(QStringLiteral("resumeSeconds")).toLongLong();
    playCatchupAtOffset(channel, program, seconds > 0 ? static_cast<double>(seconds) : -1.0);
}

void AppController::playCatchup(const QVariantMap &channelVariant, const QVariantMap &programVariant)
{
    playCatchupAtOffset(channelVariant, programVariant, -1.0);
}

void AppController::playCatchupAtOffset(
    const QVariantMap &channelVariant,
    const QVariantMap &programVariant,
    const double targetSeconds)
{
    if (programVariant.value(QStringLiteral("detailsPending")).toBool()) {
        const auto generation = ++m_catchupPlayGeneration;
        resolveEpgDetails(channelVariant, programVariant, [this, channelVariant, targetSeconds, generation](const QVariantMap &resolved, const QString &error) {
            if (generation != m_catchupPlayGeneration) return;
            if (!error.isEmpty()) setStatusText(error);
            else playCatchupAtOffsetInternal(channelVariant, resolved, targetSeconds);
        });
    } else playCatchupAtOffsetInternal(channelVariant, programVariant, targetSeconds);
}

void AppController::playCatchupAtOffsetInternal(
    const QVariantMap &channelVariant, const QVariantMap &programVariant,
    const double targetSeconds)
{
    DebugLogger::instance().log(
        QStringLiteral("catchup.resolve.input"),
        QStringLiteral("rawStart=%1 rawStop=%2 channelId=%3 profileId=%4")
            .arg(programVariant.value(QStringLiteral("start")).toString(),
                 programVariant.value(QStringLiteral("stop")).toString(),
                 channelVariant.value(QStringLiteral("id")).toString(),
                 channelVariant.value(QStringLiteral("profileId")).toString()));
    const auto validation = validateCatchupRequest(
        m_settings,
        m_settings->current(),
        m_channelListModel,
        channelVariant,
        programVariant);
    if (!validation.enabled || !validation.channel.has_value() || !validation.program.has_value()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("catchup.resolve.failure"),
            validation.reason.isEmpty() ? QStringLiteral("Catch-up validation failed.") : validation.reason);
        setStatusText(validation.reason.isEmpty() ? QStringLiteral("Catch-up is unavailable.") : validation.reason);
        return;
    }

    DebugLogger::instance().log(
        QStringLiteral("catchup.resolve.parsed"),
        QStringLiteral("parsedStartUtc=%1 parsedStopUtc=%2 utcEpoch=%3 lutcEpoch=%4 template=%5")
            .arg(validation.program->start.toUTC().toString(Qt::ISODateWithMs),
                 validation.program->stop.toUTC().toString(Qt::ISODateWithMs),
                 formatEpoch(validation.program->start),
                 formatEpoch(validation.program->stop),
                 redactSensitiveText(validation.channel->catchupSourceTemplate)));

    CatchupUrlResolver resolver(validation.profile);
    QString catchupResolveReason;
    const auto now = QDateTime::currentDateTimeUtc();
    const auto safetySeconds = 60 * std::clamp(validation.profile ? validation.profile->catchupSafetyMinutes : 3, 0, 30);
    const auto safeEdge = CatchupUrlResolver::availableEdge(now, safetySeconds, now);
    const auto target = resolver.resolveWindow(validation.channel.value(), validation.program->start, safeEdge, &catchupResolveReason);
    if (!target.has_value()) {
        const auto reason = catchupResolveReason.trimmed().isEmpty()
            ? QStringLiteral("Catch-up URL resolution failed.")
            : catchupResolveReason.trimmed();
        Core::DebugLogger::instance().log(
            QStringLiteral("catchup.resolve.failure"),
            QStringLiteral("%1 channel=%2").arg(reason, validation.channel->name));
        setStatusText(reason);
        return;
    }

    const bool pipActive = m_multiViewController->layoutMode() == QStringLiteral("pip");
    const bool pictureInPicture = pipActive && !m_multiViewController->focusedTileIsPrimary();
    QPointer<PlayerController> destination = m_playerController;
    if (pictureInPicture) {
        destination = m_multiViewController->prepareCatchupPictureInPicture();
        if (!destination) {
            return;
        }
    }
    const auto pipRevision = m_multiViewController->pipRevision();
    const auto resolvedCatchupUrl = target->url.trimmed();
    const auto catchupChannel = validation.channel.value();
    const auto catchupProgram = validation.program.value();
    const auto &playbackTarget = target.value();
    const auto catchupGeneration = ++m_catchupPlayGeneration;
    const auto requestedSeekSeconds = std::isfinite(targetSeconds) && targetSeconds >= 0.0 ? targetSeconds : 0.0;
    const auto programmeAvailableSeconds = std::max<qint64>(0,
        catchupProgram.start.secsTo(std::min(catchupProgram.stop, safeEdge)));
    const auto boundedTarget = std::clamp(requestedSeekSeconds, 0.0, static_cast<double>(programmeAvailableSeconds));
    // An exact edge selection still needs a non-empty transport range. Load
    // its last published second and retain the requested visual position.
    const auto requestedStart = std::max(catchupProgram.start,
        std::min(catchupProgram.start.addSecs(static_cast<qint64>(boundedTarget)), safeEdge.addSecs(-1)));
    const auto initialWindow = resolver.resolveWindow(catchupChannel, requestedStart, safeEdge, &catchupResolveReason);
    if (!initialWindow) {
        setStatusText(catchupResolveReason);
        return;
    }
    const auto initialCatchupUrl = initialWindow->url;
    const std::optional<double> initialStreamBaseOffsetSeconds =
        static_cast<double>(playbackTarget.programStartUtc.secsTo(initialWindow->programStartUtc));
    const std::optional<double> initialTimelinePositionSeconds =
        static_cast<double>(playbackTarget.programStartUtc.msecsTo(catchupProgram.start)) / 1000.0 + boundedTarget;
    const std::optional<double> initialSeekSeconds = std::nullopt;
    const bool endless = true;
    auto startCatchupPlayback = [this, destination, pictureInPicture, pipActive, pipRevision, catchupChannel, catchupProgram, playbackTarget, resolvedCatchupUrl, initialCatchupUrl, initialSeekSeconds, initialStreamBaseOffsetSeconds, initialTimelinePositionSeconds, endless](
                                    const quint64 generation,
                                    const QString &resolvedInitialUrl,
                                    const bool redirectApplied,
                                    const QString &redirectResolutionError) {
        if (generation != m_catchupPlayGeneration || !destination
            || (pictureInPicture && (m_multiViewController->layoutMode() != QStringLiteral("pip")
                || m_multiViewController->pipControllerObject() != destination))
            || (pipActive && m_multiViewController->pipRevision() != pipRevision)
            || (!pictureInPicture && destination != m_playerController)) {
            return;
        }

        Core::DebugLogger::instance().log(
            QStringLiteral("catchup.resolve.success"),
            QStringLiteral("Resolved catch-up for %1 at %2.")
                .arg(catchupChannel.name, catchupProgram.start.toUTC().toString(Qt::ISODateWithMs)));
        Core::DebugLogger::instance().log(
            QStringLiteral("catchup.play.start"),
            QStringLiteral("Starting catch-up playback for %1 (source=%2 effective=%3 redirectApplied=%4 reason=%5).")
                .arg(catchupChannel.name,
                     redactSensitiveUrl(resolvedCatchupUrl),
                     redactSensitiveUrl(resolvedInitialUrl),
                     redirectApplied ? QStringLiteral("true") : QStringLiteral("false"),
                     redirectResolutionError.trimmed().isEmpty() ? QStringLiteral("none") : redirectResolutionError.trimmed()));
        if (m_multiViewController != nullptr && m_multiViewController->layoutMode() != QStringLiteral("off")
            && m_multiViewController->layoutMode() != QStringLiteral("pip")) {
            m_multiViewController->setLayoutMode(QStringLiteral("off"));
        }
        if (m_multiViewController->layoutMode() == QStringLiteral("pip")) {
            m_multiViewController->prepareCatchupPictureInPicture();
        }
        destination->playCatchupChannel(
            catchupChannel,
            resolvedInitialUrl,
            catchupProgramLabel(catchupProgram, m_settingsController->dateTimeFormatter()->options()),
            playbackTarget.programStartUtc,
            catchupProgram.stop,
            resolvedCatchupUrl,
            initialSeekSeconds,
            initialStreamBaseOffsetSeconds,
            initialTimelinePositionSeconds,
            playbackTarget.safetySeconds,
            endless,
            catchupProgram);

        if (m_shellController->activeOverlay() == QStringLiteral("guide")) {
            m_shellController->clearOverlay();
        }
    };

    const auto shouldResolveRedirect =
        validation.profile.has_value() && validation.profile->type == ProfileType::Xtream;
    if (!shouldResolveRedirect) {
        startCatchupPlayback(catchupGeneration, initialCatchupUrl, false, QString {});
        return;
    }

    m_backgroundTasks.addFuture(QtConcurrent::run([this, catchupGeneration, initialCatchupUrl, startCatchupPlayback]() {
        bool redirectApplied = false;
        QString redirectResolutionError;
        const auto effectiveCatchupUrl = resolveCatchupRedirectIfPresent(
            initialCatchupUrl,
            1800,
            &redirectApplied,
            &redirectResolutionError);

        QMetaObject::invokeMethod(
            this,
            [startCatchupPlayback, catchupGeneration, effectiveCatchupUrl, redirectApplied, redirectResolutionError]() {
                startCatchupPlayback(
                    catchupGeneration,
                    effectiveCatchupUrl,
                    redirectApplied,
                    redirectResolutionError);
            },
            Qt::QueuedConnection);
    }));
}

void AppController::activateChannel(const int channelId)
{
    const auto channel = m_channelListModel->channelById(channelId);
    if (!channel.has_value()) {
        return;
    }

    if (m_multiViewController->isActive()) {
        if (!m_multiViewController->assignResolvedChannel(channel.value())) {
            return;
        }

        m_guideStateModel->selectChannel(channelId);
        if (m_shellController->activeOverlay() == QStringLiteral("guide")) {
            m_epgGridModel->setSelectedChannelId(channelId);
        }
        return;
    }

    activatePrimaryChannel(channel.value());
}

void AppController::activatePrimaryChannel(const Channel &channel)
{
    ++m_catchupPlayGeneration;

    const auto currentChannel = m_playerController->currentChannelValue();
    const auto sameChannelActivationSuppressed = currentChannel.has_value()
        && currentChannel->profileId == channel.profileId
        && currentChannel->id == channel.id
        && !m_playerController->inCatchupMode()
        && (m_playerController->isPlaying() || m_playerController->channelSwitchInProgress());
    if (currentChannel.has_value() && (currentChannel->profileId != channel.profileId || currentChannel->id != channel.id)) {
        m_previousPlaybackChannelId = currentChannel->id;
    }

    if (!sameChannelActivationSuppressed) {
        if (!m_dvrController->attachPlaybackForChannel(channel)) {
            m_playerController->playChannel(channel);
        }
    } else {
        Core::DebugLogger::instance().log(
            QStringLiteral("player"),
            QStringLiteral("Skipping duplicate retune for active/in-flight channel %1 (%2).")
                .arg(channel.name, redactSensitiveUrl(channel.streamUrl)));
    }
    Core::DebugLogger::instance().log(
        QStringLiteral("player"),
        QStringLiteral("Activating channel %1 (%2).").arg(channel.name, redactSensitiveUrl(channel.streamUrl)));
    m_nowNextModel->setChannel(channel);
    m_guideStateModel->selectChannel(channel.id);
    if (m_shellController->activeOverlay() == QStringLiteral("guide")) {
        m_epgGridModel->setSelectedChannelId(channel.id);
    }

    const auto profileKey = guidToString(channel.profileId);
    m_settings->current().lastWatchedChannelId[profileKey] = channel.id;
    m_settings->save();
}

void AppController::prefetchIconsAsync(const QList<Channel> &channels, quint64 importToken)
{
    m_backgroundTasks.addFuture(QtConcurrent::run([this, channels, importToken, generation = m_profileLoadGeneration]() mutable {
        for (auto channel : channels) {
            const auto cancelled = [this, profileId = channel.profileId, importToken]() {
                return m_stopping.load() || !DatabaseService::channelImportCurrent(profileId, importToken);
            };
            if (cancelled()) break;
            const auto path = m_iconCacheService.getOrDownload(channel, cancelled);
            if (path.isEmpty()) {
                continue;
            }

            if (cancelled()) break;
            try { m_database->publishChannelIcon(channel, importToken, path); }
            catch (...) {
                Core::DebugLogger::instance().log(QStringLiteral("icons"), QStringLiteral("Cannot publish cached icon."));
                continue;
            }
            QMetaObject::invokeMethod(
                this,
                [this, channel, path, importToken, generation]() {
                    if (m_stopping || generation != m_profileLoadGeneration
                        || !DatabaseService::channelImportCurrent(channel.profileId, importToken)) return;
                    const auto modelChannel = m_channelListModel->channelById(channel.id);
                    if (!modelChannel || modelChannel->profileId != channel.profileId
                        || modelChannel->iconUrl != channel.iconUrl) return;
                    m_channelListModel->setCachedIconPath(channel.id, path);
                    std::optional<Channel> refreshedChannel;
                    for (auto &loaded : m_loadedChannels) {
                        if (loaded.id == channel.id && loaded.profileId == channel.profileId
                            && loaded.iconUrl == channel.iconUrl) {
                            loaded.cachedIconPath = path;
                            refreshedChannel = loaded;
                            break;
                        }
                    }
                    const auto currentChannel = m_playerController->currentChannelValue();
                    if (refreshedChannel.has_value()
                        && currentChannel.has_value()
                        && currentChannel->profileId == refreshedChannel->profileId
                        && currentChannel->id == refreshedChannel->id) {
                        m_playerController->refreshCurrentChannelMetadata(refreshedChannel.value());
                    }
                    rebuildGuideGridAsync();
                },
                Qt::QueuedConnection);
        }
    }));
}

void AppController::loadEpgAsync(const ServerProfile &profile, const bool forceRefresh)
{
    EpgCacheService::cancel(m_epgImportCancellation);
    const auto token = m_epgImportCancellation = EpgCacheService::beginImport(profile.id);
    const auto existing = m_epgService->snapshot();
    const auto generation = ++m_epgLoadGeneration;
    const auto refreshIntervalMinutes = m_settings->current().refreshIntervalMinutes;
    const auto autoRefreshEnabled = m_settings->current().autoRefreshEpg;
    setEpgCacheBootstrapPending(!forceRefresh && (QFile::exists(EpgCacheService::manifestFile(profile.id))
        || QFile::exists(AppDataPaths::epgCacheFile(profile.id))));
    if (!m_epgRefreshInProgress) {
        m_epgRefreshInProgress = true;
        emit epgRefreshStateChanged();
    }

    m_backgroundTasks.addFuture(QtConcurrent::run(EpgService::importPool(), [this, generation, profile, refreshIntervalMinutes, autoRefreshEnabled, forceRefresh, token, existing]() {
        const auto cancellationCompletion = qScopeGuard([this, generation, profile, token] {
            if (!token->load()) return;
            QMetaObject::invokeMethod(this, [this, generation, profile] {
                if (generation != m_epgLoadGeneration) return;
                const auto current = m_settings->activeProfile();
                if (current && current->id == profile.id) loadEpgAsync(*current);
                else { m_epgRefreshInProgress = false; setEpgCacheBootstrapPending(false); emit epgRefreshStateChanged(); }
            }, Qt::QueuedConnection);
        });
        if (token->load()) return;
        const auto sourceFingerprint = EpgCacheService::sourceFingerprint(profile);
        const auto now = QDateTime::currentDateTimeUtc();
        EpgCacheService::LoadResult cache;
        if (existing->store && existing->store->metadata().profileId == profile.id
            && existing->store->metadata().fingerprint == sourceFingerprint) {
            cache.status = EpgCacheService::LoadStatus::Loaded;
            cache.data = {profile.id, sourceFingerprint, existing->store->metadata().fetchedAt, *existing};
        } else cache = m_epgCacheService.load(profile.id, token);
        if (token->load()) return;

        auto applySnapshot = [this, generation, profileId = profile.id](
                                 EpgService::Snapshot snapshot,
                                 const QDateTime &fetchedAt,
                                 const QString &errorText,
                                 const bool scheduleRefresh,
                                 const bool setRefreshError,
                                 const bool finishRefreshState,
                                 const bool cacheBootstrapPending) {
            auto snapshotPtr = std::make_shared<const EpgService::Snapshot>(std::move(snapshot));
            QMetaObject::invokeMethod(
                this,
                [this, generation, profileId, snapshotPtr, fetchedAt, errorText, scheduleRefresh, setRefreshError, finishRefreshState, cacheBootstrapPending]() {
                    applyEpgSnapshot(
                        generation,
                        profileId,
                        snapshotPtr,
                        fetchedAt,
                        errorText,
                        scheduleRefresh,
                        setRefreshError,
                        finishRefreshState,
                        cacheBootstrapPending);
                },
                Qt::QueuedConnection);
        };

        auto clearLoadedEpg = [this, generation, profileId = profile.id](const QString &errorText) {
            QMetaObject::invokeMethod(
                this,
                [this, generation, profileId, errorText]() {
                    if (generation != m_epgLoadGeneration) {
                        return;
                    }

                    clearEpg(profileId, errorText);
                },
                Qt::QueuedConnection);
        };

        auto scheduleRetry = [this, generation, profileId = profile.id, refreshIntervalMinutes, autoRefreshEnabled](const QString &errorText) {
            QMetaObject::invokeMethod(
                this,
                [this, generation, profileId, refreshIntervalMinutes, autoRefreshEnabled, errorText]() {
                    if (generation != m_epgLoadGeneration) {
                        return;
                    }

                    m_epgLoadedProfileId = profileId;
                    m_epgLastRefreshError = errorText;
                    m_epgNextRefreshAt = autoRefreshEnabled && refreshIntervalMinutes > 0
                        ? QDateTime::currentDateTimeUtc().addSecs(static_cast<qint64>(refreshIntervalMinutes) * 60)
                        : QDateTime {};
                    if (m_manualEpgRefreshPending) {
                        setStatusText(QStringLiteral("EPG refresh failed: %1").arg(errorText));
                        m_manualEpgRefreshPending = false;
                    }
                    if (m_epgRefreshInProgress) {
                        m_epgRefreshInProgress = false;
                    }
                    emit epgRefreshStateChanged();
                    updateRefreshTimer();
                },
                Qt::QueuedConnection);
        };

        auto fetchFresh = [this, &profile, &sourceFingerprint, token]() -> EpgCacheService::CacheData {
            QStringList urls;
            if (profile.type == ProfileType::Xtream && profile.xmltvUrl.trimmed().isEmpty()) {
                XtreamService xtream(m_network);
                xtream.setProfile(profile);
                urls = {xtream.xmltvUrl().toString()};
            } else urls = profile.xmltvUrl.trimmed().isEmpty()
                ? profile.discoveredXmltvUrls : QStringList {profile.xmltvUrl.trimmed()};
            const auto cancelled = [token] { return token->load(); };
            QElapsedTimer elapsed; elapsed.start();
            auto data = m_epgCacheService.build(profile.id, sourceFingerprint, [&](const EpgStore::Sink &sink) {
                for (const auto &source : urls) {
                    if (cancelled()) throw std::runtime_error("EPG import cancelled.");
                    const QUrl url(source);
                    if (url.isLocalFile()) {
                        QFile file(url.toLocalFile());
                        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read the local XMLTV file.");
                        EpgService::streamEntries(&file, sink, cancelled);
                    } else {
                        QTemporaryFile file(QDir(AppDataPaths::epgCacheDirectory()).filePath(QStringLiteral("download-XXXXXX.source")));
                        if (!file.open()) throw std::runtime_error("Cannot create XMLTV download file.");
                        m_network->download(url, &file, cancelled);
                        if (!file.flush() || !file.seek(0)) throw std::runtime_error("Cannot read XMLTV download.");
                        EpgService::streamEntries(&file, sink, cancelled);
                    }
                }
            }, urls.size() > 1, token);
            Core::DebugLogger::instance().log(QStringLiteral("epg"),
                QStringLiteral("Imported %1 programmes in %2 ms; database %3 bytes.")
                    .arg(data.snapshot.totalEntries).arg(elapsed.elapsed()).arg(QFileInfo(data.snapshot.store->path()).size()));
            return data;
        };

        const auto cacheUsable = cache.status == EpgCacheService::LoadStatus::Loaded
            && EpgCacheService::matchesProfile(cache.data, profile);
        const auto cacheStale = cacheUsable
            && EpgCacheService::isStale(cache.data.fetchedAt, refreshIntervalMinutes, now);

        if (cacheUsable && (!forceRefresh || existing->store != cache.data.snapshot.store)) {
            applySnapshot(
                cache.data.snapshot,
                cache.data.fetchedAt,
                QString {},
                !cacheStale && !forceRefresh,
                !cacheStale && !forceRefresh,
                !cacheStale && !forceRefresh,
                !forceRefresh);
            if (!cacheStale && !forceRefresh) {
                return;
            }
        }

        if (sourceFingerprint.isEmpty()) {
            if (!cacheUsable) {
                clearLoadedEpg(QString {});
            } else {
                QMetaObject::invokeMethod(this, [this]() { setEpgCacheBootstrapPending(false); }, Qt::QueuedConnection);
            }
            return;
        }

        try {
            auto fresh = fetchFresh();
            m_epgCacheService.save(fresh, token);
            applySnapshot(std::move(fresh.snapshot), fresh.fetchedAt, QString {}, true, true, true, false);
        } catch (const std::exception &error) {
            if (token->load()) return;
            const auto errorText = QString::fromUtf8(error.what());
            if (!cacheUsable) {
                clearLoadedEpg(errorText);
            } else {
                scheduleRetry(errorText);
            }
        }
    }));
}

QList<ChannelCategory> AppController::buildM3uCategories(const QList<Channel> &channels)
{
    QMap<QString, QString> categories;
    for (const auto &channel : channels) {
        const auto categoryId = normalizeChannelCategoryId(channel.categoryId);
        categories.insert(categoryId, displayNameForCategoryId(categoryId));
    }

    QList<ChannelCategory> result;
    for (auto it = categories.cbegin(); it != categories.cend(); ++it) {
        result.push_back(ChannelCategory { it.key(), it.value(), 0 });
    }
    return result;
}

} // namespace OKILTV::App
