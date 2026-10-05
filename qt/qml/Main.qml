pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtQuick.Dialogs
import QtQml
import OKILTV
import "theme/Theme.js" as Theme

ApplicationWindow {
    id: window

    FileDialog {
        id: subtitleFileDialog
        title: "Upload subtitles..."
        fileMode: FileDialog.OpenFile
        nameFilters: ["Subtitle files (*.srt *.ass *.ssa *.vtt *.sub *.idx *.sup *.pgs *.smi *.sami *.scc *.ttml *.dfxp *.lrc *.txt *.mks *.rt *.utf *.utf8 *.utf-8)", "All files (*)"]
        property var previousFocus: null
        onAccepted: { window.vod.finishSubtitleUpload(selectedFile); restoreFocus() }
        onRejected: { window.vod.finishSubtitleUpload(); restoreFocus() }
        function restoreFocus() {
            if (previousFocus) previousFocus.forceActiveFocus()
            previousFocus = null
        }
    }
    Connections {
        target: window.vod
        function onSubtitleUploadRequested() {
            subtitleFileDialog.previousFocus = window.activeFocusItem
            subtitleFileDialog.open()
        }
    }

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
    readonly property bool topBarVisible: windowChromeBar.visible
    readonly property int topBarReservedHeight: window.visibility === Window.FullScreen
        ? 0 : windowChromeBar.occupiedHeight
    readonly property int mediaNavigationHeight: mediaModeChrome.occupiedHeight
    readonly property real mediaNavigationWidth: mediaModeChrome.navigationWidth
    readonly property bool modeNavigationFocused: mediaModeChrome.navigationFocused
    readonly property bool chromeAnimationsRunning: livePage.chromeAnimationsRunning
        || windowChromeBar.animating || mediaModeChrome.animating
    readonly property string selectedMediaMode: window.vodOpen ? window.vodLibraryKind
        : window.vod && (window.vod.active || window.vod.episodeTransition)
            ? (window.vod.activeSeries ? "series" : "movies") : "live"
    readonly property bool vodPalette: window.vodMounted || window.selectedMediaMode !== "live"
    property bool pendingLiveNavigation: false
    readonly property bool modeNavigationEnabled: window.shell.activeOverlay !== "settings"
        && !window.vodTransitioning && !livePage.chromeAnimationsRunning
        && !window.pendingLiveNavigation && !(window.vod && window.vod.liveTransition)
        && !window.subtitleDialogActive && !downloadUi.interactionActive && !dvrExitDialog.visible && !updateDialog.visible
        && !window.downloads.shuttingDown && !window.multiView.degradePromptVisible

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
    readonly property var vod: typeof vodRuntime !== "undefined" ? vodRuntime : null
    // qmllint enable unqualified
    readonly property bool vodOpen: window.shell.activeOverlay === "vod"
    property string vodLibraryKind: "movies"
    readonly property var vodPage: vodLibraryKind === "series" ? seriesPage : moviesPage
    property bool vodMounted: false
    property bool vodClosing: false
    property real vodSlideProgress: 0
    readonly property bool vodTransitioning: vodMounted
        && (vodClosing || vodSlideProgress < 1 || vodSlideAnimation.running)
    readonly property bool textEditorFocused: activeFocusItem instanceof TextInput || activeFocusItem instanceof TextEdit
    readonly property bool subtitleDialogActive: window.vod !== null && Boolean(window.vod.subtitleDialogOpen)
    readonly property bool overlayShortcutsEnabled: window.shell.activeOverlay !== "settings" && !window.vodOpen && !window.subtitleDialogActive && !downloadUi.interactionActive && !dvrExitDialog.visible && !updateDialog.visible && !window.downloads.shuttingDown
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
            { sequence: "Backspace", key: Qt.Key_Backspace, scope: "vodBack" },
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

        const separator = livePage.decimalSeparator
        if (separator !== "." && separator !== ",") {
            shortcuts.push({ sequence: separator, key: 0, text: separator, scope: "live" })
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

    function toggleVod(kind = "movies") {
        if (window.vodTransitioning) return true
        if (window.textEditorFocused || window.shell.activeOverlay === "settings"
            || window.subtitleDialogActive || downloadUi.interactionActive || dvrExitDialog.visible || updateDialog.visible || window.downloads.shuttingDown)
            return false
        if (window.vodOpen && window.vodLibraryKind === kind) closeVod()
        else window.openVodLibrary(kind)
        return true
    }
    function openVodLibrary(kind) {
        if (window.vodOpen && window.vodLibraryKind === kind) return
        const switching = window.vodOpen
        if (switching) window.vodPage.catalog.close()
        window.vodLibraryKind = kind
        if (switching) window.vodPage.prepareForOpen()
        else window.shell.openOverlay("vod")
    }
    function selectMediaMode(mode) {
        if (!window.modeNavigationEnabled) return
        if (mode !== "live") {
            window.openVodLibrary(mode)
            return
        }
        if (window.vod) window.vod.finishLibraryBrowsing(false)
        if (window.vodOpen) {
            window.pendingLiveNavigation = true
            window.closeVod()
        } else window.activateLiveSection()
    }
    function activateLiveSection() {
        window.shell.clearOverlay()
        livePage.forceActiveFocus()
        if (window.vod) window.vod.returnToLive()
        else window.app.returnToLive()
        window.pendingLiveNavigation = false
        livePage.revealUi("pointer")
    }
    onVodOpenChanged: {
        if (window.vodOpen) {
            if (window.vod) window.vod.beginLibraryBrowsing()
            vodSlideAnimation.stop()
            window.vodClosing = false
            window.vodSlideProgress = 0
            window.vodMounted = true
            vodPage.prepareForOpen()
            Qt.callLater(function() {
                if (!window.vodOpen || !window.vodMounted || window.vodClosing) return
                vodSlideAnimation.to = 1
                vodSlideAnimation.start()
            })
        } else if (window.vodMounted) window.finishVodClose(false)
    }
    function closeVod() {
        if (!window.vodMounted || window.vodClosing) return
        window.vodClosing = true
        vodPage.closePopups()
        vodSlideAnimation.stop()
        vodSlideAnimation.to = 0
        vodSlideAnimation.start()
    }
    function finishVodClose(clearOverlay) {
        vodSlideAnimation.stop()
        window.vodMounted = false
        window.vodClosing = false
        window.vodSlideProgress = 0
        if (!clearOverlay) window.pendingLiveNavigation = false
        if (window.pendingLiveNavigation && clearOverlay) {
            window.activateLiveSection()
            return
        }
        if (clearOverlay && window.vodOpen) {
            window.shell.clearOverlay()
            window.shell.overlaysVisible = false
            livePage.forceActiveFocus()
        }
        if (window.shell.activeOverlay === "none" && window.vod) window.vod.finishLibraryBrowsing()
    }
    function dispatchShortcut(key, modifiers, text) {
        if (window.subtitleDialogActive) return true
        if (downloadUi.interactionActive || dvrExitDialog.visible || updateDialog.visible || window.downloads.shuttingDown)
            return false
        if (window.vodOpen) {
            if (window.vodTransitioning) return key === Qt.Key_Escape
            return key === Qt.Key_Escape ? vodPage.handleEscape() : false
        }
        return livePage.handleWindowKey({
            key: key,
            text: text || "",
            modifiers: modifiers !== undefined ? modifiers : Qt.NoModifier
        })
    }

    function shortcutEnabled(scope) {
        if (window.modeNavigationFocused && scope !== "always") return false
        if (livePage.vodSeasonFocused && (scope === "live" || scope === "overlay")) return false
        if (window.subtitleDialogActive || downloadUi.interactionActive || dvrExitDialog.visible || updateDialog.visible || window.downloads.shuttingDown)
            return false
        if (window.vodOpen) return false
        if (scope === "vodBack") {
            return livePage.vodActive && window.shell.activeOverlay === "none"
                && !window.shell.overlaysVisible && !window.textEditorFocused
                && !livePage.leftPickerOpen && !livePage.chromeAnimationsRunning
        }
        if (scope === "grid" || scope === "guideOrGrid") {
            return livePage.multiviewSelectionAvailable
                || (scope === "guideOrGrid" && window.shell.activeOverlay === "guide")
        }
        if (scope === "always") {
            return true
        }
        if (scope === "download") {
            return window.overlayShortcutsEnabled && !(window.vod && window.vod.active) && !livePage.searchFieldActive && !livePage.leftPickerOpen
        }
        if (scope === "live") {
            return window.liveShortcutsEnabled
        }
        if (scope === "overlay") {
            return window.overlayShortcutsEnabled && !window.textEditorFocused
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
                && modelData.sequence !== "Backspace"
                && modelData.sequence !== "Ctrl+Return" && modelData.sequence !== "Ctrl+Enter"
            enabled: window.shortcutEnabled(modelData.scope)
            onActivated: window.dispatchShortcut(
                modelData.key,
                modelData.modifiers !== undefined ? modelData.modifiers : Qt.NoModifier,
                modelData.text || "")
        }
    }

    Shortcut {
        sequence: "V"
        autoRepeat: false
        enabled: !window.subtitleDialogActive && !window.textEditorFocused && window.shell.activeOverlay !== "settings"
            && !window.pendingLiveNavigation && !(window.vod && window.vod.liveTransition)
            && !downloadUi.interactionActive && !dvrExitDialog.visible && !updateDialog.visible && !window.downloads.shuttingDown
        onActivated: window.toggleVod()
    }
    Shortcut {
        sequence: "B"
        autoRepeat: false
        enabled: !window.subtitleDialogActive && !window.textEditorFocused && window.shell.activeOverlay !== "settings"
            && !window.pendingLiveNavigation && !(window.vod && window.vod.liveTransition)
            && !downloadUi.interactionActive && !dvrExitDialog.visible && !updateDialog.visible && !window.downloads.shuttingDown
        onActivated: window.toggleVod("series")
    }


    Shortcut {
        sequence: "M"
        enabled: window.liveShortcutsEnabled
        onActivated: window.dispatchShortcut(Qt.Key_M)
    }

    Shortcut {
        sequence: "Escape"
        enabled: !window.subtitleDialogActive && !downloadUi.choosingFile && !dvrExitDialog.visible && !updateDialog.visible && !window.downloads.shuttingDown
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
        enabled: window.overlayShortcutsEnabled && !window.modeNavigationFocused
        onActivated: {
            if (livePage.searchFieldActive && mediaModeChrome.visible && window.modeNavigationEnabled)
                mediaModeChrome.focusNavigation()
            else window.dispatchShortcut(Qt.Key_Tab)
        }
    }

    LiveTvPage {
        id: livePage
        anchors.fill: parent
        mainWindow: window
        downloadIndicatorVisible: downloadUi.indicatorVisible
        topBarExternalHideLock: windowChromeBar.interactionActive || mediaModeChrome.interactionActive
            || windowResizeHandles.interactionActive || window.subtitleDialogActive || downloadUi.interactionActive
        onTopBarExternalHideLockChanged: {
            if (topBarExternalHideLock) livePage.revealUi("pointer")
        }
    }

    NumberAnimation {
        id: vodSlideAnimation
        target: window
        property: "vodSlideProgress"
        duration: Theme.transitionMs + 60
        easing.type: Easing.OutCubic
        onFinished: {
            if (window.vodClosing) window.finishVodClose(true)
        }
    }

    Item {
        id: vodOverlayFrame
        objectName: "ui.region.vod_overlay"
        anchors.fill: parent
        anchors.topMargin: window.topBarReservedHeight
        visible: window.vodMounted
        clip: true
        z: 30

        // Keep uncovered video non-interactive until the overlay is fully hidden.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: wheel => { wheel.accepted = true }
        }

        VodMoviesPage {
            id: moviesPage
            objectName: "ui.vod.page"
            anchors.fill: parent
            visible: window.vodLibraryKind === "movies"
            enabled: visible && window.vodOpen && !window.vodTransitioning
                && !window.subtitleDialogActive && !downloadUi.interactionActive && !dvrExitDialog.visible && !updateDialog.visible
            transform: Translate { y: (1 - window.vodSlideProgress) * vodOverlayFrame.height }
            // qmllint disable unqualified
            catalog: vodCatalog
            // qmllint enable unqualified
            uiTransparency: window.settings.uiTransparency
            windowWidth: window.width
            navigationTopInset: Theme.mediaModeChromeHeight
            preferredSidebarWidth: window.shell.vodLibrarySidebarWidth
            onSidebarWidthCommitted: newWidth => window.shell.setVodLibrarySidebarWidth(newWidth)
            onCloseRequested: window.closeVod()
            Connections {
                target: moviesPage.catalog
                function onPlaybackStarted() { window.closeVod() }
            }
        }
        VodMoviesPage {
            id: seriesPage
            objectName: "ui.vod.seriesPage"
            anchors.fill: parent
            visible: window.vodLibraryKind === "series"
            enabled: visible && window.vodOpen && !window.vodTransitioning
                && !window.subtitleDialogActive && !downloadUi.interactionActive && !dvrExitDialog.visible && !updateDialog.visible
            transform: Translate { y: (1 - window.vodSlideProgress) * vodOverlayFrame.height }
            // qmllint disable unqualified
            catalog: vodSeriesCatalog
            // qmllint enable unqualified
            uiTransparency: window.settings.uiTransparency
            windowWidth: window.width
            navigationTopInset: Theme.mediaModeChromeHeight
            preferredSidebarWidth: window.shell.vodLibrarySidebarWidth
            onSidebarWidthCommitted: newWidth => window.shell.setVodLibrarySidebarWidth(newWidth)
            onCloseRequested: window.closeVod()
            Connections {
                target: seriesPage.catalog
                function onPlaybackStarted() { window.closeVod() }
            }
        }
    }

    Timer {
        interval: 100
        repeat: true
        running: window.vod && window.vod.libraryReturnPending
        onTriggered: {
            if (window.shell.activeOverlay !== "settings" && !window.vodTransitioning && !window.subtitleDialogActive && !downloadUi.interactionActive && !dvrExitDialog.visible && !updateDialog.visible)
                window.vod.deliverLibraryReturn()
        }
    }
    Connections {
        target: window.vod
        function onLiveRequested() { window.app.returnToLive() }
        function onLibraryRequested() {
            window.vodLibraryKind = "series"
            if (!window.vodOpen) window.shell.openOverlay("vod")
            else window.vodPage.prepareForOpen()
        }
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
        // Keep window edges reachable above VOD and title-bar input surfaces.
        z: 35
    }

    WindowChromeBar {
        id: windowChromeBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        window: window
        livePage: livePage
        targetVisible: livePage.topBarVisible
        vodPalette: window.vodPalette
        uiTransparency: window.settings.uiTransparency
        z: 30
    }

    MediaModeChrome {
        id: mediaModeChrome
        parent: window.contentItem
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: window.topBarReservedHeight
        // Navigation stays fixed above the library and through the Live reveal.
        targetVisible: livePage.showHoverUi || window.vodMounted || window.pendingLiveNavigation
            || (window.vod && window.vod.liveTransition) || livePage.chromeAnimationsRunning
        selectedMode: window.selectedMediaMode
        vodPalette: window.vodPalette
        navigationEnabled: window.modeNavigationEnabled
        uiTransparency: window.settings.uiTransparency
        onModeRequested: mode => window.selectMediaMode(mode)
        onNavigationDismissed: livePage.forceActiveFocus()
        z: 30
    }

    Connections {
        target: window.shell
        function onActiveOverlayChanged() {
            Qt.callLater(function() {
                if (window.shell.activeOverlay === "none" && !window.vodMounted
                    && !window.pendingLiveNavigation && window.vod) window.vod.finishLibraryBrowsing()
            })
        }
    }

    BusyIndicator {
        objectName: "ui.sourceRefresh.busy"
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.spacingM
        anchors.topMargin: Theme.spacingM + window.topBarReservedHeight
        running: window.app.isBusy || Boolean(window.vod && window.vod.sourceSyncInProgress)
        visible: running
        z: 31
    }

    Rectangle {
        id: vodNotice
        objectName: "ui.vod.notice"
        parent: window.Overlay.overlay
        property string message: ""
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: window.topBarReservedHeight + Theme.spacingL
        width: Math.min(520, parent.width - 32)
        height: vodNoticeText.implicitHeight + 24
        visible: message.length > 0
        color: Theme.surface
        radius: Theme.radiusM
        z: 100
        Text {
            id: vodNoticeText
            anchors.centerIn: parent
            width: parent.width - 24
            text: vodNotice.message
            color: Theme.textPrimary
            font.pixelSize: 14
            wrapMode: Text.Wrap
        }
        Timer {
            id: vodNoticeTimer
            interval: 8000
            onTriggered: vodNotice.message = ""
        }
        Connections {
            target: window.vod
            function onNotification(message) {
                vodNotice.message = message
                vodNoticeTimer.restart()
            }
        }
    }

    UpdateAvailableDialog {
        id: updateDialog
        controller: window.updates
        uiTransparency: window.settings.uiTransparency
        allowedToOpen: window.visible && window.visibility !== Window.Minimized
            && !window.subtitleDialogActive && !downloadUi.interactionActive && !dvrExitDialog.visible
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
