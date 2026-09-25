pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtQml
import OKILTV
import "theme/Theme.js" as Theme

ApplicationWindow {
    id: window

    width: 1600
    height: 900
    minimumWidth: 426
    minimumHeight: 240
    visible: true
    property bool alwaysOnTop: false
    flags: alwaysOnTop
        ? (Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint)
        : (Qt.Window | Qt.FramelessWindowHint)
    title: "OKILTV"
    color: Theme.window
    property bool allowWindowClose: false
    property string pendingCloseSource: ""
    readonly property bool topBarVisible: livePage.topBarVisible
    readonly property int topBarReservedHeight: windowChromeBar.occupiedHeight

    FontLoader {
        id: plexRegular
        source: "assets/fonts/LiberationSans-Regular.ttf"
    }

    font.family: plexRegular.status === FontLoader.Ready ? plexRegular.name : font.family
    // qmllint disable unqualified
    readonly property var dateTime: dateTimeFormatter
    readonly property var shell: shellController
    readonly property var settings: settingsController
    readonly property var tray: trayController
    readonly property var dvr: dvrController
    readonly property var app: appController
    readonly property var downloads: catchupDownloadController
    readonly property var updates: updateCheckController
    readonly property var multiView: multiViewController
    // qmllint enable unqualified
    readonly property bool overlayShortcutsEnabled: window.shell.activeOverlay !== "settings" && !downloadUi.interactionActive && !dvrExitDialog.visible && !updateDialog.visible && !window.downloads.shuttingDown
    readonly property bool liveShortcutsEnabled: window.overlayShortcutsEnabled && !livePage.searchFieldActive
    readonly property var forwardedShortcuts: {
        const shortcuts = [
            { sequence: "Space", key: Qt.Key_Space, scope: "live" },
            { sequence: "F", key: Qt.Key_F, scope: "live" },
            { sequence: "F1", key: Qt.Key_F1, scope: "always" },
            { sequence: "F2", key: Qt.Key_F2, scope: "always" },
            { sequence: "F3", key: Qt.Key_F3, scope: "always" },
            { sequence: "F6", key: Qt.Key_F6, scope: "always" },
            { sequence: ",", key: Qt.Key_Comma, scope: "live" },
            { sequence: ".", key: Qt.Key_Period, scope: "live" },
            { sequence: "Ctrl+G", key: Qt.Key_G, modifiers: Qt.ControlModifier, scope: "overlay" },
            { sequence: "Ctrl+S", key: Qt.Key_S, modifiers: Qt.ControlModifier, scope: "overlay" },
            { sequence: "Ctrl+F", key: Qt.Key_F, modifiers: Qt.ControlModifier, scope: "nonGuideOverlay" },
            { sequence: "Ctrl+O", key: Qt.Key_O, modifiers: Qt.ControlModifier, scope: "live" },
            { sequence: "Ctrl+P", key: Qt.Key_P, modifiers: Qt.ControlModifier, scope: "live" },
            { sequence: "Ctrl+Shift+P", key: Qt.Key_P, modifiers: Qt.ControlModifier | Qt.ShiftModifier, scope: "live" },
            { sequence: "Ctrl+D", key: Qt.Key_D, modifiers: Qt.ControlModifier, scope: "download" },
            { sequence: "Ctrl+R", key: Qt.Key_R, modifiers: Qt.ControlModifier, scope: "overlay" },
            { sequence: "Ctrl+Return", key: Qt.Key_Return, modifiers: Qt.ControlModifier, scope: "guideOrGrid" },
            { sequence: "Ctrl+Enter", key: Qt.Key_Enter, modifiers: Qt.ControlModifier, scope: "guideOrGrid" },
            { sequence: "Ctrl+Up", key: Qt.Key_Up, modifiers: Qt.ControlModifier, scope: "overlay" },
            { sequence: "Ctrl+Down", key: Qt.Key_Down, modifiers: Qt.ControlModifier, scope: "guideOrGrid" },
            { sequence: "Ctrl+Left", key: Qt.Key_Left, modifiers: Qt.ControlModifier, scope: "grid" },
            { sequence: "Ctrl+Right", key: Qt.Key_Right, modifiers: Qt.ControlModifier, scope: "grid" },
            { sequence: "J", key: Qt.Key_J, scope: "live" },
            { sequence: "L", key: Qt.Key_L, scope: "live" },
            { sequence: "Home", key: Qt.Key_Home, scope: "live" },
            { sequence: "Up", key: Qt.Key_Up, scope: "live" },
            { sequence: "Down", key: Qt.Key_Down, scope: "live" },
            { sequence: "Left", key: Qt.Key_Left, scope: "live" },
            { sequence: "Right", key: Qt.Key_Right, scope: "live" },
            { sequence: "Return", key: Qt.Key_Return, scope: "overlay" },
            { sequence: "Enter", key: Qt.Key_Enter, scope: "overlay" },
            { sequence: "Delete", key: Qt.Key_Delete, scope: "overlay" }
        ]

        for (let digit = 0; digit <= 9; ++digit) {
            shortcuts.push({
                sequence: String(digit),
                key: Qt.Key_0 + digit,
                scope: "live"
            })
        }

        return shortcuts
    }

    function updateWindowState() {
        window.shell.updateWindowMetrics(width, height)
        window.shell.fullscreen = visibility === Window.FullScreen
    }

    function restoreFromTray() {
        if (visibility === Window.Minimized || !visible) {
            showNormal()
        }
        raise()
        requestActivate()
    }

    function syncTrayIconVisibility() {
        if (!window.tray.available) {
            return
        }
        if (window.settings.minimizeToTrayOnMinimize) {
            window.tray.showTrayIcon()
        } else {
            window.tray.hideTrayIcon()
        }
    }

    function requestAppClose(source) {
        const closeSource = source || "window"
        if (window.dvr.exitConfirmationRequired || window.downloads.hasPending) {
            window.pendingCloseSource = closeSource
            if (closeSource === "tray") {
                restoreFromTray()
            }
            dvrExitDialog.open()
            return
        }

        window.beginExit()
    }

    function requestCatchupDownload(channel, program) {
        downloadUi.requestDownload(channel, program)
    }

    function beginExit() {
        if (window.downloads.shuttingDown)
            return
        window.updates.shutdown()
        window.downloads.shutdown()
    }

    function finishExit() {
        window.pendingCloseSource = ""
        window.allowWindowClose = true
        close()
        Qt.callLater(function() { Qt.quit() })
    }

    function toggleAlwaysOnTop() {
        window.alwaysOnTop = !window.alwaysOnTop
    }

    onWidthChanged: updateWindowState()
    onHeightChanged: updateWindowState()
    onVisibilityChanged: {
        updateWindowState()
        if (window.visibility === Window.Minimized
            && window.settings.minimizeToTrayOnMinimize
            && window.tray.available) {
            window.tray.showTrayIcon()
            hide()
        }
    }
    onClosing: function(close) {
        if (window.allowWindowClose) {
            close.accepted = true
            window.shell.setReopenMaximizedOnLaunch(visibility === Window.Maximized)
            window.tray.hideTrayIcon()
            window.allowWindowClose = false
            window.pendingCloseSource = ""
            return
        }
        close.accepted = false
        window.requestAppClose("window")
    }

    function dispatchShortcut(key, modifiers) {
        if (downloadUi.interactionActive || dvrExitDialog.visible || updateDialog.visible || window.downloads.shuttingDown)
            return false
        return livePage.handleWindowKey({
            key: key,
            modifiers: modifiers !== undefined ? modifiers : Qt.NoModifier
        })
    }

    function shortcutEnabled(scope) {
        if (downloadUi.interactionActive || dvrExitDialog.visible || updateDialog.visible || window.downloads.shuttingDown)
            return false
        if (scope === "grid" || scope === "guideOrGrid") {
            return livePage.multiviewSelectionAvailable
                || (scope === "guideOrGrid" && window.shell.activeOverlay === "guide")
        }
        if (scope === "always") {
            return true
        }
        if (scope === "download") {
            return window.overlayShortcutsEnabled && !livePage.searchFieldActive && !livePage.leftPickerOpen
        }
        if (scope === "live") {
            return window.liveShortcutsEnabled
        }
        if (scope === "overlay") {
            return window.overlayShortcutsEnabled
        }
        if (scope === "nonGuideOverlay") {
            return window.overlayShortcutsEnabled && window.shell.activeOverlay !== "guide"
        }
        return false
    }

    Instantiator {
        model: window.forwardedShortcuts

        delegate: Shortcut {
            required property var modelData

            sequence: modelData.sequence
            autoRepeat: modelData.sequence !== "Ctrl+O"
                && modelData.sequence !== "Ctrl+Return" && modelData.sequence !== "Ctrl+Enter"
            enabled: window.shortcutEnabled(modelData.scope)
            onActivated: window.dispatchShortcut(
                modelData.key,
                modelData.modifiers !== undefined ? modelData.modifiers : Qt.NoModifier)
        }
    }

    Shortcut {
        sequence: "M"
        enabled: window.liveShortcutsEnabled
        onActivated: window.dispatchShortcut(Qt.Key_M)
    }

    Shortcut {
        sequence: "Escape"
        enabled: !downloadUi.choosingFile && !dvrExitDialog.visible && !updateDialog.visible && !window.downloads.shuttingDown
        onActivated: {
            if (downloadUi.handleEscape())
                return
            if (!window.dispatchShortcut(Qt.Key_Escape) && window.visibility === Window.FullScreen) {
                window.showNormal()
            }
        }
    }

    Shortcut {
        sequence: "Tab"
        enabled: window.overlayShortcutsEnabled
        onActivated: window.dispatchShortcut(Qt.Key_Tab)
    }

    LiveTvPage {
        id: livePage
        anchors.fill: parent
        mainWindow: window
        downloadIndicatorVisible: downloadUi.indicatorVisible
        topBarExternalHideLock: windowChromeBar.interactionActive || windowResizeHandles.interactionActive || downloadUi.interactionActive
    }

    CatchupDownloads {
        dateTimePattern: window.dateTime.dateTimePattern
        id: downloadUi
        anchors.fill: parent
        buttonHost: livePage.downloadButtonHost
        transportBar: livePage.downloadTransportBar
        controller: window.downloads
        app: window.app
        shellChromeVisible: window.shell.overlaysVisible || window.shell.activeOverlay !== "none"
        mainWindow: window
        topInset: window.topBarReservedHeight
        uiTransparency: window.settings.uiTransparency
        z: 40
    }

    Connections {
        target: window.downloads
        function onShutdownFinished() { Qt.callLater(function() { window.finishExit() }) }
    }

    WindowResizeHandles {
        id: windowResizeHandles
        anchors.fill: parent
        window: window
        livePage: livePage
        z: 15
    }

    WindowChromeBar {
        id: windowChromeBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        window: window
        livePage: livePage
        targetVisible: livePage.topBarVisible
        z: 30
    }

    BusyIndicator {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.spacingM
        anchors.topMargin: Theme.spacingM + window.topBarReservedHeight
        running: window.app.isBusy
        visible: running
        z: 20
    }

    UpdateAvailableDialog {
        id: updateDialog
        controller: window.updates
        uiTransparency: window.settings.uiTransparency
        allowedToOpen: window.visible && window.visibility !== Window.Minimized
            && !downloadUi.interactionActive && !dvrExitDialog.visible
            && window.pendingCloseSource === "" && !window.downloads.shuttingDown
            && window.shell.activeOverlay !== "settings" && !window.multiView.degradePromptVisible
    }

    Dialog {
        id: dvrExitDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        focus: true
        title: "Background tasks"
        standardButtons: Dialog.Yes | Dialog.No
        width: Math.min(440, parent.width - 32)
        padding: 20
        spacing: 16

        background: Rectangle {
            radius: 8
            color: Theme.uiBackground("#e6070d12", window.settings.uiTransparency)
        }

        header: Text {
            text: dvrExitDialog.title
            color: Theme.textPrimary
            font.pixelSize: 16
            font.bold: true
            leftPadding: dvrExitDialog.leftPadding
            rightPadding: dvrExitDialog.rightPadding
            topPadding: dvrExitDialog.topPadding
            wrapMode: Text.Wrap
        }

        footer: DialogButtonBox {
            standardButtons: dvrExitDialog.standardButtons
            alignment: Qt.AlignRight
            leftPadding: dvrExitDialog.leftPadding
            rightPadding: dvrExitDialog.rightPadding
            bottomPadding: dvrExitDialog.bottomPadding
            background: Item {}
            delegate: AppButton {
                id: exitAction
                compact: true
                borderless: true
                background: Rectangle {
                    radius: 4
                    color: exitAction.down ? "#35ffffff"
                        : (exitAction.hovered || exitAction.visualFocus ? "#20ffffff" : "transparent")
                }
            }
        }

        contentItem: Text {
            text: (window.dvr.exitConfirmationRequired
                ? "A recording is active or scheduled to start within 15 minutes. " : "")
                + (window.downloads.hasPending
                    ? "Active, paused and queued downloads will be cancelled and their unfinished files deleted. " : "")
                + "Exit anyway?"
            font.pixelSize: 14
            wrapMode: Text.Wrap
            color: Theme.textPrimary
        }

        onAccepted: {
            window.beginExit()
        }
        onRejected: window.pendingCloseSource = ""
    }

    Connections {
        target: window.tray

        function onShowRequested() {
            window.restoreFromTray()
        }

        function onExitRequested() {
            window.requestAppClose("tray")
        }
    }

    Connections {
        target: window.settings

        function onSettingsChanged() {
            window.syncTrayIconVisibility()
        }
    }

    Component.onCompleted: {
        if (window.shell.reopenMaximizedOnLaunch()) {
            showMaximized()
        }
        updateWindowState()
        syncTrayIconVisibility()
    }
}
