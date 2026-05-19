#include "appcontroller.h"

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
#include <QGuiApplication>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QQuickWindow>
#include <QSet>
#include <QSysInfo>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace OKILTV::App {

using namespace Core;

namespace {

constexpr auto kFavouritesCategoryId = "__favourites__";
constexpr int kNewSourceAutoSelectThreshold = 20;
constexpr int kWatchStatsFlushIntervalMs = 60 * 1000;

struct LoadProfileResult
{
    bool ok { false };
    bool sourceRefreshSucceeded { false };
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

QString catchupProgramLabel(const EpgEntry &program)
{
    const auto title = program.title.trimmed();
    const auto timeRange = epgEntryTimeRange(program);
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

    for (const auto &profile : settings.profiles) {
        if (profile.id == resolvedChannel->profileId) {
            validation.profile = profile;
            break;
        }
    }

    validation.enabled = true;
    return validation;
}

} // namespace

AppController::~AppController()
{
    flushTrackedWatchSeconds();
    m_backgroundTasks.waitForFinished();
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
    connect(m_guideStateModel, &GuideStateModel::selectedGroupIdChanged, this, [this]() {
        rebuildGuideGridAsync();
    });
    connect(m_settingsController, &SettingsController::saved, this, [this]() {
        updateRefreshTimer();
        m_guideStateModel->setPreviewEnabled(m_settings->current().guidePreviewEnabled);
        rebuildGuideGridAsync();
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
    connect(m_playerController, &PlayerController::playbackError, this, [this](const QString &message) {
        Core::DebugLogger::instance().log(QStringLiteral("app"), QStringLiteral("Playback error: %1").arg(message));
        if (m_playerController->inCatchupMode()) {
            Core::DebugLogger::instance().log(
                QStringLiteral("catchup.play.error"),
                QStringLiteral("Catch-up playback failed: %1").arg(message));
        }
        setStatusText(message);
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
    connect(m_playerController, &PlayerController::playbackChannelActivated, this, [this](const int channelId) {
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
    connect(m_playerController, &PlayerController::isPlayingChanged, this, [this]() {
        if (!m_playerController->isPlaying()) {
            flushTrackedWatchSeconds();
            m_watchTrackingActive = false;
            m_watchStatsFlushTimer.stop();
            return;
        }

        beginWatchTrackingForCurrentChannel();
    });
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

    return m_epgFetchedAt.toLocalTime().toString(QStringLiteral("dd-MM-yyyy HH:mm"));
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
        m_settings->current().remuxRecordingsToMkv);

    auto preferencesChanged = false;
    for (const auto &profile : m_settings->current().profiles) {
        preferencesChanged = syncProfileGroupPreferences(profile.id, m_database->loadChannels(profile.id))
            || preferencesChanged;
    }
    if (preferencesChanged) {
        m_settings->save();
        m_profilesModel->reload();
    }

    const auto profile = m_settings->activeProfile();
    if (profile.has_value()) {
        loadProfile(guidToString(profile->id));
    }
}

void AppController::loadProfile(const QString &profileId)
{
    flushTrackedWatchSeconds();

    const auto parsedId = parseGuid(profileId);
    const auto profile = m_settings->profileById(parsedId);
    if (!profile.has_value()) {
        setStatusText(QStringLiteral("Profile not found."));
        return;
    }

    const auto currentChannel = m_playerController->currentChannelValue();
    const auto crossProfileActivationFromCatchup = currentChannel.has_value()
        && currentChannel->profileId != profile->id
        && m_playerController->inCatchupMode()
        && m_settings->current().activeProfileId.has_value()
        && m_settings->current().activeProfileId.value() == profile->id;
    if (crossProfileActivationFromCatchup) {
        ++m_catchupPlayGeneration;
        Core::DebugLogger::instance().log(
            QStringLiteral("catchup.play.stop"),
            QStringLiteral("Stopping catch-up before activating source %1 (%2).")
                .arg(profile->name, guidToString(profile->id)));
        setStatusText(QStringLiteral("Stopping catch-up before switching to %1...").arg(profile->name));
        m_playerController->stop();
    }

    const auto settingsSnapshot = m_settings->current();
    const auto generation = ++m_profileLoadGeneration;

    setBusy(true);
    setStatusText(QStringLiteral("Loading %1...").arg(profile->name));
    Core::DebugLogger::instance().log(
        QStringLiteral("profile"),
        QStringLiteral("Loading profile %1 (%2).").arg(profile->name, guidToString(profile->id)));

    m_backgroundTasks.addFuture(QtConcurrent::run([this, generation, profile = profile.value(), settingsSnapshot]() {
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
                result.channels = m3u.loadFromUrl(QUrl(profile.m3uUrl), profile.id);
                result.categories = buildM3uCategories(result.channels);
            } else if (profile.type == ProfileType::M3UFile) {
                M3UService m3u(m_network);
                result.channels = m3u.loadFromFile(profile.m3uFilePath, profile.id);
                result.categories = buildM3uCategories(result.channels);
            }

            m_database->replaceChannelsForProfile(profile.id, result.channels);
            result.watchSecondsByChannelId = m_database->loadWatchSecondsByProfile(profile.id);
            result.profile.lastRefreshed = QDateTime::currentDateTimeUtc();
            result.sourceRefreshSucceeded = true;
            result.ok = true;
            result.statusText = QStringLiteral("%1 channels loaded").arg(result.channels.size());
        } catch (const std::exception &error) {
            result.channels = m_database->loadChannels(profile.id);
            if (!result.channels.isEmpty()) {
                result.categories = buildM3uCategories(result.channels);
                result.watchSecondsByChannelId = m_database->loadWatchSecondsByProfile(profile.id);
                result.ok = true;
                result.statusText =
                    QStringLiteral("Using cached channels after refresh failure: %1").arg(QString::fromUtf8(error.what()));
            } else {
                result.errorText = QString::fromUtf8(error.what());
            }
        }

        const auto key = guidToString(profile.id);
        if (settingsSnapshot.lastWatchedChannelId.contains(key)) {
            result.lastWatchedChannelId = settingsSnapshot.lastWatchedChannelId.value(key);
        }

        QMetaObject::invokeMethod(
            this,
            [this, generation, result]() {
                if (generation != m_profileLoadGeneration) {
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
                const auto activeProfileBeforeSave = activeProfileId();
                for (auto &profile : m_settings->current().profiles) {
                    if (profile.id == result.profile.id) {
                        if (result.sourceRefreshSucceeded) {
                            profile.lastRefreshed = result.profile.lastRefreshed;
                        }
                        if (!result.profile.xtreamServerTimezone.trimmed().isEmpty()) {
                            profile.xtreamServerTimezone = result.profile.xtreamServerTimezone.trimmed();
                        }
                    }
                }

                if (!m_settings->current().activeProfileId.has_value()
                    && m_settings->current().profiles.size() == 1) {
                    m_settings->setActiveProfileId(result.profile.id);
                }

                m_settings->save();
                const auto activeProfileAfterSave = activeProfileId();
                if (activeProfileBeforeSave != activeProfileAfterSave) {
                    emit activeProfileIdChanged();
                }
                m_profilesModel->reload();
                m_settingsController->reload();
                if (m_shellController->activeOverlay() != QStringLiteral("settings")) {
                    m_shellController->restoreLastView();
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
                        activateChannel(result.lastWatchedChannelId.value());
                    }

                    prefetchIconsAsync(result.channels);
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
                 .arg(profile.has_value() ? Core::AppDataPaths::epgCacheFile(profile->id) : QStringLiteral("<none>"));
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
    dueProfileIds.reserve(m_settings->current().profiles.size());

    for (const auto &profile : m_settings->current().profiles) {
        if (profile.type == ProfileType::M3UFile) {
            continue;
        }

        const auto intervalHours = std::max(0, profile.autoRefreshIntervalHours);
        if (intervalHours == 0) {
            continue;
        }

        if (!profile.lastRefreshed.isValid()) {
            dueProfileIds.push_back(guidToString(profile.id));
            continue;
        }

        const auto elapsedSeconds = profile.lastRefreshed.toUTC().secsTo(nowUtc);
        if (elapsedSeconds < 0) {
            continue;
        }

        const auto intervalSeconds = static_cast<qint64>(intervalHours) * 60 * 60;
        if (elapsedSeconds >= intervalSeconds) {
            dueProfileIds.push_back(guidToString(profile.id));
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

    m_epgService->applySnapshot(snapshot);
    m_epgLoadedProfileId = profileId;
    m_epgFetchedAt = fetchedAt;
    m_epgNextRefreshAt = scheduleRefresh
        ? EpgCacheService::nextRefreshAt(fetchedAt, m_settings->current().refreshIntervalMinutes)
        : QDateTime {};
    if (setRefreshError) {
        m_epgLastRefreshError = errorText;
    }
    if (m_manualEpgRefreshPending) {
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

    m_backgroundTasks.addFuture(QtConcurrent::run([this, generation, channels]() {
        QHash<int, QVariantMap> infoByChannelId;
        for (const auto &channel : channels) {
            if (channel.tvgId.trimmed().isEmpty()) {
                continue;
            }

            const auto currentProgram = m_epgService->currentProgram(channel.tvgId);
            if (!currentProgram.has_value()) {
                continue;
            }

            infoByChannelId.insert(
                channel.id,
                QVariantMap {
                    { QStringLiteral("title"), currentProgram->title },
                    { QStringLiteral("timeRange"), epgEntryTimeRange(currentProgram.value()) }
                });
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
    const auto groupId = m_guideStateModel->selectedGroupId().trimmed();
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
    const auto profileKey = guidToString(profileId);
    auto &hiddenGroups = m_settings->current().hiddenGroupsByProfile[profileKey];
    auto &groupOrder = m_settings->current().groupOrderByProfile[profileKey];
    const auto favouritesGroupId = QString::fromUtf8(kFavouritesCategoryId);
    QStringList discoveredGroups;
    QSet<QString> seenGroups;
    discoveredGroups.reserve(channels.size());
    auto settingsChanged = false;

    for (const auto &channel : channels) {
        const auto groupId = normalizeChannelCategoryId(channel.categoryId);
        if (!seenGroups.contains(groupId)) {
            seenGroups.insert(groupId);
            discoveredGroups.push_back(groupId);
        }
    }

    if (!channels.isEmpty() && !discoveredGroups.contains(favouritesGroupId)) {
        discoveredGroups.prepend(favouritesGroupId);
    }

    const auto hasExistingPreferences = !hiddenGroups.isEmpty() || !groupOrder.isEmpty();
    if (!hasExistingPreferences && !discoveredGroups.isEmpty()) {
        groupOrder = discoveredGroups;
        hiddenGroups = discoveredGroups.size() > kNewSourceAutoSelectThreshold ? discoveredGroups : QStringList {};
        hiddenGroups.removeAll(favouritesGroupId);
        return true;
    }

    for (const auto &groupId : discoveredGroups) {
        const auto knownGroup = hiddenGroups.contains(groupId) || groupOrder.contains(groupId);
        if (!knownGroup) {
            if (groupId != favouritesGroupId) {
                hiddenGroups.push_back(groupId);
            }
            settingsChanged = true;
        }
        if (!groupOrder.contains(groupId) && groupId != favouritesGroupId) {
            groupOrder.push_back(groupId);
            settingsChanged = true;
        }
    }

    if (!channels.isEmpty() && !groupOrder.contains(favouritesGroupId)) {
        groupOrder.prepend(favouritesGroupId);
        settingsChanged = true;
    }

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
    if (!m_watchTrackingActive
        || m_watchTrackingChannelId < 0
        || m_watchTrackingProfileId.isNull()
        || !m_watchTrackingElapsed.isValid()) {
        return;
    }

    const auto elapsedSeconds = std::max<qint64>(0, m_watchTrackingElapsed.elapsed() / 1000);
    if (elapsedSeconds > 0) {
        m_database->incrementWatchSeconds(
            m_watchTrackingProfileId,
            m_watchTrackingChannelId,
            elapsedSeconds);
        m_watchSecondsByChannelId[m_watchTrackingChannelId] =
            m_watchSecondsByChannelId.value(m_watchTrackingChannelId, 0) + elapsedSeconds;
        m_channelListModel->setWatchSeconds(m_watchSecondsByChannelId);
        if (m_guideStateModel->selectedGroupId() == QString::fromUtf8(kFavouritesCategoryId)) {
            rebuildGuideGridAsync();
        }
    }

    if (m_playerController->isPlaying()) {
        m_watchTrackingElapsed.restart();
        return;
    }

    m_watchTrackingActive = false;
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

QVariantMap AppController::catchupActionState(const QVariantMap &channel, const QVariantMap &program) const
{
    const auto validation = validateCatchupRequest(m_settings->current(), m_channelListModel, channel, program);
    return {
        { QStringLiteral("visible"), validation.visible },
        { QStringLiteral("enabled"), validation.enabled },
        { QStringLiteral("reason"), validation.reason }
    };
}

void AppController::playCatchup(const QVariantMap &channelVariant, const QVariantMap &programVariant)
{
    DebugLogger::instance().log(
        QStringLiteral("catchup.resolve.input"),
        QStringLiteral("rawStart=%1 rawStop=%2 channelId=%3 profileId=%4")
            .arg(programVariant.value(QStringLiteral("start")).toString(),
                 programVariant.value(QStringLiteral("stop")).toString(),
                 channelVariant.value(QStringLiteral("id")).toString(),
                 channelVariant.value(QStringLiteral("profileId")).toString()));
    const auto validation = validateCatchupRequest(
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
    const auto target = resolver.resolve(validation.channel.value(), validation.program.value(), &catchupResolveReason);
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

    const auto resolvedCatchupUrl = target->url.trimmed();
    const auto catchupChannel = validation.channel.value();
    const auto catchupProgram = validation.program.value();
    const auto playbackTarget = target.value();
    const auto catchupGeneration = ++m_catchupPlayGeneration;
    auto startCatchupPlayback = [this, catchupChannel, catchupProgram, playbackTarget, resolvedCatchupUrl](
                                    const quint64 generation,
                                    const QString &effectiveCatchupUrl,
                                    const bool redirectApplied,
                                    const QString &redirectResolutionError) {
        if (generation != m_catchupPlayGeneration) {
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
                     redactSensitiveUrl(effectiveCatchupUrl),
                     redirectApplied ? QStringLiteral("true") : QStringLiteral("false"),
                     redirectResolutionError.trimmed().isEmpty() ? QStringLiteral("none") : redirectResolutionError.trimmed()));
        m_playerController->playCatchupChannel(
            catchupChannel,
            effectiveCatchupUrl,
            catchupProgramLabel(catchupProgram),
            playbackTarget.programStartUtc,
            playbackTarget.programStopUtc,
            resolvedCatchupUrl);

        if (m_shellController->activeOverlay() == QStringLiteral("guide")) {
            m_shellController->clearOverlay();
        }
    };

    const auto shouldResolveRedirect =
        validation.profile.has_value() && validation.profile->type == ProfileType::Xtream;
    if (!shouldResolveRedirect) {
        startCatchupPlayback(catchupGeneration, resolvedCatchupUrl, false, QString {});
        return;
    }

    m_backgroundTasks.addFuture(QtConcurrent::run([this, catchupGeneration, resolvedCatchupUrl, startCatchupPlayback]() {
        bool redirectApplied = false;
        QString redirectResolutionError;
        const auto effectiveCatchupUrl = resolveCatchupRedirectIfPresent(
            resolvedCatchupUrl,
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

void AppController::prefetchIconsAsync(const QList<Channel> &channels)
{
    m_backgroundTasks.addFuture(QtConcurrent::run([this, channels]() mutable {
        for (auto channel : channels) {
            const auto path = m_iconCacheService.getOrDownload(channel);
            if (path.isEmpty()) {
                continue;
            }

            QMetaObject::invokeMethod(
                this,
                [this, channelId = channel.id, path]() {
                    m_channelListModel->setCachedIconPath(channelId, path);
                    std::optional<Channel> refreshedChannel;
                    for (auto &loaded : m_loadedChannels) {
                        if (loaded.id == channelId) {
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
    const auto generation = ++m_epgLoadGeneration;
    const auto refreshIntervalMinutes = m_settings->current().refreshIntervalMinutes;
    const auto autoRefreshEnabled = m_settings->current().autoRefreshEpg;
    setEpgCacheBootstrapPending(!forceRefresh && QFile::exists(AppDataPaths::epgCacheFile(profile.id)));
    if (!m_epgRefreshInProgress) {
        m_epgRefreshInProgress = true;
        emit epgRefreshStateChanged();
    }

    m_backgroundTasks.addFuture(QtConcurrent::run([this, generation, profile, refreshIntervalMinutes, autoRefreshEnabled, forceRefresh]() {
        const auto sourceFingerprint = EpgCacheService::sourceFingerprint(profile);
        const auto now = QDateTime::currentDateTimeUtc();
        const auto cache = m_epgCacheService.load(profile.id);

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

        auto fetchFresh = [this, &profile, &sourceFingerprint]() -> EpgCacheService::CacheData {
            QByteArray xmltvPayload;
            if (profile.type == ProfileType::Xtream) {
                XtreamService xtream(m_network);
                xtream.setProfile(profile);
                xmltvPayload = xtream.getXmltvBytes();
            } else if (!profile.xmltvUrl.trimmed().isEmpty()) {
                xmltvPayload = m_network->get(QUrl(profile.xmltvUrl));
            }

            const auto entries = EpgService::parseEntries(xmltvPayload);

            EpgCacheService::CacheData data;
            data.profileId = profile.id;
            data.sourceFingerprint = sourceFingerprint;
            data.fetchedAt = QDateTime::currentDateTimeUtc();
            data.snapshot = EpgService::buildSnapshot(entries);
            return data;
        };

        const auto cacheUsable = cache.status == EpgCacheService::LoadStatus::Loaded
            && EpgCacheService::matchesProfile(cache.data, profile);
        const auto cacheStale = cacheUsable
            && EpgCacheService::isStale(cache.data.fetchedAt, refreshIntervalMinutes, now);

        if (!forceRefresh && cacheUsable) {
            applySnapshot(
                cache.data.snapshot,
                cache.data.fetchedAt,
                QString {},
                !cacheStale,
                !cacheStale,
                !cacheStale,
                true);
            if (!cacheStale) {
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
            m_epgCacheService.save(fresh);
            applySnapshot(std::move(fresh.snapshot), fresh.fetchedAt, QString {}, true, true, true, false);
        } catch (const std::exception &error) {
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
