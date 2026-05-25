#include "appcontroller.h"
#include "channellistmodel.h"
#include "displaysleepblocker.h"
#include "dvrcontroller.h"
#include "epggridmodel.h"
#include "guidestatemodel.h"
#include "multiviewcontroller.h"
#include "nownextmodel.h"
#include "playercontroller.h"
#include "portableruntimecontroller.h"
#include "profilesmodel.h"
#include "settingscontroller.h"
#include "shellcontroller.h"
#include "sourcegroupsmodel.h"
#include "traycontroller.h"
#include "timeshiftcontroller.h"
#include "uitestbridge.h"
#include "uitestcapturecontroller.h"

#include "../core/appdatapaths.h"
#include "../core/database_service.h"
#include "../core/debuglogger.h"
#include "../core/epgservice.h"
#include "../core/networkaccess.h"
#include "../core/portablebootstrap.h"
#include "../core/settingsmanager.h"

#include <QApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSqlDatabase>
#include <QSurfaceFormat>

#include <clocale>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#if defined(Q_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

struct StartupOptions
{
    OKILTV::Core::RuntimeContext runtimeContext;
    std::vector<QByteArray> filteredArguments;
};

using StartupLogFn = std::function<void(const QString &)>;

#if defined(Q_OS_WINDOWS)
void enableNativeSnapSupportForFramelessWindow(QQuickWindow *window)
{
    if (window == nullptr) {
        return;
    }

    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    if (hwnd == nullptr) {
        return;
    }

    SetLastError(0);
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (style == 0 && GetLastError() != 0) {
        OKILTV::Core::DebugLogger::instance().log(
            QStringLiteral("startup"),
            QStringLiteral("Failed to query Win32 window style; native snap support not adjusted."));
        return;
    }

    constexpr LONG_PTR requiredStyleBits = WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU;
    const auto updatedStyle = style | requiredStyleBits;
    if (updatedStyle != style) {
        SetLastError(0);
        if (SetWindowLongPtrW(hwnd, GWL_STYLE, updatedStyle) == 0 && GetLastError() != 0) {
            OKILTV::Core::DebugLogger::instance().log(
                QStringLiteral("startup"),
                QStringLiteral("Failed to update Win32 window style for native snap support."));
            return;
        }
    }

    SetWindowPos(
        hwnd,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
#endif

void updateDisplaySleepBlockerForWindowState(
    OKILTV::App::DisplaySleepBlocker *blocker,
    const OKILTV::App::SettingsController *settingsController,
    const OKILTV::App::PlayerController *playerController,
    const QQuickWindow *window)
{
    if (blocker == nullptr) {
        return;
    }

    if (window == nullptr || settingsController == nullptr || playerController == nullptr) {
        blocker->setBlocked(false);
        return;
    }

    const auto visibility = window->visibility();
    const auto windowAllowsBlocking = visibility != QWindow::Hidden
        && visibility != QWindow::Minimized;
    const auto shouldBlock = settingsController->preventDisplaySleep()
        && playerController->isPlaying()
        && windowAllowsBlocking;
    blocker->setBlocked(shouldBlock);
}

StartupOptions parseStartupOptions(const int argc, char *argv[])
{
    StartupOptions options;
    options.filteredArguments.reserve(static_cast<std::size_t>(argc));

    if (argc > 0) {
        options.filteredArguments.emplace_back(argv[0]);
    }

    for (int index = 1; index < argc; ++index) {
        const auto argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QStringLiteral("--portable-bootstrap") && index + 1 < argc) {
            options.runtimeContext.launchMode = OKILTV::Core::LaunchMode::Portable;
            options.runtimeContext.portableBootstrapPath = QString::fromLocal8Bit(argv[index + 1]);
            options.runtimeContext.dataRootOverride =
                OKILTV::Core::PortableBootstrap::load(options.runtimeContext.portableBootstrapPath).dataRootOverride;
            ++index;
            continue;
        }

        options.filteredArguments.push_back(argument.toLocal8Bit());
    }

    return options;
}

template <typename T, typename Factory>
std::unique_ptr<T> constructComponent(const StartupLogFn &startupStep, QStringView name, Factory &&factory)
{
    startupStep(QStringLiteral("Constructing %1.").arg(name));
    auto instance = factory();
    startupStep(QStringLiteral("%1 ready.").arg(name));
    return instance;
}

void registerMetaTypes(const StartupLogFn &startupStep)
{
    startupStep(QStringLiteral("Registering meta types."));
    qRegisterMetaType<OKILTV::Core::Channel>();
    qRegisterMetaType<OKILTV::Core::ServerProfile>();
    startupStep(QStringLiteral("Meta types registered."));
}

struct CoreServices
{
    std::unique_ptr<OKILTV::Core::SettingsManager> settings;
    std::unique_ptr<OKILTV::Core::DatabaseService> database;
    std::shared_ptr<OKILTV::Core::NetworkAccess> network;
    std::unique_ptr<OKILTV::Core::EpgService> epgService;
};

CoreServices constructCoreServices(const StartupLogFn &startupStep)
{
    startupStep(QStringLiteral("Constructing core services."));
    CoreServices services;

    services.settings = constructComponent<OKILTV::Core::SettingsManager>(startupStep, QStringLiteral("SettingsManager"), []() {
        return std::make_unique<OKILTV::Core::SettingsManager>();
    });
    startupStep(QStringLiteral("Loading settings from %1").arg(services.settings->settingsFilePath()));
    services.settings->load();
    startupStep(QStringLiteral("Settings loaded. Profiles=%1").arg(services.settings->sourceSummaries().size()));

    startupStep(QStringLiteral("Available SQL drivers: %1").arg(QSqlDatabase::drivers().join(QStringLiteral(", "))));
    services.database = constructComponent<OKILTV::Core::DatabaseService>(startupStep, QStringLiteral("DatabaseService"), []() {
        return std::make_unique<OKILTV::Core::DatabaseService>();
    });
    startupStep(QStringLiteral("DatabaseService ready. Database=%1").arg(services.database->databaseFilePath()));

    startupStep(QStringLiteral("Creating network access layer."));
    services.network = OKILTV::Core::makeDefaultNetworkAccess();
    startupStep(QStringLiteral("Network access layer ready."));

    services.epgService = constructComponent<OKILTV::Core::EpgService>(startupStep, QStringLiteral("EpgService"), []() {
        return std::make_unique<OKILTV::Core::EpgService>();
    });

    startupStep(QStringLiteral("Core services ready."));
    return services;
}

struct AppServices
{
    std::unique_ptr<OKILTV::App::ProfilesModel> profilesModel;
    std::unique_ptr<OKILTV::App::ChannelListModel> channelListModel;
    std::unique_ptr<OKILTV::App::NowNextModel> nowNextModel;
    std::unique_ptr<OKILTV::App::NowNextModel> playbackNowNextModel;
    std::unique_ptr<OKILTV::App::EpgGridModel> epgGridModel;
    std::unique_ptr<OKILTV::App::GuideStateModel> guideStateModel;
    std::unique_ptr<OKILTV::App::SourceGroupsModel> settingsSourceGroupsModel;
    std::unique_ptr<OKILTV::App::SourceGroupsModel> liveSourceGroupsModel;
    std::unique_ptr<OKILTV::App::ShellController> shellController;
    std::unique_ptr<OKILTV::App::PlayerController> playerController;
    std::unique_ptr<OKILTV::App::MultiViewController> multiViewController;
    std::unique_ptr<OKILTV::App::DvrController> dvrController;
    std::unique_ptr<OKILTV::App::TimeshiftController> timeshiftController;
    std::unique_ptr<OKILTV::App::TrayController> trayController;
    std::unique_ptr<OKILTV::App::DisplaySleepBlocker> displaySleepBlocker;
    std::unique_ptr<OKILTV::App::SettingsController> settingsController;
    std::unique_ptr<OKILTV::App::PortableRuntimeController> portableRuntimeController;
    std::unique_ptr<OKILTV::App::UiTestCaptureController> uiTestCaptureController;
    std::unique_ptr<OKILTV::App::UiTestBridge> uiTestBridge;
};

AppServices constructAppServices(
    const StartupLogFn &startupStep,
    OKILTV::Core::SettingsManager *settings,
    OKILTV::Core::DatabaseService *database,
    OKILTV::Core::EpgService *epgService)
{
    startupStep(QStringLiteral("Constructing app services."));
    AppServices services;

    services.profilesModel = constructComponent<OKILTV::App::ProfilesModel>(startupStep, QStringLiteral("ProfilesModel"), [settings]() {
        return std::make_unique<OKILTV::App::ProfilesModel>(settings);
    });
    services.channelListModel = constructComponent<OKILTV::App::ChannelListModel>(startupStep, QStringLiteral("ChannelListModel"), [settings]() {
        return std::make_unique<OKILTV::App::ChannelListModel>(settings);
    });
    services.nowNextModel = constructComponent<OKILTV::App::NowNextModel>(startupStep, QStringLiteral("NowNextModel"), [epgService]() {
        return std::make_unique<OKILTV::App::NowNextModel>(epgService);
    });
    services.playbackNowNextModel = constructComponent<OKILTV::App::NowNextModel>(
        startupStep,
        QStringLiteral("PlaybackNowNextModel"),
        [epgService]() {
            return std::make_unique<OKILTV::App::NowNextModel>(epgService);
        });
    services.epgGridModel = constructComponent<OKILTV::App::EpgGridModel>(startupStep, QStringLiteral("EpgGridModel"), [epgService]() {
        return std::make_unique<OKILTV::App::EpgGridModel>(epgService);
    });
    services.guideStateModel = constructComponent<OKILTV::App::GuideStateModel>(
        startupStep,
        QStringLiteral("GuideStateModel"),
        [epgService, settings]() {
            return std::make_unique<OKILTV::App::GuideStateModel>(epgService, settings);
        });
    services.settingsSourceGroupsModel = constructComponent<OKILTV::App::SourceGroupsModel>(
        startupStep,
        QStringLiteral("SettingsSourceGroupsModel"),
        [settings, database]() {
            return std::make_unique<OKILTV::App::SourceGroupsModel>(settings, database);
        });
    services.liveSourceGroupsModel = constructComponent<OKILTV::App::SourceGroupsModel>(
        startupStep,
        QStringLiteral("LiveSourceGroupsModel"),
        [settings, database]() {
            return std::make_unique<OKILTV::App::SourceGroupsModel>(settings, database);
        });
    services.shellController = constructComponent<OKILTV::App::ShellController>(startupStep, QStringLiteral("ShellController"), [settings]() {
        return std::make_unique<OKILTV::App::ShellController>(settings);
    });
    services.playerController = constructComponent<OKILTV::App::PlayerController>(startupStep, QStringLiteral("PlayerController"), []() {
        return std::make_unique<OKILTV::App::PlayerController>();
    });
    services.multiViewController = constructComponent<OKILTV::App::MultiViewController>(
        startupStep,
        QStringLiteral("MultiViewController"),
        [settings, &services]() {
            return std::make_unique<OKILTV::App::MultiViewController>(
                settings,
                services.channelListModel.get(),
                services.playerController.get());
        });
    services.dvrController = constructComponent<OKILTV::App::DvrController>(
        startupStep,
        QStringLiteral("DvrController"),
        [settings, &services]() {
            return std::make_unique<OKILTV::App::DvrController>(settings, services.playerController.get());
        });
    services.timeshiftController = constructComponent<OKILTV::App::TimeshiftController>(
        startupStep,
        QStringLiteral("TimeshiftController"),
        [settings, &services]() {
            return std::make_unique<OKILTV::App::TimeshiftController>(
                settings,
                services.playerController.get(),
                services.dvrController.get(),
                services.multiViewController.get());
        });
    services.playerController->setTimeshiftController(services.timeshiftController.get());
    services.trayController = constructComponent<OKILTV::App::TrayController>(startupStep, QStringLiteral("TrayController"), []() {
        return std::make_unique<OKILTV::App::TrayController>();
    });
    services.displaySleepBlocker = constructComponent<OKILTV::App::DisplaySleepBlocker>(
        startupStep,
        QStringLiteral("DisplaySleepBlocker"),
        []() {
            return std::make_unique<OKILTV::App::DisplaySleepBlocker>();
        });
    services.settingsController = constructComponent<OKILTV::App::SettingsController>(
        startupStep,
        QStringLiteral("SettingsController"),
        [settings, &services]() {
            return std::make_unique<OKILTV::App::SettingsController>(
                settings,
                services.playerController.get(),
                services.multiViewController.get(),
                services.profilesModel.get());
        });
    services.portableRuntimeController = constructComponent<OKILTV::App::PortableRuntimeController>(
        startupStep,
        QStringLiteral("PortableRuntimeController"),
        []() {
            return std::make_unique<OKILTV::App::PortableRuntimeController>();
        });
    services.uiTestCaptureController = constructComponent<OKILTV::App::UiTestCaptureController>(
        startupStep,
        QStringLiteral("UiTestCaptureController"),
        []() {
            return std::make_unique<OKILTV::App::UiTestCaptureController>();
        });
    services.uiTestBridge = constructComponent<OKILTV::App::UiTestBridge>(startupStep, QStringLiteral("UiTestBridge"), []() {
        return std::make_unique<OKILTV::App::UiTestBridge>();
    });

    startupStep(QStringLiteral("App services ready."));
    return services;
}

void wireInterControllerSignals(const StartupLogFn &startupStep, AppServices &services)
{
    startupStep(QStringLiteral("Wiring inter-controller signals."));

    QObject::connect(
        services.playerController.get(),
        &OKILTV::App::PlayerController::playbackChannelActivated,
        services.playbackNowNextModel.get(),
        [&services]() {
            services.playbackNowNextModel->setChannel(services.playerController->currentChannelValue());
        });
    QObject::connect(
        services.playerController.get(),
        &OKILTV::App::PlayerController::currentChannelChanged,
        services.playbackNowNextModel.get(),
        [&services]() {
            if (!services.playerController->currentChannelValue().has_value()) {
                services.playbackNowNextModel->clear();
            }
        });
    QObject::connect(
        services.settingsController.get(),
        &OKILTV::App::SettingsController::saved,
        services.timeshiftController.get(),
        &OKILTV::App::TimeshiftController::applySettings);

    startupStep(QStringLiteral("Inter-controller signals wired."));
}

void registerQmlContextProperties(
    QQmlApplicationEngine &engine,
    OKILTV::App::AppController *appController,
    const AppServices &services)
{
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), appController);
    engine.rootContext()->setContextProperty(QStringLiteral("profilesModel"), services.profilesModel.get());
    engine.rootContext()->setContextProperty(QStringLiteral("channelListModel"), services.channelListModel.get());
    engine.rootContext()->setContextProperty(QStringLiteral("nowNextModel"), services.nowNextModel.get());
    engine.rootContext()->setContextProperty(QStringLiteral("playbackNowNextModel"), services.playbackNowNextModel.get());
    engine.rootContext()->setContextProperty(QStringLiteral("epgGridModel"), services.epgGridModel.get());
    engine.rootContext()->setContextProperty(QStringLiteral("guideStateModel"), services.guideStateModel.get());
    engine.rootContext()->setContextProperty(QStringLiteral("settingsSourceGroupsModel"), services.settingsSourceGroupsModel.get());
    engine.rootContext()->setContextProperty(QStringLiteral("liveSourceGroupsModel"), services.liveSourceGroupsModel.get());
    engine.rootContext()->setContextProperty(QStringLiteral("shellController"), services.shellController.get());
    engine.rootContext()->setContextProperty(QStringLiteral("appPlayerController"), services.playerController.get());
    engine.rootContext()->setContextProperty(QStringLiteral("multiViewController"), services.multiViewController.get());
    engine.rootContext()->setContextProperty(QStringLiteral("dvrController"), services.dvrController.get());
    engine.rootContext()->setContextProperty(QStringLiteral("trayController"), services.trayController.get());
    engine.rootContext()->setContextProperty(QStringLiteral("settingsController"), services.settingsController.get());
    engine.rootContext()->setContextProperty(QStringLiteral("portableRuntimeController"), services.portableRuntimeController.get());
    engine.rootContext()->setContextProperty(QStringLiteral("uiTestCaptureController"), services.uiTestCaptureController.get());
    engine.rootContext()->setContextProperty(QStringLiteral("uiTestBridge"), services.uiTestBridge.get());
}

} // namespace

