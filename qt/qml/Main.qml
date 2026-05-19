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
    readonly property var shell: shellController
    readonly property var settings: settingsController
    readonly property var tray: trayController
    readonly property var dvr: dvrController
    readonly property var app: appController
    // qmllint enable unqualified
    readonly property bool overlayShortcutsEnabled: window.shell.activeOverlay !== "settings"
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
            { sequence: "Ctrl+Alt+O", key: Qt.Key_O, modifiers: Qt.ControlModifier | Qt.AltModifier, scope: "live" },
            { sequence: "Ctrl+Shift+O", key: Qt.Key_O, modifiers: Qt.ControlModifier | Qt.ShiftModifier, scope: "live" },
            { sequence: "Ctrl+P", key: Qt.Key_P, modifiers: Qt.ControlModifier, scope: "live" },
            { sequence: "Ctrl+Shift+P", key: Qt.Key_P, modifiers: Qt.ControlModifier | Qt.ShiftModifier, scope: "live" },
            { sequence: "Ctrl+R", key: Qt.Key_R, modifiers: Qt.ControlModifier, scope: "overlay" },
            { sequence: "Ctrl+Return", key: Qt.Key_Return, modifiers: Qt.ControlModifier, scope: "guideOnly" },
            { sequence: "Ctrl+Enter", key: Qt.Key_Enter, modifiers: Qt.ControlModifier, scope: "guideOnly" },
            { sequence: "Ctrl+Up", key: Qt.Key_Up, modifiers: Qt.ControlModifier, scope: "overlay" },
            { sequence: "Ctrl+Down", key: Qt.Key_Down, modifiers: Qt.ControlModifier, scope: "guideOnly" },
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
        if (window.dvr.exitConfirmationRequired) {
            window.pendingCloseSource = closeSource
            if (closeSource === "tray") {
                restoreFromTray()
            }
            dvrExitDialog.open()
            return
        }

        window.pendingCloseSource = ""
        window.allowWindowClose = true
        close()
        Qt.callLater(function() {
            Qt.quit()
        })
    }

    function toggleAlwaysOnTop() {
        window.alwaysOnTop = !window.alwaysOnTop
    }

    onWidthChanged: updateWindowState()
    onHeightChanged: updateWindowState()
    onVisibilityChanged: {
        updateWindowState()
        if (visibility === Window.Minimized
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
        if (window.dvr.exitConfirmationRequired) {
            close.accepted = false
            window.pendingCloseSource = "window"
            dvrExitDialog.open()
            return
        }
        window.shell.setReopenMaximizedOnLaunch(visibility === Window.Maximized)
        window.tray.hideTrayIcon()
    }

    function dispatchShortcut(key, modifiers) {
        return livePage.handleWindowKey({
            key: key,
            modifiers: modifiers !== undefined ? modifiers : Qt.NoModifier
        })
    }

    function shortcutEnabled(scope) {
        if (scope === "always") {
            return true
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
        if (scope === "guideOnly") {
            return window.shell.activeOverlay === "guide"
        }
        return false
    }

    Instantiator {
        model: window.forwardedShortcuts

        delegate: Shortcut {
            required property var modelData

            sequence: modelData.sequence
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
        onActivated: {
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
        topBarExternalHideLock: windowChromeBar.interactionActive || windowResizeHandles.interactionActive
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

    Dialog {
        id: dvrExitDialog
        modal: true
        focus: true
        title: "Recording in progress"
        standardButtons: Dialog.Yes | Dialog.No

        contentItem: Text {
            width: 360
            text: "A recording is active or scheduled to start within 15 minutes. Exit anyway?"
            wrapMode: Text.Wrap
            color: Theme.textPrimary
        }

        onAccepted: {
            window.allowWindowClose = true
            window.close()
            Qt.callLater(function() {
                Qt.quit()
            })
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