int main(int argc, char *argv[])
{
    std::set_terminate([]() {
        QString detail = QStringLiteral("Unknown unhandled exception.");
        if (const auto exception = std::current_exception()) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::exception &error) {
                detail = QString::fromUtf8(error.what());
            } catch (...) {
                detail = QStringLiteral("Unhandled non-std exception.");
            }
        }

        OKILTV::Core::DebugLogger::instance().log(
            QStringLiteral("fatal"),
            QStringLiteral("std::terminate invoked: %1").arg(detail));
        abort();
    });

    auto startupStep = [](QStringView message) {
        OKILTV::Core::DebugLogger::instance().log(QStringLiteral("startup"), QString(message));
    };

    if (qEnvironmentVariableIsEmpty("QT_OPENGL")) {
        qputenv("QT_OPENGL", "desktop");
    }

    if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND")) {
        qputenv("QSG_RHI_BACKEND", "opengl");
    }

    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("Fusion"));
    }

    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QSurfaceFormat defaultFormat;
    defaultFormat.setRenderableType(QSurfaceFormat::OpenGL);
    defaultFormat.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    defaultFormat.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(defaultFormat);

    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    const auto startupOptions = parseStartupOptions(argc, argv);
    OKILTV::Core::AppDataPaths::initializeRuntime(startupOptions.runtimeContext);

    std::vector<char *> filteredArgv;
    filteredArgv.reserve(startupOptions.filteredArguments.size());
    for (const auto &argument : startupOptions.filteredArguments) {
        filteredArgv.push_back(const_cast<char *>(argument.constData()));
    }

    auto filteredArgc = static_cast<int>(filteredArgv.size());
    QApplication application(filteredArgc, filteredArgv.data());
    std::setlocale(LC_NUMERIC, "C");
    application.setOrganizationName(QStringLiteral("OKILTV"));
    application.setApplicationName(QStringLiteral("OKILTV"));
    application.setWindowIcon(QIcon(QStringLiteral(":/resources/icons/app.png")));
    OKILTV::Core::DebugLogger::instance().startSessionLogging();
    startupStep(QStringLiteral("QGuiApplication initialized. Platform=%1").arg(QGuiApplication::platformName()));

    try {
        registerMetaTypes(startupStep);
        auto coreServices = constructCoreServices(startupStep);
        auto appServices = constructAppServices(
            startupStep,
            coreServices.settings.get(),
            coreServices.database.get(),
            coreServices.epgService.get());
        wireInterControllerSignals(startupStep, appServices);

        auto appController = constructComponent<OKILTV::App::AppController>(
            startupStep,
            QStringLiteral("AppController"),
            [&]() {
                return std::make_unique<OKILTV::App::AppController>(
                    coreServices.settings.get(),
                    coreServices.database.get(),
                    coreServices.network,
                    appServices.profilesModel.get(),
                    appServices.channelListModel.get(),
                    appServices.nowNextModel.get(),
                    appServices.playbackNowNextModel.get(),
                    appServices.epgGridModel.get(),
                    appServices.guideStateModel.get(),
                    appServices.shellController.get(),
                    appServices.multiViewController.get(),
                    appServices.playerController.get(),
                    appServices.dvrController.get(),
                    appServices.timeshiftController.get(),
                    appServices.settingsController.get(),
                    coreServices.epgService.get());
            });
        appServices.uiTestBridge->attachControllers(
            appController.get(),
            appServices.shellController.get(),
            appServices.channelListModel.get(),
            appServices.guideStateModel.get(),
            appServices.playbackNowNextModel.get(),
            appServices.playerController.get(),
            appServices.timeshiftController.get(),
            appServices.settingsController.get(),
            appServices.uiTestCaptureController.get());

        startupStep(QStringLiteral("Creating QQmlApplicationEngine."));
        QQmlApplicationEngine engine;
        startupStep(QStringLiteral("Registering QML context properties."));
        registerQmlContextProperties(engine, appController.get(), appServices);
        startupStep(QStringLiteral("QML context properties registered."));

        startupStep(QStringLiteral("Loading main QML."));
        engine.addImportPath(QStringLiteral("qrc:/"));
        engine.addImportPath(QLibraryInfo::path(QLibraryInfo::QmlImportsPath));
        engine.loadFromModule(QStringLiteral("OKILTV"), QStringLiteral("Main"));
        if (engine.rootObjects().isEmpty()) {
            engine.load(QUrl(QStringLiteral("qrc:/qt/qml/OKILTV/qml/Main.qml")));
        }
        if (engine.rootObjects().isEmpty()) {
            engine.load(QUrl(QStringLiteral("qrc:/OKILTV/qml/Main.qml")));
        }
        if (engine.rootObjects().isEmpty()) {
            OKILTV::Core::DebugLogger::instance().log(
                QStringLiteral("startup"),
                QStringLiteral("QQmlApplicationEngine failed to create the root object."));
            return 1;
        }
        auto *mainWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        if (mainWindow == nullptr) {
            startupStep(QStringLiteral("Main QML root object is not a QQuickWindow; display sleep blocking disabled."));
            appServices.displaySleepBlocker->setBlocked(false);
        } else {
#if defined(Q_OS_WINDOWS)
            enableNativeSnapSupportForFramelessWindow(mainWindow);
#endif
            QObject::connect(
                mainWindow,
                &QWindow::visibilityChanged,
                &application,
                [&appServices, mainWindow]() {
                    updateDisplaySleepBlockerForWindowState(
                        appServices.displaySleepBlocker.get(),
                        appServices.settingsController.get(),
                        appServices.playerController.get(),
                        mainWindow);
                });
            QObject::connect(
                mainWindow,
                &QWindow::windowStateChanged,
                &application,
                [&appServices, mainWindow]() {
                    updateDisplaySleepBlockerForWindowState(
                        appServices.displaySleepBlocker.get(),
                        appServices.settingsController.get(),
                        appServices.playerController.get(),
                        mainWindow);
                });
            QObject::connect(
                appServices.playerController.get(),
                &OKILTV::App::PlayerController::isPlayingChanged,
                &application,
                [&appServices, mainWindow]() {
                    updateDisplaySleepBlockerForWindowState(
                        appServices.displaySleepBlocker.get(),
                        appServices.settingsController.get(),
                        appServices.playerController.get(),
                        mainWindow);
                });
            QObject::connect(
                appServices.settingsController.get(),
                &OKILTV::App::SettingsController::settingsChanged,
                &application,
                [&appServices, mainWindow]() {
                    updateDisplaySleepBlockerForWindowState(
                        appServices.displaySleepBlocker.get(),
                        appServices.settingsController.get(),
                        appServices.playerController.get(),
                        mainWindow);
                });
            updateDisplaySleepBlockerForWindowState(
                appServices.displaySleepBlocker.get(),
                appServices.settingsController.get(),
                appServices.playerController.get(),
                mainWindow);
        }
        appServices.uiTestCaptureController->setWindow(engine.rootObjects().constFirst());
        appServices.uiTestBridge->setWindow(engine.rootObjects().constFirst());
        startupStep(QStringLiteral("Main QML loaded successfully."));

        auto shutdownHandled = false;
        QObject::connect(&application, &QCoreApplication::aboutToQuit, &application, [&]() {
            if (shutdownHandled) {
                return;
            }
            shutdownHandled = true;

            OKILTV::Core::DebugLogger::instance().log(
                QStringLiteral("shutdown"),
                QStringLiteral("Application shutdown started."));
#if defined(Q_OS_WINDOWS)
            const auto processId = GetCurrentProcessId();
            std::thread([processId]() {
                const auto processHandle = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, processId);
                if (processHandle != nullptr) {
                    const auto waitResult = WaitForSingleObject(processHandle, 5000);
                    if (waitResult == WAIT_TIMEOUT) {
                        TerminateProcess(processHandle, 1);
                    }
                    CloseHandle(processHandle);
                }
            }).detach();
#endif

            OKILTV::Core::DebugLogger::instance().log(
                QStringLiteral("shutdown"),
                QStringLiteral("Shutting down PlayerController."));
            appServices.playerController->shutdownForApplicationExit();
            OKILTV::Core::DebugLogger::instance().log(
                QStringLiteral("shutdown"),
                QStringLiteral("Shutting down TimeshiftController."));
            appServices.timeshiftController->shutdownForApplicationExit();
            OKILTV::Core::DebugLogger::instance().log(
                QStringLiteral("shutdown"),
                QStringLiteral("Shutting down DvrController."));
            appServices.dvrController->shutdownForApplicationExit();
            OKILTV::Core::DebugLogger::instance().log(
                QStringLiteral("shutdown"),
                QStringLiteral("Application shutdown teardown completed."));
        });

        startupStep(QStringLiteral("Scheduling AppController initialization."));
        QMetaObject::invokeMethod(appController.get(), "initialize", Qt::QueuedConnection);
        startupStep(QStringLiteral("Entering event loop."));
        return application.exec();
    } catch (const std::exception &error) {
        const auto message = QString::fromUtf8(error.what());
        OKILTV::Core::DebugLogger::instance().log(
            QStringLiteral("fatal"),
            QStringLiteral("Unhandled std::exception during startup: %1").arg(message));
        return 2;
    } catch (...) {
        OKILTV::Core::DebugLogger::instance().log(
            QStringLiteral("fatal"),
            QStringLiteral("Unhandled non-std exception during startup."));
        return 3;
    }
}
