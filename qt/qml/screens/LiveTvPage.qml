import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import OKILTV
import "../theme/Theme.js" as Theme

Item {
    id: root

    FontLoader {
        id: numericOsdFont
        source: "qrc:/resources/fonts/VCR_OSD_MONO_1.001.ttf"
    }

    property var mainWindow
    // qmllint disable unqualified
    readonly property var shell: shellController
    readonly property var settings: settingsController
    readonly property var app: appController
    readonly property var player: appPlayerController
    readonly property var channelList: channelListModel
    readonly property var guideState: guideStateModel
    readonly property var nowNext: nowNextModel
    readonly property var playbackNowNext: playbackNowNextModel
    readonly property var profiles: profilesModel
    readonly property var liveGroups: liveSourceGroupsModel
    readonly property var multiView: multiViewController
    readonly property var dvr: dvrController
    // qmllint enable unqualified
    property string currentClockText: Qt.formatDateTime(new Date(), "ddd dd MMM  hh:mm")
    property bool hasPlaybackChannel: root.player.currentChannel.id !== undefined
    property bool showPlaybackSpinner: (root.player.isLoading
            || root.player.isBuffering
            || root.player.timeshiftPreparing)
        && root.hasPlaybackChannel
    property bool showChannelSwitchBlackout: root.player.channelSwitchInProgress
        && root.hasPlaybackChannel
    property bool showChannelLoadError: root.player.channelLoadFailed
        && root.hasPlaybackChannel
        && !root.showPlaybackSpinner
    property bool hasSelectedChannel: root.guideState.selectedChannel.id !== undefined
    property bool hasChannelList: root.channelList.filteredCount > 0
    property bool hasAnyChannels: root.channelList.totalCount > 0
    property bool hasActiveProfile: root.app.activeProfileId.length > 0
    readonly property bool multiviewActive: root.multiView.layoutMode !== "off"
    readonly property bool multiviewGridActive: root.multiviewActive && root.multiView.layoutMode !== "pip"
    readonly property bool showVideoCanvas: root.hasPlaybackChannel || root.multiviewActive
    readonly property int multiviewVisibleSlots: {
        const tiles = root.multiView.tiles || []
        if (tiles.length > 0) {
            return tiles.length
        }
        return root.hasPlaybackChannel ? 1 : 0
    }
    readonly property string multiviewLayoutText: root.multiviewGridActive
        ? String(root.multiView.layoutColumns) + "x" + String(root.multiView.layoutRows)
        : (root.multiView.layoutMode === "pip" ? "PiP" : "Off")
    readonly property bool multiviewRetainedSelectionVisible: root.multiView.layoutMode === "off"
        && root.multiView.retainedSelectionActive
    readonly property var focusedMultiviewTileData: {
        if (!root.multiviewActive) {
            return null
        }
        return root.multiviewTileData(Number(root.multiView.focusedTileIndex || 0))
    }
    readonly property bool canStopPlaybackAction: {
        if (root.multiviewGridActive) {
            const focusedTile = root.focusedMultiviewTileData
            return !!focusedTile && !Boolean(focusedTile.isEmpty)
        }
        return root.hasPlaybackChannel
    }
    readonly property bool leftPickerOpen: root.groupPickerOpen || root.sourcePickerOpen || root.audioPickerOpen || root.subtitlePickerOpen
    property bool groupPickerOpen: false
    property string groupPickerSearchText: ""
    property string groupPickerHighlightedId: ""
    property bool sourcePickerOpen: false
    property string sourcePickerSearchText: ""
    property string sourcePickerHighlightedId: ""
    property string pendingSourcePickerProfileId: ""
    property string pendingSourcePickerRevealSource: ""
    property int sourcePickerModelRevision: 0
    property bool audioPickerOpen: false
    property int audioPickerHighlightedId: -1
    property var audioPickerRows: []
    property bool subtitlePickerOpen: false
    property int subtitlePickerHighlightedId: -1
    property var subtitlePickerRows: []
    property string leftPaneClosingMode: ""
    property bool leftPaneClosingToHidden: false
    readonly property string leftPaneDisplayMode: {
        if (root.groupPickerOpen) {
            return "group"
        }
        if (root.sourcePickerOpen) {
            return "source"
        }
        if (root.audioPickerOpen) {
            return "audio"
        }
        if (root.subtitlePickerOpen) {
            return "subtitle"
        }
        if (root.leftPaneClosingToHidden) {
            return root.leftPaneClosingMode
        }
        return "channels"
    }
    property int groupPickerModelRevision: 0
    readonly property var groupPickerVisibleRows: {
        const modelRevision = root.groupPickerModelRevision
        const total = root.liveGroups.totalCount
        const selectedCount = root.liveGroups.selectedCount
        const rows = []
        const query = root.groupPickerSearchText.trim().toLowerCase()
        const favouritesGroupId = "__favourites__"
        let selectedChannelCount = 0

        for (let row = 0; row < total; ++row) {
            const group = root.liveGroups.get(row)
            if (!group) continue
            if (!group.selected) {
                continue
            }
            if ((group.id || "") !== favouritesGroupId) {
                selectedChannelCount += Number(group.count || 0)
            }
            if (query.length > 0 && (group.name || "").toLowerCase().indexOf(query) < 0) {
                continue
            }
            rows.push({
                id: group.id,
                name: group.name,
                count: group.count,
                isAll: false
            })
        }

        if (query.length === 0 && selectedCount > 0) {
            rows.unshift({
                id: "",
                name: "--All Groups--",
                count: selectedChannelCount,
                isAll: true
            })
        }

        return rows
    }
    readonly property var sourcePickerVisibleRows: {
        const modelRevision = root.sourcePickerModelRevision
        const rows = []
        const query = root.sourcePickerSearchText.trim().toLowerCase()

        for (let row = 0;; ++row) {
            const profile = root.profiles.get(row)
            if (!profile || !profile.id) {
                break
            }
            if (query.length > 0 && (profile.name || "").toLowerCase().indexOf(query) < 0) {
                continue
            }
            rows.push({
                id: profile.id,
                name: profile.name,
                subtitle: profile.isActive ? profile.typeLabel + " • Active source" : profile.typeLabel,
                isActive: Boolean(profile.isActive)
            })
        }

        return rows
    }
    property bool searchFieldActive: searchField.activeFocus
        || groupPickerSearchField.activeFocus
        || sourcePickerSearchField.activeFocus
    property bool showHoverUi: root.shell.overlaysVisible || root.shell.activeOverlay !== "none" || root.leftPickerOpen
    readonly property bool topBarVisible: !root.shell.fullscreen && root.showHoverUi
    readonly property real topBarReservedHeight: root.mainWindow ? root.mainWindow.topBarReservedHeight : 0
    readonly property real topOverlayMargin: root.topBarReservedHeight + Theme.spacingL
    property bool topBarExternalHideLock: false
    onTopBarExternalHideLockChanged: updateAutoHide()
    onLeftPickerOpenChanged: clearLeftPaneHideIfSettled()
    property bool showShellChrome: root.shell.overlaysVisible
        && root.shell.activeOverlay === "none"
        && !root.leftPickerOpen
    readonly property bool chromeAnimationsRunning: leftChromeSlideAnimation.running
        || leftChromeOpacityAnimation.running
        || guideButtonSlideAnimation.running
        || guideButtonOpacityAnimation.running
        || rightChromeSlideAnimation.running
        || rightChromeOpacityAnimation.running
        || bottomChromeSlideAnimation.running
        || bottomChromeOpacityAnimation.running
    property bool guideOverlayMounted: false
    property bool guideOverlayOpen: false
    property bool guideOverlayClosing: false
    property bool guideCloseToVideoOnly: false
    property int committedChannelId: -1
    property bool hoverPreviewActive: false
    property bool previewPinned: false
    property bool suppressSelectionInteraction: false
    property var hoveredProgramData: ({})
    property Item hoverAnchorItem: null
    property bool hoverBubbleVisible: false
    property bool hoverBubbleRowActive: false
    property real hoverBubbleBridgeY: 0
    property real hoverBubbleBridgeHeight: 0
    property real hoverBubbleBridgeWidth: 0
    readonly property bool hoverBubbleActive: root.hoverBubbleRowActive
        || (hoverBubbleBridge.visible && hoverBubbleBridgeHover.hovered)
        || (epgHoverBubble.visible && epgHoverBubble.hovered)
    property int overlayAnimationDuration: Theme.transitionMs + 60
    property bool keyboardModeLocked: false
    property int keyboardNavigationCandidateCount: 0
    property string playerKeyboardFocusArea: "none"
    property bool multiviewSelectionMode: false
    property int multiviewSelectionIndex: 0
    property bool pendingEmptyPipAssignment: false
    readonly property bool pendingEmptyPipActive: {
        if (!root.pendingEmptyPipAssignment || root.multiView.layoutMode !== "pip") {
            return false
        }
        const secondaryTile = root.multiviewTileData(1)
        return !secondaryTile || Boolean(secondaryTile.isEmpty)
    }
    property string overlayInteractionSource: "none"
    property bool keyboardProgramBubbleActive: false
    property int pendingChannelViewRow: -1
    property int pendingChannelViewMode: ListView.Contain
    property int pendingHoverPreviewChannelId: -1
    property bool channelListScrollSettling: false
    readonly property bool channelListScrollActiveRaw: channelView.moving
        || channelView.dragging
        || channelView.flicking
        || channelViewScrollBar.pressed
    readonly property bool channelListScrollingActive: channelListScrollSettling
        || channelListScrollActiveRaw
    property int rightPaneSelectionFlatIndex: -1
    property string rightPaneSelectionKind: ""
    property int rightPaneUpcomingIndex: -1
    property var rightPaneProgramData: ({})
    property bool keyboardVolumeHudVisible: false
    property string keyboardVolumeHudIconFile: "volume-over-50.svg"
    property bool timeshiftTimelineHoverVisible: false
    property real timeshiftTimelineHoverFraction: 0
    property bool timeshiftBadgeForceBehindLive: false
    property string liveTimelineNoticeText: ""
    readonly property bool liveProgramSeekActive: root.player.playbackMode !== "catchup"
        && !root.player.timeshiftActive
        && root.liveCatchupState.visible
        && root.player.liveBufferActive
    readonly property bool liveProgramTimelineActive: root.liveProgramSeekActive
        && root.liveCatchupChannelCapable
    readonly property string transportTimelineMode: root.player.catchupTimelineActive
        ? "catchup"
        : (root.player.timeshiftActive ? "timeshift" : (root.liveProgramTimelineActive ? "liveProgram" : "none"))
    readonly property bool transportTimelineActive: root.transportTimelineMode !== "none"
    readonly property bool transportSeekActive: root.player.catchupTimelineActive
        || root.player.timeshiftActive
        || root.liveProgramSeekActive
    readonly property bool transportTimelineUsingCatchup: root.transportTimelineMode === "catchup"
    readonly property bool transportTimelineUsingTimeshift: root.transportTimelineMode === "timeshift"
    readonly property bool transportTimelineUsingLiveProgram: root.transportTimelineMode === "liveProgram"
    readonly property real transportBehindLiveSeconds: root.player.catchupTimelineActive
        ? Math.max(0, Number(root.player.catchupTimelineAvailableSeconds || 0) - Number(root.player.catchupTimelinePositionSeconds || 0))
        : (root.player.timeshiftActive
            ? root.timeshiftUiBehindLiveSeconds
            : (root.liveProgramSeekActive ? Math.max(0, Number(root.player.liveBufferBehindLiveSeconds || 0)) : 0))
    readonly property real transportLiveThresholdSeconds: 10.0
    readonly property real timeshiftUiBehindLiveSeconds: {
        const lag = Number(root.player.timeshiftBehindLiveSeconds || 0)
        if (!Number.isFinite(lag) || lag <= 0) {
            return 0
        }
        return lag
    }
    readonly property bool timeshiftUiAtLiveEdge: !root.transportSeekActive
        || root.transportBehindLiveSeconds <= root.transportLiveThresholdSeconds
    readonly property bool timeshiftBadgeShowBehindLive: root.transportSeekActive
        && (!root.timeshiftUiAtLiveEdge || root.timeshiftBadgeForceBehindLive)
    property bool debugBubbleEnabled: false
    property real debugBubbleX: Theme.spacingL
    property real debugBubbleY: Theme.spacingL
    property var debugBubbleData: ({})
    property var debugBufferSamples: []
    property int debugBufferSampleCapacity: 120
    property int debugBufferWriteIndex: 0
    property real debugBufferScaleMaxSeconds: 3.0
    property var debugBitrateSamples: []
    property int debugBitrateSampleCapacity: 120
    property int debugBitrateWriteIndex: 0
    property real debugBitrateScaleMaxKbps: 1000.0
    readonly property bool debugBubbleBlocked: root.shell.activeOverlay !== "none"
        || root.leftPickerOpen
        || root.guideOverlayMounted
    readonly property bool debugBubbleVisible: root.debugBubbleEnabled && !root.debugBubbleBlocked
    readonly property string debugBubbleCurrentResolution: Math.max(0, Math.round(root.width))
        + "x" + Math.max(0, Math.round(root.height))
    property string numericInputDigits: ""
    property string numericInputState: "none"
    readonly property string numericHudText: root.numericInputState === "error"
        ? "No channel"
        : root.numericInputDigits
    readonly property bool numericEntryContextActive: root.shell.activeOverlay === "none"
        && !root.leftPickerOpen
        && !searchField.activeFocus
        && !root.guideOverlayMounted
        && !root.guideOverlayOpen
    readonly property bool numericHudVisible: root.numericEntryContextActive
        && root.numericInputState !== "none"
        && root.numericHudText.length > 0
    property bool channelChangeBubbleVisible: false
    property bool playbackBubbleEpgLoading: root.app.epgCacheBootstrapPending
        && root.playbackNowNext.loading
    property bool browsePreviewActive: root.showShellChrome
        && root.hasSelectedChannel
        && (root.previewPinned
            || root.hoverPreviewActive
            || searchField.activeFocus
            || searchField.hovered
            || channelListHover.hovered
            || previewHoldTimer.running)
    property bool playbackEpgAvailable: (root.playbackNowNext.currentProgram.title || "").length > 0
        || (root.playbackNowNext.nextProgram.title || "").length > 0
        || root.playbackNowNext.upcomingPrograms.length > 0
    property bool browseEpgAvailable: (root.nowNext.currentProgram.title || "").length > 0
        || (root.nowNext.nextProgram.title || "").length > 0
        || root.nowNext.upcomingPrograms.length > 0
    readonly property int playbackChannelId: Number(root.player.currentChannel.id)
    readonly property bool selectedMatchesPlayback: !Number.isNaN(root.playbackChannelId)
        && root.playbackChannelId >= 0
        && root.channelList.selectedChannelId === root.playbackChannelId
    readonly property bool sideUsesBrowseModel: root.browsePreviewActive
        || (!root.hasPlaybackChannel && root.hasSelectedChannel)
        || (!root.playbackEpgAvailable && root.browseEpgAvailable
            && (root.selectedMatchesPlayback || (!root.hasPlaybackChannel && root.hasSelectedChannel)))
    property var sideNowNextModel: root.sideUsesBrowseModel
        ? root.nowNext
        : root.playbackNowNext
    property var liveCatchupState: ({ "visible": false, "enabled": false, "reason": "" })
    readonly property bool liveCatchupEligible: root.liveCatchupState.visible && root.liveCatchupState.enabled
    readonly property bool liveCatchupChannelCapable: {
        const channel = root.player.currentChannel || ({})
        return Boolean(channel.catchupSupported) && Number(channel.catchupWindowHours || 0) > 0
    }
    readonly property bool liveCatchupButtonVisible: root.liveCatchupChannelCapable
        && (root.liveCatchupState.visible || root.player.playbackMode === "catchup")
    readonly property bool liveCatchupButtonEnabled: root.liveCatchupEligible
        && root.player.playbackMode !== "catchup"
    readonly property bool sideHasCurrentProgram: (root.sideNowNextModel.currentProgram.title || "").length > 0
    readonly property bool sideHasNextProgram: (root.sideNowNextModel.nextProgram.title || "").length > 0
    readonly property bool sideHasUpcomingPrograms: root.sideNowNextModel.upcomingPrograms.length > 0
    readonly property bool sideHasAnyEpgData: root.sideHasCurrentProgram
        || root.sideHasNextProgram
        || root.sideHasUpcomingPrograms
    property real leftPanelWidth: root.shell.layoutBand === "compact" ? 308 : 340
    property real rightPanelWidth: root.leftPanelWidth
    property real bottomPanelWidth: Math.max(
        560,
        Math.min(
            root.shell.layoutBand === "compact" ? 720 : 780,
            parent.width - root.leftPanelWidth - root.rightPanelWidth - Theme.spacingXL * 3))
    readonly property bool settingsOverlayWide: root.shell.overlaySection === "sources"
        || (root.shell.activeOverlay === "settings" && settingsPage.prefersWideOverlay)
    readonly property real settingsOverlayTargetWidth: Math.min(
        parent ? parent.width : 0,
        root.settingsOverlayWide
            ? Math.min((parent ? parent.width : 0) * 0.9, root.shell.layoutBand === "compact" ? 1140 : 1320)
            : settingsPage.preferredNarrowOverlayWidth)
    readonly property string iconBasePath: "qrc:/resources/icons/"
    readonly property int rightPaneUpcomingRowHeight: 52

    function iconPath(fileName) {
        return root.iconBasePath + fileName
    }

    function volumeIconFile() {
        const level = Math.round(Number(root.player.volume || 0))
        if (root.player.muted || level <= 0) {
            return "volume-muted.svg"
        }
        if (level < 50) {
            return "volume-below-50.svg"
        }
        return "volume-over-50.svg"
    }

    function refreshLiveCatchupState() {
        root.liveCatchupState = root.app.catchupActionState(
            root.player.currentChannel || ({}),
            root.playbackNowNext.currentProgram || ({}))
    }

    function multiviewTileData(tileIndex) {
        const tiles = root.multiView.tiles || []
        if (tileIndex < 0 || tileIndex >= tiles.length) {
            return null
        }
        return tiles[tileIndex]
    }

    function selectedMultiviewShortcutChannelId() {
        if (root.shell.activeOverlay === "guide" || guideOverlayMounted) {
            return root.guideState.selectedChannelId >= 0 ? root.guideState.selectedChannelId : -1
        }

        const leftPaneSelectionActive = root.playerKeyboardFocusArea === "leftPane" || root.browsePreviewActive
        if (!leftPaneSelectionActive) {
            return -1
        }

        const selectedChannelId = root.channelList.selectedChannelId
        if (selectedChannelId < 0 || root.channelList.rowForChannelId(selectedChannelId) < 0) {
            return -1
        }
        return selectedChannelId
    }

    function handlePictureInPictureShortcut() {
        const selectedChannelId = root.selectedMultiviewShortcutChannelId()
        const guideSelectionActive = root.shell.activeOverlay === "guide" || guideOverlayMounted
        const pipWasActive = root.multiView.layoutMode === "pip"
        const changed = root.multiView.togglePictureInPicture(selectedChannelId)
        if (!changed) {
            return false
        }
        if (!pipWasActive && selectedChannelId < 0 && root.multiView.layoutMode === "pip") {
            root.pendingEmptyPipAssignment = true
            if (guideSelectionActive) {
                closeGuideOverlay(false)
            }
            root.closeTransientPlayerChrome()
            if (root.channelList.rowForChannelId(root.channelList.selectedChannelId) < 0) {
                root.channelList.selectAt(0)
            }
            root.previewPinned = true
            root.scheduleSelectedChannelVisible(ListView.Contain)
            root.shell.overlaysVisible = false
            interactionFocusTarget.forceActiveFocus()
            root.setPlayerKeyboardFocusArea("leftPane")
            return true
        }
        if (changed && guideSelectionActive && selectedChannelId >= 0) {
            closeGuideOverlay(false)
        }
        return changed
    }

    function handleStopPlaybackAction() {
        const handledByRetainedMultiview = root.multiView.stopRetainedPromotedAndRestoreGrid()
        if (!handledByRetainedMultiview) {
            root.player.stop()
        }
        root.revealUi("pointer")
    }

    function syncPendingEmptyPipAssignment() {
        if (!root.pendingEmptyPipAssignment) {
            return
        }
        if (root.multiView.layoutMode !== "pip") {
            root.pendingEmptyPipAssignment = false
            return
        }
        const secondaryTile = root.multiviewTileData(1)
        if (secondaryTile && !Boolean(secondaryTile.isEmpty)) {
            root.pendingEmptyPipAssignment = false
            root.multiView.focusTile(0)
        }
    }

    function handleMultiviewShortcut(key, ctrlPressed, shiftPressed, altPressed) {
        if (!ctrlPressed) {
            return null
        }
        if (key === Qt.Key_O) {
            if (altPressed) {
                return root.multiView.fullPromoteAndExit()
            }
            if (shiftPressed) {
                return root.setMultiviewSelectionMode(!root.multiviewSelectionMode)
            }
            return root.multiView.toggleGrid()
        }
        if (key === Qt.Key_P) {
            if (shiftPressed) {
                return root.multiView.swapPrimaryWithPictureInPicture()
            }
            return root.handlePictureInPictureShortcut()
        }
        return null
    }

    function multiviewTileRect(tileIndex) {
        const pipMargin = root.shell.layoutBand === "compact" ? 10 : 14
        const fullWidth = Math.max(1, root.width)
        const fullHeight = Math.max(1, root.height)

        if (!root.multiviewActive || root.multiView.layoutMode === "off" || root.multiviewVisibleSlots <= 1) {
            return { x: 0, y: 0, width: fullWidth, height: fullHeight }
        }

        if (root.multiView.layoutMode === "pip") {
            if (tileIndex === 0) {
                return { x: 0, y: 0, width: fullWidth, height: fullHeight }
            }

            const pipVideoInset = 2
            const pipWidth = Math.min(
                Math.max(240, Math.round(fullWidth * 0.28)),
                Math.max(240, Math.round(fullWidth * 0.42)))
            const pipHeight = Math.min(
                Math.max(136, Math.round(pipWidth * 9 / 16)),
                Math.round(fullHeight * 0.34))
            const pipX = Math.max(pipMargin, fullWidth - pipWidth - pipMargin)
            const pipY = Math.max(pipMargin, fullHeight - pipHeight - pipMargin)
            return {
                // Keep the proven-stable PiP video surface size while removing the visible frame.
                x: pipX + pipVideoInset,
                y: pipY + pipVideoInset,
                width: Math.max(1, pipWidth - (pipVideoInset * 2)),
                height: Math.max(1, pipHeight - (pipVideoInset * 2))
            }
        }

        const columns = Math.max(1, Number(root.multiView.layoutColumns || 1))
        const rows = Math.max(1, Number(root.multiView.layoutRows || 1))
        const clampedIndex = Math.max(0, Math.min(root.multiviewVisibleSlots - 1, tileIndex))
        const row = Math.floor(clampedIndex / columns)
        const column = clampedIndex % columns
        const left = Math.floor((column * fullWidth) / columns)
        const right = Math.floor(((column + 1) * fullWidth) / columns)
        const top = Math.floor((row * fullHeight) / rows)
        const bottom = Math.floor(((row + 1) * fullHeight) / rows)
        return {
            x: left,
            y: top,
            width: Math.max(1, right - left),
            height: Math.max(1, bottom - top)
        }
    }

    Connections {
        target: uiTestCaptureController
        enabled: uiTestCaptureController && uiTestCaptureController.enabled

        function onCaptureRequested(outputPath) {
            if (!outputPath || outputPath.length === 0) {
                uiTestCaptureController.notifyCaptureSaved("", false)
                return
            }

            root.grabToImage(function(result) {
                const ok = result.saveToFile(outputPath)
                uiTestCaptureController.notifyCaptureSaved(outputPath, ok)
            })
        }
    }

    function multiviewPlaceholderText(tileData) {
        if (!tileData || !Boolean(tileData.isEmpty)) {
            return ""
        }
        if (root.multiView.layoutMode === "pip" && !Boolean(tileData.isPrimary)) {
            return ""
        }
        if (Boolean(tileData.isPrimary)) {
            return "Select a channel"
        }
        return "Ctrl+Shift+O, arrows, Enter"
    }

    function multiviewSelectionColumns() {
        if (root.multiviewGridActive) {
            return Math.max(1, Number(root.multiView.layoutColumns || 1))
        }
        if (root.multiviewActive && root.multiView.layoutMode === "pip" && root.multiviewVisibleSlots > 1) {
            return 2
        }
        return 1
    }

    function setMultiviewSelectionMode(enabled) {
        const next = Boolean(enabled) && root.multiviewActive && root.multiviewVisibleSlots > 0
        if (next) {
            if (root.shell.activeOverlay !== "none"
                || root.leftPickerOpen
                || searchField.activeFocus) {
                return false
            }
            clearKeyboardNavigationCandidate()
            root.keyboardModeLocked = false
            root.setPlayerKeyboardFocusArea("none")
            root.overlayInteractionSource = "none"
            root.shell.overlaysVisible = false
            interactionFocusTarget.forceActiveFocus()
            const focused = Number(root.multiView.focusedTileIndex || 0)
            root.multiviewSelectionMode = true
            root.multiviewSelectionIndex = Math.max(0, Math.min(root.multiviewVisibleSlots - 1, focused))
            root.updateAutoHide()
            return true
        }

        root.multiviewSelectionMode = next
        root.multiviewSelectionIndex = 0
        clearKeyboardNavigationCandidate()
        root.keyboardModeLocked = false
        root.setPlayerKeyboardFocusArea("none")
        root.overlayInteractionSource = "none"
        interactionFocusTarget.forceActiveFocus()
        root.updateAutoHide()
        return true
    }

    function moveMultiviewSelection(deltaRow, deltaColumn) {
        if (!root.multiviewSelectionMode || root.multiviewVisibleSlots <= 0) {
            return false
        }
        const count = root.multiviewVisibleSlots
        const columns = Math.max(1, root.multiviewSelectionColumns())
        const rows = Math.max(1, Math.ceil(count / columns))
        let currentIndex = Math.max(0, Math.min(count - 1, root.multiviewSelectionIndex))
        let row = Math.floor(currentIndex / columns)
        let column = currentIndex % columns
        for (let attempt = 0; attempt < (rows * columns); ++attempt) {
            row = (row + deltaRow + rows) % rows
            column = (column + deltaColumn + columns) % columns
            const candidate = (row * columns) + column
            if (candidate >= 0 && candidate < count) {
                root.multiviewSelectionIndex = candidate
                return true
            }
        }
        return false
    }

    function commitMultiviewSelection() {
        if (!root.multiviewSelectionMode) {
            return false
        }
        const count = root.multiviewVisibleSlots
        if (count <= 0) {
            root.setMultiviewSelectionMode(false)
            return false
        }
        const target = Math.max(0, Math.min(count - 1, root.multiviewSelectionIndex))
        root.multiView.focusTile(target)
        root.setMultiviewSelectionMode(false)
        return true
    }

    function multiviewStatusText(tileData) {
        if (!tileData) {
            return ""
        }
        if (Boolean(tileData.isEmpty)) {
            return root.multiviewPlaceholderText(tileData)
        }
        const state = String(tileData.playerState || "").trim()
        if (state.length === 0 || state === "playing" || state === "ready") {
            return ""
        }
        return state.charAt(0).toUpperCase() + state.slice(1)
    }

    function debugFieldValue(key) {
        const value = root.debugBubbleData[key]
        if (value === undefined || value === null) {
            return "N/A"
        }

        const text = String(value).trim()
        return text.length > 0 ? text : "N/A"
    }

    function formatTimeshiftClock(epochMs) {
        const numeric = Number(epochMs)
        if (!Number.isFinite(numeric) || numeric <= 0) {
            return "--:--"
        }
        return Qt.formatTime(new Date(numeric), "hh:mm")
    }

    function formatTimeshiftLag(seconds) {
        const numeric = Math.max(0, Math.round(Number(seconds || 0)))
        const mins = Math.floor(numeric / 60)
        const secs = numeric % 60
        return "-" + String(mins).padStart(2, "0") + ":" + String(secs).padStart(2, "0")
    }

    function markTimeshiftBadgeLive() {
        root.timeshiftBadgeForceBehindLive = false
    }

    function markTimeshiftBadgeBehindLive() {
        if (!root.transportSeekActive) {
            return
        }
        root.timeshiftBadgeForceBehindLive = true
    }

    function seekTimeshiftRelativeWithBadge(seconds) {
        if (!root.transportSeekActive) {
            return false
        }
        const delta = Number(seconds)
        if (!Number.isFinite(delta) || delta === 0) {
            return true
        }
        if (delta < 0) {
            root.markTimeshiftBadgeBehindLive()
        } else {
            const lag = Number(root.transportBehindLiveSeconds || 0)
            if (Number.isFinite(lag) && lag <= delta + 0.25) {
                return root.jumpToLiveEdgeWithBadge()
            }
        }
        root.player.seekTimeshiftRelative(delta)
        return true
    }

    function seekTimeshiftToFractionWithBadge(fraction) {
        if (!root.transportTimelineActive) {
            return false
        }
        const clamped = Math.max(0, Math.min(1, Number(fraction)))
        if (root.transportTimelineUsingLiveProgram) {
            const program = root.playbackNowNext.currentProgram || ({})
            const startMs = Date.parse(String(program.start || ""))
            const stopMs = Date.parse(String(program.stop || ""))
            if (!Number.isFinite(startMs) || !Number.isFinite(stopMs) || stopMs <= startMs) {
                return false
            }
            const nowMs = Math.min(Date.now(), stopMs)
            const programmeAvailableSeconds = Math.max(0, (nowMs - startMs) / 1000.0)
            const targetProgrammeSeconds = clamped * programmeAvailableSeconds
            const minElapsedSeconds = Math.max(0, Number(root.app.catchupMinElapsedSeconds || 0))
            if (programmeAvailableSeconds > minElapsedSeconds) {
                const targetSecondsBehindLive = Math.max(0, programmeAvailableSeconds - targetProgrammeSeconds)
                if (targetSecondsBehindLive < minElapsedSeconds) {
                    root.showLiveTimelineNotice("Timeline selection is locked within 10 minutes of live.")
                    return true
                }
            }
            const localAvailableSeconds = Math.max(0, Number(root.player.liveBufferAvailableSeconds || 0))
            const localStartSeconds = Math.max(0, programmeAvailableSeconds - localAvailableSeconds)
            if (targetProgrammeSeconds + 0.05 >= localStartSeconds) {
                const localOffset = Math.max(0, targetProgrammeSeconds - localStartSeconds)
                const localFraction = localAvailableSeconds > 0
                    ? Math.max(0, Math.min(1, localOffset / localAvailableSeconds))
                    : 1
                if (localFraction >= 0.995) {
                    return root.jumpToLiveEdgeWithBadge()
                }
                root.markTimeshiftBadgeBehindLive()
                root.player.seekTimeshiftToFraction(localFraction)
                return true
            }
            root.markTimeshiftBadgeBehindLive()
            if (!root.liveCatchupEligible) {
                root.showLiveTimelineNotice(String(root.liveCatchupState.reason || "Catch-up is unavailable."))
                return true
            }
            root.app.playCatchupAtOffset(
                root.player.currentChannel || ({}),
                root.playbackNowNext.currentProgram || ({}),
                targetProgrammeSeconds)
            return true
        }
        if (clamped >= 0.995) {
            return root.jumpToLiveEdgeWithBadge()
        }
        if (clamped < 0.999) {
            root.markTimeshiftBadgeBehindLive()
        }
        root.player.seekTimeshiftToFraction(clamped)
        return true
    }

    function jumpToLiveEdgeWithBadge() {
        if (!root.transportSeekActive) {
            return false
        }
        root.markTimeshiftBadgeLive()
        root.player.jumpToLiveEdge()
        return true
    }

    function togglePauseWithBadge() {
        if (root.transportSeekActive && root.hasPlaybackChannel && root.player.isPlaying) {
            root.markTimeshiftBadgeBehindLive()
        }
        root.player.togglePause()
    }

    function showLiveTimelineNotice(message) {
        const text = String(message || "").trim()
        if (!text.length) {
            return
        }
        root.liveTimelineNoticeText = text
        liveTimelineNoticeTimer.restart()
    }

    function timeshiftHoverClockText(fraction) {
        let startMs = 0
        let endMs = 0
        if (root.transportTimelineUsingCatchup) {
            startMs = Number(root.player.catchupTimelineStartEpochMs || 0)
            endMs = Number(root.player.catchupTimelineAvailableEdgeEpochMs || 0)
        } else if (root.transportTimelineUsingTimeshift) {
            startMs = Number(root.player.timeshiftWindowStartEpochMs || 0)
            endMs = Number(root.player.timeshiftLiveEdgeEpochMs || 0)
        } else if (root.transportTimelineUsingLiveProgram) {
            const program = root.playbackNowNext.currentProgram || ({})
            const start = Date.parse(String(program.start || ""))
            const stop = Date.parse(String(program.stop || ""))
            const nowMs = Date.now()
            startMs = Number.isFinite(start) ? start : 0
            endMs = Number.isFinite(stop) ? Math.min(nowMs, stop) : nowMs
        }
        if (!Number.isFinite(startMs) || !Number.isFinite(endMs) || endMs <= startMs) {
            return "--:--"
        }
        const clamped = Math.max(0, Math.min(1, Number(fraction || 0)))
        return root.formatTimeshiftClock(startMs + (endMs - startMs) * clamped)
    }

    function resetDebugBufferChart() {
        root.debugBufferSamples = []
        root.debugBufferWriteIndex = 0
        root.debugBufferScaleMaxSeconds = 3.0
        if (debugBufferChart.available) {
            debugBufferChart.requestPaint()
        }
    }

    function appendDebugBufferSample(bufferDurationSeconds, minBufferNeededSeconds) {
        const capacity = Math.max(8, Math.floor(root.debugBufferSampleCapacity))
        const samples = root.debugBufferSamples.slice(0, capacity)
        while (samples.length < capacity) {
            samples.push(null)
        }

        const normalizedSample = Number.isFinite(bufferDurationSeconds) && bufferDurationSeconds >= 0
            ? bufferDurationSeconds
            : null
        samples[root.debugBufferWriteIndex] = normalizedSample

        let nextWriteIndex = root.debugBufferWriteIndex + 1
        if (nextWriteIndex >= capacity) {
            nextWriteIndex = 0
        }

        const targetSeconds = Number.isFinite(minBufferNeededSeconds) && minBufferNeededSeconds > 0
            ? minBufferNeededSeconds
            : 0.0
        let observedMax = 0.0
        for (let index = 0; index < capacity; ++index) {
            const sample = Number(samples[index])
            if (Number.isFinite(sample) && sample > observedMax) {
                observedMax = sample
            }
        }
        const nextScale = Math.max(1.0, targetSeconds, observedMax)

        root.debugBufferSamples = samples
        root.debugBufferWriteIndex = nextWriteIndex
        root.debugBufferScaleMaxSeconds = nextScale
        if (debugBufferChart.available) {
            debugBufferChart.requestPaint()
        }
    }

    function resetDebugBitrateChart() {
        root.debugBitrateSamples = []
        root.debugBitrateWriteIndex = 0
        root.debugBitrateScaleMaxKbps = 1000.0
        if (debugBitrateChart.available) {
            debugBitrateChart.requestPaint()
        }
    }

    function appendDebugBitrateSample(bitrateKbps) {
        const capacity = Math.max(8, Math.floor(root.debugBitrateSampleCapacity))
        const samples = root.debugBitrateSamples.slice(0, capacity)
        while (samples.length < capacity) {
            samples.push(null)
        }

        const normalizedSample = Number.isFinite(bitrateKbps) && bitrateKbps >= 0
            ? bitrateKbps
            : null
        samples[root.debugBitrateWriteIndex] = normalizedSample

        let nextWriteIndex = root.debugBitrateWriteIndex + 1
        if (nextWriteIndex >= capacity) {
            nextWriteIndex = 0
        }

        let observedMax = 0.0
        for (let index = 0; index < capacity; ++index) {
            const sample = Number(samples[index])
            if (Number.isFinite(sample) && sample > observedMax) {
                observedMax = sample
            }
        }
        const nextScale = Math.max(1000.0, observedMax)

        root.debugBitrateSamples = samples
        root.debugBitrateWriteIndex = nextWriteIndex
        root.debugBitrateScaleMaxKbps = nextScale
        if (debugBitrateChart.available) {
            debugBitrateChart.requestPaint()
        }
    }

    function channelDisplayNumber(channelData) {
        if (!channelData || channelData.id === undefined) {
            return ""
        }

        const sortOrder = Number(channelData.sortOrder || 0)
        const rawValue = sortOrder > 0 ? sortOrder : Number(channelData.id || 0)
        if (!Number.isFinite(rawValue) || rawValue <= 0) {
            return ""
        }

        return String(Math.floor(rawValue)).padStart(3, "0") + "."
    }

    function programEpisodeTitle(programData) {
        if (!programData) {
            return ""
        }
        return String(programData.subTitle || "").trim()
    }

    function sideDvrChannelData() {
        if (root.sideUsesBrowseModel
            && root.guideState.selectedChannel
            && root.guideState.selectedChannel.id !== undefined) {
            return root.guideState.selectedChannel
        }
        if (root.player.currentChannel && root.player.currentChannel.id !== undefined) {
            return root.player.currentChannel
        }
        if (root.guideState.selectedChannel && root.guideState.selectedChannel.id !== undefined) {
            return root.guideState.selectedChannel
        }
        return ({})
    }

    function isProgramDvrMarked(programData, channelData) {
        const _scheduleCount = root.dvr.scheduledCount
        if (!programData || !channelData) {
            return false
        }
        if (channelData.id === undefined || channelData.profileId === undefined) {
            return false
        }
        if ((programData.start || "").length === 0 || (programData.stop || "").length === 0) {
            return false
        }
        return root.dvr.isProgramScheduled(channelData, programData)
    }

    function toggleManualRecordingShortcut() {
        if (root.player.isRecording) {
            root.player.stopRecording()
            return true
        }
        if (!root.player.isPlaying) {
            return true
        }
        const progTitle = root.playbackNowNext.currentProgram.title || ""
        const chanName = root.player.currentChannel.name || ""
        root.player.startRecording(root.settings.recordingsDirectory, chanName, progTitle)
        return true
    }

    function toggleHoveredRightPaneDvrSchedule() {
        if (!root.hoverBubbleRowActive) {
            return false
        }
        const program = root.hoveredProgramData || ({})
        if ((program.start || "").length === 0 || (program.stop || "").length === 0) {
            return false
        }
        const channel = root.sideDvrChannelData()
        if (channel.id === undefined
            || (channel.profileId || "").length === 0
            || (channel.streamUrl || "").length === 0) {
            return false
        }
        return root.dvr.toggleProgramSchedule(channel, program)
    }

    function toggleSelectedRightPaneDvrSchedule() {
        if (root.playerKeyboardFocusArea !== "rightPane") {
            return false
        }
        const program = root.rightPaneProgramData || ({})
        if ((program.start || "").length === 0 || (program.stop || "").length === 0) {
            return false
        }
        const channel = root.sideDvrChannelData()
        if (channel.id === undefined
            || (channel.profileId || "").length === 0
            || (channel.streamUrl || "").length === 0) {
            return false
        }
        return root.dvr.toggleProgramSchedule(channel, program)
    }

    function togglePreferredRightPaneDvrSchedule() {
        const preferKeyboardSelection = root.playerKeyboardFocusArea === "rightPane"
            && (root.overlayInteractionSource === "keyboard"
                || root.keyboardModeLocked
                || root.keyboardProgramBubbleActive)
        if (preferKeyboardSelection && root.toggleSelectedRightPaneDvrSchedule()) {
            return true
        }
        if (root.toggleHoveredRightPaneDvrSchedule()) {
            return true
        }
        return root.toggleSelectedRightPaneDvrSchedule()
    }

    function handleCtrlRShortcut() {
        const preferKeyboardSelection = root.overlayInteractionSource === "keyboard"
            || root.keyboardModeLocked
            || root.playerKeyboardFocusArea !== "none"
        if (root.shell.activeOverlay === "guide" || guideOverlayMounted) {
            if (guidePage.togglePreferredProgramDvrSchedule(preferKeyboardSelection)) {
                return true
            }
            return root.toggleManualRecordingShortcut()
        }
        if (root.togglePreferredRightPaneDvrSchedule()) {
            return true
        }
        return root.toggleManualRecordingShortcut()
    }

    function digitForKey(key) {
        switch (key) {
        case Qt.Key_0:
            return "0"
        case Qt.Key_1:
            return "1"
        case Qt.Key_2:
            return "2"
        case Qt.Key_3:
            return "3"
        case Qt.Key_4:
            return "4"
        case Qt.Key_5:
            return "5"
        case Qt.Key_6:
            return "6"
        case Qt.Key_7:
            return "7"
        case Qt.Key_8:
            return "8"
        case Qt.Key_9:
            return "9"
        default:
            return ""
        }
    }

    function shouldClearNumericEntryForKey(key, ctrlPressed) {
        if (ctrlPressed) {
            return key === Qt.Key_F
                || key === Qt.Key_G
                || key === Qt.Key_S
                || key === Qt.Key_Up
                || key === Qt.Key_Down
        }

        return key === Qt.Key_Space
            || key === Qt.Key_F
            || key === Qt.Key_M
            || key === Qt.Key_Comma
            || key === Qt.Key_Period
            || key === Qt.Key_Tab
            || key === Qt.Key_Left
            || key === Qt.Key_Right
            || key === Qt.Key_Up
            || key === Qt.Key_Down
            || key === Qt.Key_Return
            || key === Qt.Key_Enter
    }

    function clearNumericEntry() {
        numericInputDigits = ""
        numericInputState = "none"
        numericCommitTimer.stop()
        numericFailureTimer.stop()
    }

    function appendNumericInputDigit(digit) {
        if (!digit || digit.length !== 1 || !root.numericEntryContextActive) {
            return false
        }

        if (root.numericInputState === "error") {
            root.numericInputDigits = ""
        }
        root.numericInputState = "digits"
        root.numericInputDigits += digit
        numericFailureTimer.stop()
        numericCommitTimer.restart()
        return true
    }

    function commitNumericEntry() {
        if (root.numericInputState !== "digits" || root.numericInputDigits.length === 0) {
            return
        }

        const channelNumber = Number(root.numericInputDigits)
        if (Number.isNaN(channelNumber) || channelNumber <= 0) {
            root.numericInputDigits = ""
            root.numericInputState = "error"
            numericFailureTimer.restart()
            return
        }

        if (root.channelList.activateByDisplayNumber(Math.floor(channelNumber))) {
            root.clearNumericEntry()
            return
        }

        root.numericInputDigits = ""
        root.numericInputState = "error"
        numericFailureTimer.restart()
    }

    function overlayHideLocked() {
        const hoverLocksEnabled = root.overlayInteractionSource !== "keyboard" || root.keyboardModeLocked
        return root.keyboardModeLocked
            || searchField.activeFocus
            || groupPickerSearchField.activeFocus
            || sourcePickerSearchField.activeFocus
            || root.topBarExternalHideLock
            || (hoverLocksEnabled
                && (leftChromeHover.hovered
                    || rightChromeHover.hovered
                    || bottomChromeHover.hovered
                    || guideButtonHover.hovered
                    || volumeControl.hoverActive
                    || root.hoverBubbleActive))
    }

    function clearKeyboardNavigationCandidate() {
        keyboardNavigationCandidateCount = 0
        keyboardNavigationCandidateTimer.stop()
    }

    function noteKeyboardNavigationKey() {
        if (root.shell.activeOverlay !== "none") {
            return
        }

        revealUi("keyboard")
        if (keyboardModeLocked) {
            return
        }

        keyboardNavigationCandidateCount += 1
        if (keyboardNavigationCandidateCount >= 2) {
            keyboardModeLocked = true
            clearKeyboardNavigationCandidate()
            updateAutoHide()
            return
        }

        keyboardNavigationCandidateTimer.restart()
    }

    function updateAutoHide() {
        if (!root.settings.overlayAutoHide
            || root.shell.activeOverlay !== "none"
            || root.leftPickerOpen
            || root.overlayHideLocked()
            || !root.shell.overlaysVisible) {
            overlayTimer.stop()
            return
        }
        overlayTimer.restart()
    }

    function revealUi(source) {
        if (root.pendingEmptyPipActive) {
            return
        }
        if (source === "keyboard" || source === "pointer") {
            root.overlayInteractionSource = source
        } else if (root.overlayInteractionSource === "none") {
            root.overlayInteractionSource = "pointer"
        }
        root.shell.showOverlaysTemporary()
        updateAutoHide()
    }

    function setPlayerKeyboardFocusArea(area) {
        const nextArea = area && area.length > 0 ? area : "none"
        if (nextArea !== "rightPane" && root.keyboardProgramBubbleActive) {
            root.clearKeyboardProgramBubble()
        }
        playerKeyboardFocusArea = nextArea
    }

    function noteBrowseInteraction() {
        revealUi("pointer")
        previewHoldTimer.restart()
    }

    function previewChannel(channelId) {
        if (channelId < 0) {
            return
        }
        hoverPreviewActive = true
        if (root.channelList.selectedChannelId !== channelId) {
            root.channelList.selectById(channelId)
        }
    }

    function queueHoverPreviewChannel(channelId) {
        if (channelId < 0) {
            return
        }
        pendingHoverPreviewChannelId = channelId
        hoverPreviewDebounceTimer.restart()
    }

    function flushQueuedHoverPreviewChannel() {
        if (pendingHoverPreviewChannelId < 0 || root.channelListScrollingActive) {
            return
        }
        const channelId = pendingHoverPreviewChannelId
        pendingHoverPreviewChannelId = -1
        root.previewChannel(channelId)
    }

    function updateChannelListScrollState() {
        if (root.channelListScrollActiveRaw) {
            root.channelListScrollSettling = true
        }
        channelListScrollSettleTimer.restart()
    }

    function commitChannelSelection(channelId) {
        if (channelId < 0) {
            return
        }
        hoverPreviewActive = false
        previewPinned = true
        committedChannelId = channelId
        if (root.channelList.selectedChannelId !== channelId) {
            root.channelList.selectById(channelId)
        }
    }

    function clearHoverPreview() {
        if (!hoverPreviewActive) {
            return
        }
        hoverPreviewActive = false
        if (committedChannelId >= 0 && root.channelList.selectedChannelId !== committedChannelId) {
            root.channelList.selectById(committedChannelId)
        }
    }

    function showProgramHoverBubble(programData, anchorItem) {
        if (!programData || !anchorItem) {
            return
        }
        keyboardProgramBubbleActive = false
        hoveredProgramData = programData
        hoverAnchorItem = anchorItem
        hoverBubbleRowActive = true
        hoverBubbleVisible = true
        hoverBubbleHideTimer.stop()
        Qt.callLater(function() {
            root.repositionProgramHoverBubble()
        })
    }

    function releaseProgramHoverBubble(anchorItem) {
        if (anchorItem && hoverAnchorItem && hoverAnchorItem !== anchorItem) {
            return
        }
        hoverBubbleRowActive = false
        if (!epgHoverBubble.hovered) {
            hoverBubbleHideTimer.restart()
        }
    }

    function hideProgramHoverBubble() {
        hoverBubbleHideTimer.stop()
        keyboardProgramBubbleActive = false
        hoverBubbleRowActive = false
        hoverBubbleVisible = false
        hoverAnchorItem = null
        hoveredProgramData = ({})
        hoverBubbleBridgeWidth = 0
        hoverBubbleBridgeHeight = 0
    }

    function showKeyboardProgramBubble(programData) {
        if (!programData) {
            return
        }

        keyboardProgramBubbleActive = true
        hoveredProgramData = programData
        hoverAnchorItem = keyboardBubbleAnchor
        hoverBubbleRowActive = false
        hoverBubbleVisible = true
        hoverBubbleHideTimer.stop()
        Qt.callLater(function() {
            root.repositionProgramHoverBubble()
        })
    }

    function clearKeyboardProgramBubble() {
        if (!keyboardProgramBubbleActive) {
            return
        }

        keyboardProgramBubbleActive = false
        if (!hoverBubbleRowActive && !epgHoverBubble.hovered) {
            hideProgramHoverBubble()
        }
    }

    function repositionProgramHoverBubble() {
        if (!hoverBubbleVisible || !hoverAnchorItem || !epgHoverBubble.visible) {
            return
        }

        const anchorPosition = hoverAnchorItem.mapToItem(root, 0, 0)
        const anchorLeft = anchorPosition.x
        const anchorCenterY = anchorPosition.y + (hoverAnchorItem.height / 2)
        epgHoverBubble.x = rightChrome.x - epgHoverBubble.width
        const minY = root.topBarReservedHeight + Theme.spacingS
        const maxY = Math.max(minY, root.height - epgHoverBubble.height - 8)
        epgHoverBubble.y = Math.max(minY, Math.min(anchorCenterY - (epgHoverBubble.height / 2), maxY))
        hoverBubbleBridgeY = anchorPosition.y
        hoverBubbleBridgeHeight = Math.max(1, hoverAnchorItem.height)
        hoverBubbleBridgeWidth = Math.max(0, anchorLeft - rightChrome.x)
    }

    function toggleFullscreen() {
        if (!root.mainWindow) {
            return
        }

        if (root.mainWindow.visibility === Window.FullScreen) {
            root.mainWindow.showNormal()
        } else {
            root.mainWindow.showFullScreen()
        }
    }

    function playSelection(revealSource) {
        if (root.guideState.selectedChannelId >= 0) {
            root.activateChannelById(root.guideState.selectedChannelId)
        }
        revealUi(revealSource)
    }

    function showChannelChangeBubble() {
        if (root.multiviewActive || !root.player.channelSwitchInProgress) {
            return
        }
        channelChangeBubbleVisible = true
        channelChangeBubbleTimer.restart()
    }

    function activateChannelById(channelId) {
        if (channelId < 0) {
            return false
        }

        return root.channelList.activateById(channelId)
    }

    function toggleSelectedChannelFavourite() {
        const selectedChannelId = root.channelList.selectedChannelId
        if (selectedChannelId < 0 || root.channelList.rowForChannelId(selectedChannelId) < 0) {
            return false
        }

        return root.channelList.toggleFavorite(selectedChannelId)
    }

    function activateChannelRelative(delta, suppressBrowseReveal) {
        if (suppressBrowseReveal) {
            root.suppressSelectionInteraction = true
        }

        const activated = root.channelList.activateRelative(delta)
        if (suppressBrowseReveal) {
            Qt.callLater(function() {
                root.suppressSelectionInteraction = false
            })
        }
        return activated
    }

    function rightPaneProgramActive(kind, upcomingIndex, anchorItem) {
        if (root.hoverBubbleVisible && root.hoverAnchorItem === anchorItem) {
            return true
        }
        if (playerKeyboardFocusArea !== "rightPane" || rightPaneSelectionKind !== kind) {
            return false
        }
        return kind !== "upcoming" || rightPaneUpcomingIndex === upcomingIndex
    }

    function rightPaneEntries() {
        const entries = []
        if ((root.sideNowNextModel.currentProgram.title || "").length > 0) {
            entries.push({
                kind: "now",
                listIndex: -1,
                program: root.sideNowNextModel.currentProgram
            })
        }
        if ((root.sideNowNextModel.nextProgram.title || "").length > 0) {
            entries.push({
                kind: "next",
                listIndex: -1,
                program: root.sideNowNextModel.nextProgram
            })
        }
        for (let index = 0; index < root.sideNowNextModel.upcomingPrograms.length; ++index) {
            entries.push({
                kind: "upcoming",
                listIndex: index,
                program: root.sideNowNextModel.upcomingPrograms[index]
            })
        }
        return entries
    }

    function upcomingRowFullyVisible(rowIndex) {
        if (rowIndex < 0 || !root.sideHasUpcomingPrograms) {
            return false
        }
        const rowHeight = root.rightPaneUpcomingRowHeight
        const rowTop = rowIndex * rowHeight
        const rowBottom = rowTop + rowHeight
        const visibleTop = upcomingProgramsView.contentY
        const visibleBottom = visibleTop + upcomingProgramsView.height
        return rowTop >= visibleTop && rowBottom <= visibleBottom
    }

    function updateKeyboardBubbleAnchor(entry) {
        if (!entry) {
            return
        }

        let anchorX = rightChrome.x
        let anchorY = rightChrome.y
        let anchorWidth = rightChrome.width
        let anchorHeight = 1

        if (entry.kind === "now") {
            const position = nowProgramCard.mapToItem(root, 0, 0)
            anchorX = position.x
            anchorY = position.y
            anchorWidth = nowProgramCard.width
            anchorHeight = nowProgramCard.height
        } else if (entry.kind === "next") {
            const position = nextProgramCard.mapToItem(root, 0, 0)
            anchorX = position.x
            anchorY = position.y
            anchorWidth = nextProgramCard.width
            anchorHeight = nextProgramCard.height
        } else {
            if (!root.upcomingRowFullyVisible(entry.listIndex)) {
                upcomingProgramsView.positionViewAtIndex(entry.listIndex, ListView.Contain)
            }
            const position = upcomingProgramsView.mapToItem(root, 0, 0)
            anchorX = rightChrome.x
            anchorY = position.y + entry.listIndex * root.rightPaneUpcomingRowHeight - upcomingProgramsView.contentY
            anchorWidth = upcomingProgramsView.width
            anchorHeight = root.rightPaneUpcomingRowHeight
        }

        keyboardBubbleAnchor.x = anchorX
        keyboardBubbleAnchor.y = anchorY
        keyboardBubbleAnchor.width = anchorWidth
        keyboardBubbleAnchor.height = anchorHeight
        showKeyboardProgramBubble(entry.program)
    }

    function selectRightPaneEntry(flatIndex, revealSource) {
        const entries = rightPaneEntries()
        if (entries.length === 0) {
            return false
        }

        const nextIndex = Math.max(0, Math.min(entries.length - 1, flatIndex))
        const entry = entries[nextIndex]
        previewPinned = true
        interactionFocusTarget.forceActiveFocus()
        setPlayerKeyboardFocusArea("rightPane")
        rightPaneSelectionFlatIndex = nextIndex
        rightPaneSelectionKind = entry.kind
        rightPaneUpcomingIndex = entry.listIndex
        rightPaneProgramData = entry.program
        if ((revealSource || "").length > 0) {
            revealUi(revealSource)
        }
        updateKeyboardBubbleAnchor(entry)
        return true
    }

    function syncRightPaneKeyboardSelection() {
        if (playerKeyboardFocusArea !== "rightPane") {
            return
        }

        const entries = rightPaneEntries()
        if (entries.length === 0) {
            setPlayerKeyboardFocusArea("none")
            return
        }

        const nextIndex = rightPaneSelectionFlatIndex >= 0
            ? Math.min(rightPaneSelectionFlatIndex, entries.length - 1)
            : 0
        // Keep keyboard selection anchored, but do not extend overlay auto-hide
        // on passive NOW/NEXT model updates.
        selectRightPaneEntry(nextIndex, "")
    }

    function syncRightPaneSelectionForModelUpdate() {
        if (playerKeyboardFocusArea !== "rightPane") {
            return
        }

        const entries = rightPaneEntries()
        if (entries.length === 0) {
            setPlayerKeyboardFocusArea("none")
            return
        }

        const nextIndex = rightPaneSelectionFlatIndex >= 0
            ? Math.min(rightPaneSelectionFlatIndex, entries.length - 1)
            : 0
        const nextEntry = entries[nextIndex]
        const previousStart = (rightPaneProgramData.start || "")
        const nextStart = (nextEntry.program.start || "")
        const sameSlotProgram = rightPaneSelectionKind === nextEntry.kind
            && rightPaneUpcomingIndex === nextEntry.listIndex
            && previousStart === nextStart

        rightPaneSelectionFlatIndex = nextIndex
        rightPaneProgramData = nextEntry.program
        if (sameSlotProgram) {
            return
        }

        // Keep keyboard selection anchored, but do not extend overlay auto-hide
        // on passive NOW/NEXT model updates.
        selectRightPaneEntry(nextIndex, "")
    }

    function enterLeftPaneFocus(channelId, revealSource) {
        if (!root.hasChannelList) {
            return false
        }

        let targetChannelId = channelId
        if (targetChannelId < 0) {
            targetChannelId = !Number.isNaN(root.playbackChannelId) && root.playbackChannelId >= 0
                ? root.playbackChannelId
                : root.channelList.selectedChannelId
        }

        previewPinned = true
        interactionFocusTarget.forceActiveFocus()
        setPlayerKeyboardFocusArea("leftPane")
        revealUi(revealSource)
        if (targetChannelId >= 0
            && root.channelList.rowForChannelId(targetChannelId) >= 0
            && root.channelList.selectById(targetChannelId)) {
            root.scheduleSelectedChannelVisible(ListView.Contain)
            return true
        }
        const selected = root.channelList.selectAt(0)
        if (selected) {
            root.scheduleSelectedChannelVisible(ListView.Beginning)
        }
        return selected
    }

    function focusTopChannel(revealSource) {
        if (!root.hasChannelList) {
            return false
        }

        previewPinned = true
        interactionFocusTarget.forceActiveFocus()
        setPlayerKeyboardFocusArea("leftPane")
        revealUi(revealSource)
        const selected = root.channelList.selectAt(0)
        if (selected) {
            root.scheduleSelectedChannelVisible(ListView.Beginning)
        }
        return selected
    }

    function moveLeftPaneSelection(delta, revealSource) {
        if (!root.hasChannelList) {
            return false
        }

        previewPinned = true
        interactionFocusTarget.forceActiveFocus()
        setPlayerKeyboardFocusArea("leftPane")
        revealUi(revealSource)
        const moved = root.channelList.selectRelativeWrapped(delta)
        if (moved) {
            root.scheduleSelectedChannelVisible(ListView.Contain)
        }
        return moved
    }

    function enterRightPaneFocus() {
        return selectRightPaneEntry(0, "keyboard")
    }

    function moveRightPaneSelection(delta) {
        const entries = rightPaneEntries()
        if (entries.length === 0) {
            return false
        }

        let nextIndex = rightPaneSelectionFlatIndex
        if (playerKeyboardFocusArea !== "rightPane" || nextIndex < 0 || nextIndex >= entries.length) {
            nextIndex = 0
        } else {
            nextIndex = Math.max(0, Math.min(entries.length - 1, nextIndex + delta))
        }

        return selectRightPaneEntry(nextIndex, "keyboard")
    }

    function showKeyboardVolumeHud(iconFile) {
        if (iconFile && iconFile.length > 0) {
            keyboardVolumeHudIconFile = iconFile
        }
        keyboardVolumeHudVisible = true
        keyboardVolumeHudTimer.restart()
    }

    function resetDebugBubblePosition() {
        root.debugBubbleX = Theme.spacingL
        root.debugBubbleY = Theme.spacingL
    }

    function clampDebugBubblePosition() {
        const minMargin = Theme.spacingL
        const maxX = Math.max(minMargin, root.width - debugBubble.width - minMargin)
        const maxY = Math.max(minMargin, root.height - debugBubble.height - minMargin)
        root.debugBubbleX = Math.max(minMargin, Math.min(root.debugBubbleX, maxX))
        root.debugBubbleY = Math.max(minMargin, Math.min(root.debugBubbleY, maxY))
    }

    function refreshDebugBubbleData() {
        const snapshot = root.player.debugOverlaySnapshot()
        root.debugBubbleData = snapshot ? snapshot : ({})
        root.appendDebugBufferSample(
            Number(root.debugBubbleData.bufferDurationSeconds),
            Number(root.debugBubbleData.minBufferNeededSeconds))
        root.appendDebugBitrateSample(Number(root.debugBubbleData.bitrateValueKbps))
    }

    function toggleDebugBubble() {
        root.debugBubbleEnabled = !root.debugBubbleEnabled
        if (!root.debugBubbleEnabled) {
            root.resetDebugBubblePosition()
            root.debugBubbleData = ({})
            root.resetDebugBufferChart()
            root.resetDebugBitrateChart()
            debugBubbleRefreshTimer.stop()
            return
        }

        if (root.debugBubbleVisible) {
            root.refreshDebugBubbleData()
            root.clampDebugBubblePosition()
            debugBubbleRefreshTimer.restart()
        }
    }

    function scheduleSelectedChannelVisible(positionMode) {
        const row = root.channelList.rowForChannelId(root.channelList.selectedChannelId)
        if (row < 0) {
            return false
        }

        const mode = positionMode !== undefined ? positionMode : ListView.Contain
        if (mode === ListView.Contain && root.channelRowFullyVisible(row)) {
            return true
        }
        if (channelViewSyncTimer.running
            && pendingChannelViewRow === row
            && pendingChannelViewMode === mode) {
            return true
        }

        pendingChannelViewRow = row
        pendingChannelViewMode = mode
        channelViewSyncTimer.restart()
        return true
    }

    function channelRowFullyVisible(rowIndex) {
        if (rowIndex < 0 || !root.hasChannelList) {
            return false
        }
        const rowHeight = 62
        const rowSpacing = 2
        const rowStride = rowHeight + rowSpacing
        const rowTop = rowIndex * rowStride
        const rowBottom = rowTop + rowHeight
        const visibleTop = channelView.contentY
        const visibleBottom = visibleTop + channelView.height
        return rowTop >= visibleTop && rowBottom <= visibleBottom
    }

    function adjustVolume(delta, iconFile) {
        if (root.player.muted || Number(root.player.volume || 0) <= 0) {
            root.player.toggleMute()
        }

        const currentVolume = Number(root.player.volume || 0)
        const nextVolume = Math.max(0, Math.min(100, currentVolume + delta))
        if (nextVolume !== currentVolume) {
            root.player.volume = nextVolume
        }

        const hudIcon = nextVolume <= 0 ? "volume-muted.svg" : iconFile
        showKeyboardVolumeHud(hudIcon)
    }

    function toggleMuteWithKeyboardHud() {
        root.player.toggleMute()
        showKeyboardVolumeHud(root.player.muted ? "volume-muted.svg" : "volume-over-50.svg")
    }

    function openGuideOverlay(revealSource) {
        setPlayerKeyboardFocusArea("none")
        revealUi(revealSource)
        root.shell.openOverlay("guide")
    }

    function openSettingsOverlay(section, revealSource) {
        if (guideOverlayMounted || root.shell.activeOverlay === "guide") {
            forceCloseGuideOverlay()
        }
        setPlayerKeyboardFocusArea("none")
        if (searchField.activeFocus) {
            interactionFocusTarget.forceActiveFocus()
        }
        revealUi(revealSource)
        root.shell.openOverlay("settings", section ? section : "")
    }

    function finalizeGuideOverlayClose(clearShellOverlay) {
        const hideOverlaysAfterClose = guideCloseToVideoOnly
        guideCloseToVideoOnly = false
        guideCloseTimer.stop()
        guideOverlayClosing = false
        guideOverlayOpen = false
        guideOverlayMounted = false
        if (clearShellOverlay && root.shell.activeOverlay === "guide") {
            root.shell.clearOverlay()
        }
        if (hideOverlaysAfterClose) {
            root.exitKeyboardNavigation(true)
            root.shell.overlaysVisible = false
        }
        updateAutoHide()
    }

    function forceCloseGuideOverlay() {
        if (!guideOverlayMounted && root.shell.activeOverlay !== "guide") {
            return
        }
        finalizeGuideOverlayClose(true)
    }

    function closeGuideOverlay(hideOverlaysAfterClose) {
        if (guideOverlayClosing) {
            return
        }
        guideCloseToVideoOnly = !!hideOverlaysAfterClose
        if (!guideOverlayMounted) {
            if (root.shell.activeOverlay === "guide") {
                root.shell.clearOverlay()
            }
            if (guideCloseToVideoOnly) {
                root.exitKeyboardNavigation(true)
                root.shell.overlaysVisible = false
                guideCloseToVideoOnly = false
            }
            return
        }
        guideOverlayClosing = true
        guideOverlayOpen = false
        guideCloseTimer.restart()
    }

    function collapseGuideOverlayToVideoOnly() {
        closeGuideOverlay(true)
    }

    function focusSearchField() {
        if (!root.hasAnyChannels) {
            openSettingsOverlay("sources", "keyboard")
            return
        }

        if (guideOverlayMounted || root.shell.activeOverlay === "guide") {
            forceCloseGuideOverlay()
        } else if (root.shell.activeOverlay !== "none") {
            root.shell.clearOverlay()
        }

        revealUi("keyboard")
        searchField.forceActiveFocus()
        previewHoldTimer.restart()
    }

    function ensureSourcePickerHighlight() {
        const rows = root.sourcePickerVisibleRows
        if (rows.length === 0) {
            root.sourcePickerHighlightedId = ""
            return false
        }

        for (let index = 0; index < rows.length; ++index) {
            if (rows[index].id === root.sourcePickerHighlightedId) {
                return true
            }
        }

        root.sourcePickerHighlightedId = rows[0].id
        return true
    }

    function openSourcePicker(revealSource) {
        const firstProfile = root.profiles.get(0)
        if (!firstProfile.id) {
            root.openSettingsOverlay("sources", revealSource)
            return
        }

        if (guideOverlayMounted || root.shell.activeOverlay === "guide") {
            forceCloseGuideOverlay()
        } else if (root.shell.activeOverlay !== "none") {
            root.shell.clearOverlay()
        }

        root.groupPickerOpen = false
        root.groupPickerSearchText = ""
        root.groupPickerHighlightedId = ""
        root.sourcePickerSearchText = ""
        root.sourcePickerHighlightedId = ""
        root.sourcePickerOpen = true
        interactionFocusTarget.forceActiveFocus()
        root.setPlayerKeyboardFocusArea("sourceSearch")
        revealUi(revealSource)
        root.shell.overlaysVisible = true
        Qt.callLater(function() {
            root.ensureSourcePickerHighlight()
            sourcePickerSearchField.forceActiveFocus()
        })
    }

    function beginLeftPaneHide(mode) {
        root.leftPaneClosingMode = mode
        root.leftPaneClosingToHidden = true
    }

    function clearLeftPaneHideIfSettled() {
        if (root.leftPickerOpen || root.showShellChrome || root.pendingEmptyPipActive) {
            root.leftPaneClosingMode = ""
            root.leftPaneClosingToHidden = false
            return
        }
        if (!leftChromeSlideAnimation.running && !leftChromeOpacityAnimation.running) {
            root.leftPaneClosingMode = ""
            root.leftPaneClosingToHidden = false
        }
    }

    function closeSourcePicker(hideOverlaysAfterClose) {
        if (!root.sourcePickerOpen) {
            return
        }

        if (hideOverlaysAfterClose) {
            root.beginLeftPaneHide("source")
        }
        root.sourcePickerOpen = false
        root.sourcePickerSearchText = ""
        root.sourcePickerHighlightedId = ""
        if (sourcePickerSearchField.activeFocus) {
            interactionFocusTarget.forceActiveFocus()
        }
        root.setPlayerKeyboardFocusArea("none")
        if (hideOverlaysAfterClose) {
            root.exitKeyboardNavigation(true)
            root.shell.overlaysVisible = false
            return
        }
        root.scheduleSelectedChannelVisible(ListView.Contain)
        root.updateAutoHide()
    }

    function focusTopSourceRow(revealSource) {
        if (!root.ensureSourcePickerHighlight()) {
            return false
        }

        interactionFocusTarget.forceActiveFocus()
        root.setPlayerKeyboardFocusArea("leftPane")
        revealUi(revealSource)
        return true
    }

    function moveSourcePickerSelection(delta, revealSource) {
        const rows = root.sourcePickerVisibleRows
        if (rows.length === 0) {
            return false
        }

        let index = 0
        for (let row = 0; row < rows.length; ++row) {
            if (rows[row].id === root.sourcePickerHighlightedId) {
                index = row
                break
            }
        }
        index = Math.max(0, Math.min(rows.length - 1, index + delta))
        root.sourcePickerHighlightedId = rows[index].id
        interactionFocusTarget.forceActiveFocus()
        root.setPlayerKeyboardFocusArea("leftPane")
        revealUi(revealSource)
        if (sourcePickerView.forceLayout) {
            sourcePickerView.forceLayout()
        }
        sourcePickerView.positionViewAtIndex(index, ListView.Contain)
        return true
    }

    function confirmSourcePickerSelection(revealSource) {
        const rows = root.sourcePickerVisibleRows
        if (rows.length === 0) {
            return false
        }

        let selectedId = root.sourcePickerHighlightedId
        if (!rows.some(function(row) { return row.id === selectedId })) {
            selectedId = rows[0].id
        }

        root.pendingSourcePickerProfileId = selectedId
        root.pendingSourcePickerRevealSource = revealSource || "keyboard"
        root.closeSourcePicker(false)
        if (!root.profiles.selectProfile(selectedId)) {
            root.pendingSourcePickerProfileId = ""
            root.pendingSourcePickerRevealSource = ""
            return false
        }
        root.keyboardModeLocked = false
        root.keyboardNavigationCandidateCount = 1
        keyboardNavigationCandidateTimer.restart()
        root.setPlayerKeyboardFocusArea("leftPane")
        root.updateAutoHide()
        return true
    }

    function ensureGroupPickerHighlight() {
        const rows = root.groupPickerVisibleRows
        if (rows.length === 0) {
            root.groupPickerHighlightedId = ""
            return false
        }

        for (let index = 0; index < rows.length; ++index) {
            if (rows[index].id === root.groupPickerHighlightedId) {
                return true
            }
        }

        root.groupPickerHighlightedId = rows[0].id
        return true
    }

    function openGroupPicker(revealSource) {
        if (!root.hasActiveProfile) {
            root.openSettingsOverlay("sources", revealSource)
            return
        }

        if (guideOverlayMounted || root.shell.activeOverlay === "guide") {
            forceCloseGuideOverlay()
        } else if (root.shell.activeOverlay !== "none") {
            root.shell.clearOverlay()
        }

        root.sourcePickerOpen = false
        root.sourcePickerSearchText = ""
        root.sourcePickerHighlightedId = ""
        root.liveGroups.profileId = root.app.activeProfileId
        root.liveGroups.reload()
        root.groupPickerSearchText = ""
        root.groupPickerHighlightedId = ""
        root.groupPickerOpen = true
        interactionFocusTarget.forceActiveFocus()
        root.setPlayerKeyboardFocusArea("groupSearch")
        revealUi(revealSource)
        root.shell.overlaysVisible = true
        Qt.callLater(function() {
            root.ensureGroupPickerHighlight()
            groupPickerSearchField.forceActiveFocus()
        })
    }

    function closeGroupPicker(hideOverlaysAfterClose) {
        if (!root.groupPickerOpen) {
            return
        }

        if (hideOverlaysAfterClose) {
            root.beginLeftPaneHide("group")
        }
        root.groupPickerOpen = false
        root.groupPickerSearchText = ""
        root.groupPickerHighlightedId = ""
        if (groupPickerSearchField.activeFocus) {
            interactionFocusTarget.forceActiveFocus()
        }
        root.setPlayerKeyboardFocusArea("none")
        if (hideOverlaysAfterClose) {
            root.exitKeyboardNavigation(true)
            root.shell.overlaysVisible = false
            return
        }
        root.scheduleSelectedChannelVisible(ListView.Contain)
        root.updateAutoHide()
    }

    function openAudioPicker() {
        if (guideOverlayMounted || root.shell.activeOverlay === "guide") {
            forceCloseGuideOverlay()
        } else if (root.shell.activeOverlay !== "none") {
            root.shell.clearOverlay()
        }
        root.groupPickerOpen = false
        root.sourcePickerOpen = false
        root.subtitlePickerOpen = false
        root.subtitlePickerRows = []
        root.audioPickerRows = root.player.audioTracks()
        root.audioPickerOpen = true
        root.audioPickerHighlightedId = root.audioPickerRows.length > 0 ? root.audioPickerRows[0].id : -1
        interactionFocusTarget.forceActiveFocus()
        root.setPlayerKeyboardFocusArea("audioTrack")
        revealUi("keyboard")
        root.shell.overlaysVisible = true
    }

    function closeAudioPicker(hideOverlaysAfterClose) {
        if (!root.audioPickerOpen) {
            return
        }
        if (hideOverlaysAfterClose) {
            root.beginLeftPaneHide("audio")
        }
        root.audioPickerOpen = false
        root.audioPickerRows = []
        root.audioPickerHighlightedId = -1
        root.setPlayerKeyboardFocusArea("none")
        if (hideOverlaysAfterClose) {
            root.exitKeyboardNavigation(true)
            root.shell.overlaysVisible = false
            return
        }
        root.scheduleSelectedChannelVisible(ListView.Contain)
        root.updateAutoHide()
    }

    function moveAudioPickerSelection(delta) {
        const rows = root.audioPickerRows
        if (!rows.length) {
            return false
        }
        let idx = -1
        for (let i = 0; i < rows.length; ++i) {
            if (rows[i].id === root.audioPickerHighlightedId) {
                idx = i
                break
            }
        }
        if (idx < 0) {
            idx = 0
        } else {
            idx = Math.max(0, Math.min(rows.length - 1, idx + delta))
        }
        root.audioPickerHighlightedId = rows[idx].id
        audioPickerView.positionViewAtIndex(idx, ListView.Contain)
        return true
    }

    function confirmAudioPickerSelection() {
        if (root.audioPickerHighlightedId < 0) {
            return false
        }
        root.player.selectAudioTrack(root.audioPickerHighlightedId)
        root.closeAudioPicker(false)
        return true
    }

    function openSubtitlePicker() {
        if (guideOverlayMounted || root.shell.activeOverlay === "guide") {
            forceCloseGuideOverlay()
        } else if (root.shell.activeOverlay !== "none") {
            root.shell.clearOverlay()
        }
        root.groupPickerOpen = false
        root.sourcePickerOpen = false
        root.audioPickerOpen = false
        root.audioPickerRows = []
        root.subtitlePickerRows = root.player.subtitleTracks()
        root.subtitlePickerOpen = true
        root.subtitlePickerHighlightedId = root.subtitlePickerRows.length > 0 ? root.subtitlePickerRows[0].id : -1
        interactionFocusTarget.forceActiveFocus()
        root.setPlayerKeyboardFocusArea("subtitleTrack")
        revealUi("keyboard")
        root.shell.overlaysVisible = true
    }

    function closeSubtitlePicker(hideOverlaysAfterClose) {
        if (!root.subtitlePickerOpen) {
            return
        }
        if (hideOverlaysAfterClose) {
            root.beginLeftPaneHide("subtitle")
        }
        root.subtitlePickerOpen = false
        root.subtitlePickerRows = []
        root.subtitlePickerHighlightedId = -1
        root.setPlayerKeyboardFocusArea("none")
        if (hideOverlaysAfterClose) {
            root.exitKeyboardNavigation(true)
            root.shell.overlaysVisible = false
            return
        }
        root.scheduleSelectedChannelVisible(ListView.Contain)
        root.updateAutoHide()
    }

    function moveSubtitlePickerSelection(delta) {
        const rows = root.subtitlePickerRows
        if (!rows.length) {
            return false
        }
        let idx = -1
        for (let i = 0; i < rows.length; ++i) {
            if (rows[i].id === root.subtitlePickerHighlightedId) {
                idx = i
                break
            }
        }
        if (idx < 0) {
            idx = 0
        } else {
            idx = Math.max(0, Math.min(rows.length - 1, idx + delta))
        }
        root.subtitlePickerHighlightedId = rows[idx].id
        subtitlePickerView.positionViewAtIndex(idx, ListView.Contain)
        return true
    }

    function confirmSubtitlePickerSelection() {
        if (root.subtitlePickerHighlightedId < 0) {
            return false
        }
        root.player.selectSubtitleTrack(root.subtitlePickerHighlightedId)
        root.closeSubtitlePicker(false)
        return true
    }

    function focusTopGroupRow(revealSource) {
        if (!root.ensureGroupPickerHighlight()) {
            return false
        }

        interactionFocusTarget.forceActiveFocus()
        root.setPlayerKeyboardFocusArea("leftPane")
        revealUi(revealSource)
        return true
    }

    function moveGroupPickerSelection(delta, revealSource) {
        const rows = root.groupPickerVisibleRows
        if (rows.length === 0) {
            return false
        }

        let index = 0
        for (let row = 0; row < rows.length; ++row) {
            if (rows[row].id === root.groupPickerHighlightedId) {
                index = row
                break
            }
        }
        index = Math.max(0, Math.min(rows.length - 1, index + delta))
        root.groupPickerHighlightedId = rows[index].id
        interactionFocusTarget.forceActiveFocus()
        root.setPlayerKeyboardFocusArea("leftPane")
        revealUi(revealSource)
        if (groupPickerView.forceLayout) {
            groupPickerView.forceLayout()
        }
        groupPickerView.positionViewAtIndex(index, ListView.Contain)
        return true
    }

    function applyConfirmedGroupFilter(groupId) {
        root.channelList.selectedCategoryId = groupId
        if (root.channelList.rowForChannelId(root.channelList.selectedChannelId) < 0) {
            root.channelList.selectAt(0)
        }
        root.previewPinned = true
        root.scheduleSelectedChannelVisible(ListView.Contain)
    }

    function confirmGroupPickerSelection() {
        const rows = root.groupPickerVisibleRows
        if (rows.length === 0) {
            return false
        }

        let selectedId = root.groupPickerHighlightedId
        if (!rows.some(function(row) { return row.id === selectedId })) {
            selectedId = rows[0].id
        }

        root.applyConfirmedGroupFilter(selectedId)
        root.closeGroupPicker(false)
        root.keyboardModeLocked = false
        root.keyboardNavigationCandidateCount = 1
        keyboardNavigationCandidateTimer.restart()
        root.setPlayerKeyboardFocusArea("leftPane")
        root.updateAutoHide()
        return true
    }

    function exitKeyboardNavigation(resetSearchFocus) {
        keyboardModeLocked = false
        clearKeyboardNavigationCandidate()
        rightPaneSelectionFlatIndex = -1
        rightPaneSelectionKind = ""
        rightPaneUpcomingIndex = -1
        rightPaneProgramData = ({})
        if (playerKeyboardFocusArea === "rightPane") {
            clearKeyboardProgramBubble()
        }
        if (resetSearchFocus && searchField.activeFocus) {
            interactionFocusTarget.forceActiveFocus()
        }
        if (resetSearchFocus && groupPickerSearchField.activeFocus) {
            interactionFocusTarget.forceActiveFocus()
        }
        if (resetSearchFocus && sourcePickerSearchField.activeFocus) {
            interactionFocusTarget.forceActiveFocus()
        }
        if (playerKeyboardFocusArea !== "search" || resetSearchFocus) {
            playerKeyboardFocusArea = "none"
        }
        if (!root.shell.overlaysVisible) {
            root.overlayInteractionSource = "none"
        }
    }

    function closeTransientPlayerChrome() {
        root.exitKeyboardNavigation(true)
        if (root.channelList.searchText.length > 0) {
            root.channelList.searchText = ""
        }
        root.hideProgramHoverBubble()
        root.clearNumericEntry()
        overlayTimer.stop()
        root.overlayInteractionSource = "none"
        root.shell.overlaysVisible = false
    }

    function handleEscape() {
        if (root.multiviewSelectionMode) {
            root.setMultiviewSelectionMode(false)
            return true
        }

        if (root.pendingEmptyPipAssignment && root.multiView.layoutMode === "pip") {
            const secondaryTile = root.multiviewTileData(1)
            if (!secondaryTile || Boolean(secondaryTile.isEmpty)) {
                root.pendingEmptyPipAssignment = false
                root.closeTransientPlayerChrome()
                root.multiView.togglePictureInPicture(-1)
                return true
            }
            root.pendingEmptyPipAssignment = false
        }

        if (root.audioPickerOpen) {
            root.closeAudioPicker(true)
            return true
        }

        if (root.subtitlePickerOpen) {
            root.closeSubtitlePicker(true)
            return true
        }

        if (root.sourcePickerOpen) {
            root.closeSourcePicker(true)
            return true
        }

        if (root.groupPickerOpen) {
            root.closeGroupPicker(true)
            return true
        }

        if (guideOverlayMounted || root.shell.activeOverlay === "guide") {
            closeGuideOverlay(true)
            return true
        }

        if (root.shell.activeOverlay !== "none") {
            if (root.shell.activeOverlay === "settings") {
                settingsPage.requestClose()
            } else {
                root.shell.clearOverlay()
            }
            updateAutoHide()
            return true
        }

        const hasTransientChrome = root.keyboardModeLocked
            || root.playerKeyboardFocusArea !== "none"
            || searchField.activeFocus
            || root.channelList.searchText.length > 0
            || root.hoverBubbleVisible
            || root.numericInputState !== "none"
        if (hasTransientChrome) {
            root.closeTransientPlayerChrome()
            return true
        }

        if (root.shell.overlaysVisible) {
            overlayTimer.stop()
            root.shell.overlaysVisible = false
            return true
        }

        return false
    }

    function handleLiveKey(event) {
        const ctrlPressed = (event.modifiers & Qt.ControlModifier) !== 0
        const shiftPressed = (event.modifiers & Qt.ShiftModifier) !== 0
        const altPressed = (event.modifiers & Qt.AltModifier) !== 0
        const metaPressed = (event.modifiers & Qt.MetaModifier) !== 0
        if (ctrlPressed && altPressed && !metaPressed && event.key === Qt.Key_O) {
            return root.multiView.fullPromoteAndExit()
        }
        const altOrMetaPressed = altPressed || metaPressed
        if (altOrMetaPressed) {
            return false
        }

        const digit = root.digitForKey(event.key)
        if (!ctrlPressed && digit.length > 0 && root.numericEntryContextActive) {
            return root.appendNumericInputDigit(digit)
        }

        if (root.numericInputState !== "none"
            && digit.length === 0
            && root.shouldClearNumericEntryForKey(event.key, ctrlPressed)) {
            root.clearNumericEntry()
        }

        if (root.audioPickerOpen) {
            switch (event.key) {
            case Qt.Key_Up:
                noteKeyboardNavigationKey()
                return root.moveAudioPickerSelection(-1)
            case Qt.Key_Down:
                noteKeyboardNavigationKey()
                return root.moveAudioPickerSelection(1)
            case Qt.Key_Return:
            case Qt.Key_Enter:
                noteKeyboardNavigationKey()
                return root.confirmAudioPickerSelection()
            default:
                return false
            }
        }

        if (root.subtitlePickerOpen) {
            switch (event.key) {
            case Qt.Key_Up:
                noteKeyboardNavigationKey()
                return root.moveSubtitlePickerSelection(-1)
            case Qt.Key_Down:
                noteKeyboardNavigationKey()
                return root.moveSubtitlePickerSelection(1)
            case Qt.Key_Return:
            case Qt.Key_Enter:
                noteKeyboardNavigationKey()
                return root.confirmSubtitlePickerSelection()
            default:
                return false
            }
        }

        if (root.sourcePickerOpen) {
            if (ctrlPressed) {
                if (event.key === Qt.Key_S) {
                    return true
                }
                if (event.key === Qt.Key_G) {
                    noteKeyboardNavigationKey()
                    openGroupPicker("keyboard")
                    return true
                }
                return false
            }

            if (sourcePickerSearchField.activeFocus) {
                if (event.key === Qt.Key_Down
                    || event.key === Qt.Key_Tab
                    || event.key === Qt.Key_Return
                    || event.key === Qt.Key_Enter) {
                    noteKeyboardNavigationKey()
                    return root.focusTopSourceRow("keyboard")
                }
                return false
            }

            switch (event.key) {
            case Qt.Key_Up:
                noteKeyboardNavigationKey()
                return root.moveSourcePickerSelection(-1, "keyboard")
            case Qt.Key_Down:
                noteKeyboardNavigationKey()
                return root.moveSourcePickerSelection(1, "keyboard")
            case Qt.Key_Return:
            case Qt.Key_Enter:
                noteKeyboardNavigationKey()
                return root.confirmSourcePickerSelection("keyboard")
            default:
                return false
            }
        }

        if (root.groupPickerOpen) {
            if (ctrlPressed) {
                if (event.key === Qt.Key_G) {
                    return true
                }
                if (event.key === Qt.Key_S) {
                    noteKeyboardNavigationKey()
                    openSourcePicker("keyboard")
                    return true
                }
                return false
            }

            if (groupPickerSearchField.activeFocus) {
                if (event.key === Qt.Key_Down) {
                    noteKeyboardNavigationKey()
                    return root.focusTopGroupRow("keyboard")
                }
                if (event.key === Qt.Key_Tab
                    || event.key === Qt.Key_Return
                    || event.key === Qt.Key_Enter) {
                    noteKeyboardNavigationKey()
                    return root.focusTopGroupRow("keyboard")
                }
                return false
            }

            switch (event.key) {
            case Qt.Key_Up:
                noteKeyboardNavigationKey()
                return root.moveGroupPickerSelection(-1, "keyboard")
            case Qt.Key_Down:
                noteKeyboardNavigationKey()
                return root.moveGroupPickerSelection(1, "keyboard")
            case Qt.Key_Return:
            case Qt.Key_Enter:
                noteKeyboardNavigationKey()
                return root.confirmGroupPickerSelection()
            default:
                return false
            }
        }

        if (searchField.activeFocus) {
            if (ctrlPressed && event.key === Qt.Key_G) {
                noteKeyboardNavigationKey()
                openGroupPicker("keyboard")
                return true
            }
            if (ctrlPressed && event.key === Qt.Key_S) {
                noteKeyboardNavigationKey()
                openSourcePicker("keyboard")
                return true
            }
            if (ctrlPressed && event.key === Qt.Key_Up) {
                noteKeyboardNavigationKey()
                openGuideOverlay("keyboard")
                return true
            }
            if (ctrlPressed && event.key === Qt.Key_F) {
                focusSearchField()
                return true
            }
            if (event.key === Qt.Key_Tab
                || event.key === Qt.Key_Return
                || event.key === Qt.Key_Enter) {
                noteKeyboardNavigationKey()
                return focusTopChannel("keyboard")
            }
            return false
        }

        if (root.pendingEmptyPipActive) {
            switch (event.key) {
            case Qt.Key_Up:
                noteKeyboardNavigationKey()
                return moveLeftPaneSelection(-1, "keyboard")
            case Qt.Key_Down:
                noteKeyboardNavigationKey()
                return moveLeftPaneSelection(1, "keyboard")
            case Qt.Key_Return:
            case Qt.Key_Enter:
                noteKeyboardNavigationKey()
                playSelection("keyboard")
                return true
            default:
                return false
            }
        }

        const multiviewShortcutHandled = root.handleMultiviewShortcut(
            event.key,
            ctrlPressed,
            shiftPressed,
            altPressed)
        if (multiviewShortcutHandled !== null) {
            return multiviewShortcutHandled
        }

        if (root.multiviewSelectionMode) {
            switch (event.key) {
            case Qt.Key_Left:
                return root.moveMultiviewSelection(0, -1)
            case Qt.Key_Right:
                return root.moveMultiviewSelection(0, 1)
            case Qt.Key_Up:
                return root.moveMultiviewSelection(-1, 0)
            case Qt.Key_Down:
                return root.moveMultiviewSelection(1, 0)
            case Qt.Key_Return:
            case Qt.Key_Enter:
                return root.commitMultiviewSelection()
            default:
                return false
            }
        }

        if (ctrlPressed) {
            if (event.key === Qt.Key_F) {
                focusSearchField()
                return true
            }
            if (event.key === Qt.Key_G) {
                noteKeyboardNavigationKey()
                openGroupPicker("keyboard")
                return true
            }
            if (event.key === Qt.Key_S) {
                noteKeyboardNavigationKey()
                openSourcePicker("keyboard")
                return true
            }
            if (event.key === Qt.Key_Up) {
                noteKeyboardNavigationKey()
                openGuideOverlay("keyboard")
                return true
            }
            if (event.key === Qt.Key_R) {
                return root.handleCtrlRShortcut()
            }
            return false
        }

        if (event.key === Qt.Key_F
            && playerKeyboardFocusArea === "leftPane"
            && root.channelList.selectedChannelId >= 0
            && root.channelList.rowForChannelId(root.channelList.selectedChannelId) >= 0) {
            noteKeyboardNavigationKey()
            return root.toggleSelectedChannelFavourite()
        }

        switch (event.key) {
        case Qt.Key_J:
            return root.seekTimeshiftRelativeWithBadge(-10)
        case Qt.Key_L:
            return root.seekTimeshiftRelativeWithBadge(10)
        case Qt.Key_Home:
            return root.jumpToLiveEdgeWithBadge()
        case Qt.Key_Space:
            root.togglePauseWithBadge()
            return true
        case Qt.Key_F:
            toggleFullscreen()
            return true
        case Qt.Key_M:
            toggleMuteWithKeyboardHud()
            return true
        case Qt.Key_Comma:
            adjustVolume(-5, "volume-below-50.svg")
            return true
        case Qt.Key_Period:
            adjustVolume(5, "volume-over-50.svg")
            return true
        case Qt.Key_Delete:
            if (root.multiviewActive) {
                root.multiView.closeFocusedTile()
                return true
            }
            return false
        case Qt.Key_Tab:
            noteKeyboardNavigationKey()
            focusSearchField()
            return true
        case Qt.Key_Left:
            noteKeyboardNavigationKey()
            if (playerKeyboardFocusArea === "rightPane") {
                return enterLeftPaneFocus(root.channelList.selectedChannelId, "keyboard")
            }
            if (playerKeyboardFocusArea === "leftPane") {
                return false
            }
            return enterLeftPaneFocus(-1, "keyboard")
        case Qt.Key_Right:
            noteKeyboardNavigationKey()
            if (playerKeyboardFocusArea === "rightPane") {
                return false
            }
            return enterRightPaneFocus()
        case Qt.Key_Up:
            if (playerKeyboardFocusArea === "leftPane") {
                noteKeyboardNavigationKey()
                return moveLeftPaneSelection(-1, "keyboard")
            }
            if (playerKeyboardFocusArea === "rightPane") {
                noteKeyboardNavigationKey()
                return moveRightPaneSelection(-1)
            }
            if (root.shell.overlaysVisible) {
                return false
            }
            return activateChannelRelative(-1, true)
        case Qt.Key_Down:
            if (playerKeyboardFocusArea === "leftPane") {
                noteKeyboardNavigationKey()
                return moveLeftPaneSelection(1, "keyboard")
            }
            if (playerKeyboardFocusArea === "rightPane") {
                noteKeyboardNavigationKey()
                return moveRightPaneSelection(1)
            }
            if (root.shell.overlaysVisible) {
                return false
            }
            return activateChannelRelative(1, true)
        case Qt.Key_Backspace:
            if (root.shell.overlaysVisible) {
                return false
            }
            return root.app.activatePreviousChannel()
        case Qt.Key_Return:
        case Qt.Key_Enter:
            noteKeyboardNavigationKey()
            playSelection("keyboard")
            return true
        default:
            return false
        }
    }

    function handleWindowKey(event) {
        if (event.key === Qt.Key_F1) {
            root.openAudioPicker()
            return true
        }

        if (event.key === Qt.Key_F2) {
            root.openSubtitlePicker()
            return true
        }

        if (event.key === Qt.Key_F3) {
            root.toggleDebugBubble()
            return true
        }

        if (event.key === Qt.Key_F6) {
            if (root.mainWindow && root.mainWindow.toggleAlwaysOnTop) {
                root.mainWindow.toggleAlwaysOnTop()
                return true
            }
            return false
        }

        if (event.key === Qt.Key_Escape) {
            return handleEscape()
        }

        if (root.shell.activeOverlay === "settings") {
            return false
        }

        if (root.shell.activeOverlay === "guide" || guideOverlayMounted) {
            const ctrlPressed = (event.modifiers & Qt.ControlModifier) !== 0
            const shiftPressed = (event.modifiers & Qt.ShiftModifier) !== 0
            const altPressed = (event.modifiers & Qt.AltModifier) !== 0
            if (ctrlPressed && altPressed && event.key === Qt.Key_O) {
                return root.multiView.fullPromoteAndExit()
            }
            if (ctrlPressed && event.key === Qt.Key_R) {
                return root.handleCtrlRShortcut()
            }
            if (ctrlPressed && event.key === Qt.Key_P) {
                return shiftPressed
                    ? root.multiView.swapPrimaryWithPictureInPicture()
                    : root.handlePictureInPictureShortcut()
            }
            return guidePage.handleKeyboardEvent(event)
        }

        return handleLiveKey(event)
    }

    Timer {
        id: overlayTimer
        interval: Math.max(1, root.settings.overlayAutoHideSeconds) * 1000
        repeat: false
        onTriggered: {
            if (root.settings.overlayAutoHide
                && root.shell.activeOverlay === "none"
                && !root.overlayHideLocked()) {
                root.shell.overlaysVisible = false
            }
        }
    }

    Timer {
        id: keyboardNavigationCandidateTimer
        interval: Math.max(1, root.settings.overlayAutoHideSeconds) * 1000
        repeat: false
        onTriggered: root.keyboardNavigationCandidateCount = 0
    }

    Timer {
        id: keyboardVolumeHudTimer
        interval: 1200
        repeat: false
        onTriggered: root.keyboardVolumeHudVisible = false
    }

    Timer {
        id: debugBubbleRefreshTimer
        interval: 250
        repeat: true
        running: false
        onTriggered: root.refreshDebugBubbleData()
    }

    Timer {
        id: channelChangeBubbleTimer
        interval: Math.max(1, root.settings.overlayAutoHideSeconds) * 1000
        repeat: false
        onTriggered: root.channelChangeBubbleVisible = false
    }

    Timer {
        id: numericCommitTimer
        interval: 2000
        repeat: false
        onTriggered: root.commitNumericEntry()
    }

    Timer {
        id: numericFailureTimer
        interval: 2000
        repeat: false
        onTriggered: root.clearNumericEntry()
    }

    Timer {
        id: channelViewSyncTimer
        interval: 0
        repeat: false
        onTriggered: {
            if (root.pendingChannelViewRow < 0 || !root.hasChannelList) {
                return
            }

            if (root.pendingChannelViewMode !== ListView.Contain
                || !root.channelRowFullyVisible(root.pendingChannelViewRow)) {
                channelView.positionViewAtIndex(root.pendingChannelViewRow, root.pendingChannelViewMode)
            }
        }
    }

    Timer {
        id: hoverPreviewDebounceTimer
        interval: 90
        repeat: false
        onTriggered: root.flushQueuedHoverPreviewChannel()
    }

    Timer {
        id: channelListScrollSettleTimer
        interval: 120
        repeat: false
        onTriggered: {
            if (root.channelListScrollActiveRaw) {
                root.channelListScrollSettling = true
                channelListScrollSettleTimer.restart()
                return
            }
            root.channelListScrollSettling = false
            root.flushQueuedHoverPreviewChannel()
        }
    }

    Timer {
        id: previewHoldTimer
        interval: 2600
        repeat: false
    }

    Timer {
        id: hoverBubbleHideTimer
        interval: 140
        repeat: false
        onTriggered: {
            if (!root.hoverBubbleActive && !root.keyboardProgramBubbleActive) {
                root.hideProgramHoverBubble()
            }
        }
    }

    Timer {
        id: guideCloseTimer
        interval: root.overlayAnimationDuration
        repeat: false
        onTriggered: root.finalizeGuideOverlayClose(true)
    }

    Timer {
        id: liveTimelineNoticeTimer
        interval: 5000
        repeat: false
        onTriggered: root.liveTimelineNoticeText = ""
    }

    Timer {
        interval: 1000
        running: root.visible
        repeat: true
        onTriggered: root.currentClockText = Qt.formatDateTime(new Date(), "ddd dd MMM  hh:mm")
    }

    Component.onCompleted: {
        root.liveGroups.profileId = root.app.activeProfileId
        root.channelList.selectedCategoryId = ""
        committedChannelId = root.channelList.selectedChannelId
        root.scheduleSelectedChannelVisible(ListView.Contain)
        root.refreshLiveCatchupState()
    }

    onKeyboardModeLockedChanged: root.updateAutoHide()
    onHoverBubbleActiveChanged: root.updateAutoHide()
    onNumericEntryContextActiveChanged: {
        if (!root.numericEntryContextActive) {
            root.clearNumericEntry()
        }
    }

    onSideNowNextModelChanged: {
        if (root.playerKeyboardFocusArea === "rightPane") {
            root.syncRightPaneKeyboardSelection()
        } else {
            root.hideProgramHoverBubble()
        }
    }

    onShowShellChromeChanged: {
        root.clearLeftPaneHideIfSettled()
        if (!root.showShellChrome) {
            root.hideProgramHoverBubble()
        }
    }

    onWidthChanged: {
        root.repositionProgramHoverBubble()
        root.clampDebugBubblePosition()
    }
    onHeightChanged: {
        root.repositionProgramHoverBubble()
        root.clampDebugBubblePosition()
    }

    onDebugBubbleVisibleChanged: {
        if (root.debugBubbleVisible) {
            root.refreshDebugBubbleData()
            root.clampDebugBubblePosition()
            debugBubbleRefreshTimer.restart()
            return
        }

        debugBubbleRefreshTimer.stop()
    }

    Connections {
        target: root.profiles

        function onDataChanged(topLeft, bottomRight, roles) {
            root.sourcePickerModelRevision += 1
            if (root.sourcePickerOpen) {
                root.ensureSourcePickerHighlight()
            }
        }

        function onModelReset() {
            root.sourcePickerModelRevision += 1
            if (root.sourcePickerOpen) {
                root.ensureSourcePickerHighlight()
            }
        }

        function onRowsInserted(parent, first, last) {
            root.sourcePickerModelRevision += 1
            if (root.sourcePickerOpen) {
                root.ensureSourcePickerHighlight()
            }
        }

        function onRowsRemoved(parent, first, last) {
            root.sourcePickerModelRevision += 1
            if (root.sourcePickerOpen) {
                root.ensureSourcePickerHighlight()
            }
        }

        function onRowsMoved(sourceParent, sourceStart, sourceEnd, destinationParent, destinationRow) {
            root.sourcePickerModelRevision += 1
            if (root.sourcePickerOpen) {
                root.ensureSourcePickerHighlight()
            }
        }
    }

    Connections {
        target: root.liveGroups

        function onGroupsChanged() {
            root.groupPickerModelRevision += 1
            if (root.groupPickerOpen) {
                root.ensureGroupPickerHighlight()
            }
        }
    }

    Connections {
        target: root.channelList

        function onSelectedChannelIdChanged() {
            if (!root.hoverPreviewActive) {
                root.committedChannelId = root.channelList.selectedChannelId
            }
            if (!root.suppressSelectionInteraction
                && root.guideState.selectedChannelId >= 0
                && !root.channelListScrollingActive) {
                root.noteBrowseInteraction()
            }
            if (!root.channelListScrollingActive) {
                root.scheduleSelectedChannelVisible(ListView.Contain)
            }
        }
    }

    Connections {
        target: root.nowNext
        function onChannelChanged() {
            root.hideProgramHoverBubble()
        }
        function onDataChanged() {
            if (root.sideNowNextModel === root.nowNext) {
                root.syncRightPaneSelectionForModelUpdate()
            }
        }
    }

    Connections {
        target: root.playbackNowNext
        function onChannelChanged() {
            root.hideProgramHoverBubble()
            root.refreshLiveCatchupState()
        }
        function onDataChanged() {
            if (root.sideNowNextModel === root.playbackNowNext) {
                root.syncRightPaneSelectionForModelUpdate()
            }
            root.refreshLiveCatchupState()
        }
    }

    Connections {
        target: root.app

        function onActiveProfileIdChanged() {
            root.liveGroups.profileId = root.app.activeProfileId
            if (root.sourcePickerOpen) {
                root.ensureSourcePickerHighlight()
            }
        }

        function onProfileLoadFinished(profileId, ok) {
            if (profileId === root.liveGroups.profileId) {
                root.liveGroups.reload()
                if (root.groupPickerOpen) {
                    root.ensureGroupPickerHighlight()
                }
            }

            if (profileId !== root.pendingSourcePickerProfileId) {
                return
            }

            const revealSource = root.pendingSourcePickerRevealSource || "keyboard"
            root.pendingSourcePickerProfileId = ""
            root.pendingSourcePickerRevealSource = ""

            if (!ok) {
                root.updateAutoHide()
                return
            }

            root.revealUi(revealSource)
            if (root.channelList.rowForChannelId(root.channelList.selectedChannelId) >= 0) {
                root.activateChannelById(root.channelList.selectedChannelId)
            } else if (root.hasChannelList) {
                root.channelList.activateAt(0)
            }
            root.scheduleSelectedChannelVisible(ListView.Contain)
            root.setPlayerKeyboardFocusArea("leftPane")
            root.updateAutoHide()
        }
    }

    Connections {
        target: root.player

        function onCurrentChannelChanged() {
            root.markTimeshiftBadgeLive()
            root.refreshLiveCatchupState()
            const currentChannelId = Number(root.player.currentChannel.id)
            if (Number.isNaN(currentChannelId) || currentChannelId < 0) {
                root.channelChangeBubbleVisible = false
            }
        }

        function onPlaybackModeChanged() {
            root.refreshLiveCatchupState()
            root.liveTimelineNoticeText = ""
            liveTimelineNoticeTimer.stop()
        }

        function onTimeshiftStateChanged() {
            if (!root.transportTimelineActive || root.timeshiftUiAtLiveEdge) {
                root.markTimeshiftBadgeLive()
            }
            if (root.player.timeshiftActive) {
                root.liveTimelineNoticeText = ""
                liveTimelineNoticeTimer.stop()
            }
        }

        function onPlaybackChannelActivated(channelId) {
            if (channelId < 0) {
                root.channelChangeBubbleVisible = false
                return
            }
            root.showChannelChangeBubble()
            root.liveTimelineNoticeText = ""
            liveTimelineNoticeTimer.stop()
        }
    }

    Connections {
        target: root.shell

        function onActiveOverlayChanged() {
            if (root.shell.activeOverlay !== "none" && root.groupPickerOpen) {
                root.closeGroupPicker(false)
            }
            if (root.shell.activeOverlay !== "none" && root.sourcePickerOpen) {
                root.closeSourcePicker(false)
            }

            if (root.shell.activeOverlay === "guide") {
                overlayTimer.stop()
                guideCloseTimer.stop()
                guideOverlayClosing = false
                root.setPlayerKeyboardFocusArea("none")
                if (searchField.activeFocus) {
                    interactionFocusTarget.forceActiveFocus()
                }
                if (!guideOverlayMounted) {
                    guideOverlayMounted = true
                    guideOverlayOpen = false
                    Qt.callLater(function() {
                        guideOverlayOpen = true
                        guidePage.prepareForOpen()
                    })
                }
            } else if (guideOverlayMounted && !guideOverlayClosing) {
                guideOverlayMounted = false
                guideOverlayOpen = false
            }

            if (root.shell.activeOverlay === "none") {
                root.updateAutoHide()
            } else if (root.shell.activeOverlay !== "guide") {
                overlayTimer.stop()
                root.setPlayerKeyboardFocusArea("none")
                if (searchField.activeFocus) {
                    interactionFocusTarget.forceActiveFocus()
                }
                root.hideProgramHoverBubble()
            }
        }

        function onOverlaysVisibleChanged() {
            if (root.shell.overlaysVisible) {
                root.updateAutoHide()
                root.scheduleSelectedChannelVisible(ListView.Contain)
            } else {
                if (root.groupPickerOpen) {
                    root.groupPickerOpen = false
                    root.groupPickerSearchText = ""
                    root.groupPickerHighlightedId = ""
                }
                if (root.sourcePickerOpen) {
                    root.sourcePickerOpen = false
                    root.sourcePickerSearchText = ""
                    root.sourcePickerHighlightedId = ""
                }
                root.pendingSourcePickerProfileId = ""
                root.pendingSourcePickerRevealSource = ""
                root.overlayInteractionSource = "none"
                root.exitKeyboardNavigation(true)
                root.previewPinned = false
                root.hoverPreviewActive = false
                root.hideProgramHoverBubble()
                root.keyboardVolumeHudVisible = false
                previewHoldTimer.stop()
                const playbackChannelId = Number(root.player.currentChannel.id)
                if (!Number.isNaN(playbackChannelId)
                    && playbackChannelId >= 0
                    && root.channelList.selectedChannelId !== playbackChannelId) {
                    root.suppressSelectionInteraction = true
                    root.channelList.selectById(playbackChannelId)
                    root.committedChannelId = playbackChannelId
                    Qt.callLater(function() {
                        root.suppressSelectionInteraction = false
                    })
                }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#000000"
    }

    Item {
        id: videoCanvas
        objectName: "ui.region.video_canvas"
        anchors.fill: parent
        visible: root.showVideoCanvas
        clip: true

        Repeater {
            model: root.multiviewVisibleSlots

            delegate: Item {
                required property int index

                readonly property var tileRect: root.multiviewTileRect(index)
                readonly property bool gridMode: root.multiviewGridActive
                readonly property bool tileMetaVisible: root.multiviewActive
                    && (root.shell.overlaysVisible || root.shell.activeOverlay !== "none")
                readonly property bool tileSelectionCursorActive: root.multiviewSelectionMode
                readonly property bool tileFocusedBorderVisible: gridMode
                    && !root.multiviewSelectionMode
                    && Boolean(tileData.isFocused)
                readonly property bool tileSelectionBorderVisible: gridMode
                    && root.multiviewSelectionMode
                    && tileSelectionCursorIndex === index
                readonly property int tileSelectionCursorIndex: tileSelectionCursorActive
                    ? Math.max(0, Math.min(root.multiviewVisibleSlots - 1, root.multiviewSelectionIndex))
                    : Number(root.multiView.focusedTileIndex || 0)
                readonly property var tileData: {
                    const liveTileData = root.multiviewTileData(index)
                    if (liveTileData) {
                        return liveTileData
                    }
                    return {
                        isPrimary: index === 0,
                        isFocused: Number(root.multiView.focusedTileIndex || 0) === index,
                        isEmpty: true,
                        channelName: index === 0 ? "Primary tile" : "Empty tile",
                        playerState: "empty",
                        hasError: false,
                        errorText: "",
                        playerObject: null
                    }
                }
                visible: index < root.multiviewVisibleSlots
                x: tileRect.x
                y: tileRect.y
                width: tileRect.width
                height: tileRect.height
                z: root.multiviewActive && !Boolean(tileData.isPrimary)
                    ? (Boolean(tileData.isFocused) ? 3 : 2)
                    : 1

                Rectangle {
                    anchors.fill: parent
                    color: "#000000"
                    border.width: 0
                    radius: 0
                }

                MpvVideoItem {
                    objectName: "multiviewTileVideo_" + String(index)
                    anchors.fill: parent
                    visible: !Boolean(parent.tileData.isEmpty) && parent.visible
                    playerObject: Number(index) === 0
                        ? root.player.playbackPlayerObject
                        : parent.tileData.playerObject
                }

                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.width: parent.tileFocusedBorderVisible ? 2 : 0
                    border.color: "#3b82f6"
                    radius: 0
                    z: 5
                }

                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.width: parent.tileSelectionBorderVisible ? 3 : 0
                    border.color: "#f59e0b"
                    radius: 0
                    z: 6
                }

                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.width: 0
                    radius: 0
                    visible: Boolean(parent.tileData.isEmpty)

                    Text {
                        anchors.centerIn: parent
                        width: Math.max(120, parent.width - 32)
                        text: root.multiviewPlaceholderText(parent.parent.tileData)
                        color: Theme.textSecondary
                        font.pixelSize: root.multiviewActive ? 15 : 18
                        font.bold: root.multiviewActive
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                    }
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 8
                    anchors.bottomMargin: 8
                    visible: parent.tileMetaVisible && !Boolean(tileData.isEmpty)
                    width: Math.min(parent.width - 16, tileChannelLabel.implicitWidth + 12)
                    height: tileChannelLabel.implicitHeight + 8
                    radius: 4
                    color: "#8e09131c"
                    border.width: 0
                    clip: true
                    z: 6

                    Text {
                        id: tileChannelLabel
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.margins: 6
                        text: tileData.channelName || (tileData.isPrimary ? "Primary tile" : "Secondary tile")
                        color: Theme.textPrimary
                        font.pixelSize: 12
                        font.bold: true
                        elide: Text.ElideRight
                    }
                }

                Text {
                    anchors.centerIn: parent
                    width: Math.max(120, parent.width - 24)
                    visible: Boolean(tileData.hasError)
                    text: tileData.errorText || "Channel couldn't be loaded"
                    color: "#ffb4ac"
                    font.pixelSize: 13
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    style: Text.Outline
                    styleColor: "#b0000000"
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: root.multiviewActive
                    hoverEnabled: true
                    onClicked: root.multiView.focusTile(index)
                }
            }
        }

        Item {
            id: multiviewGridSeparators
            anchors.fill: parent
            visible: root.multiviewGridActive && root.multiviewVisibleSlots > 1
            z: 7
            readonly property int columns: Math.max(1, Number(root.multiView.layoutColumns || 1))
            readonly property int rows: Math.max(1, Number(root.multiView.layoutRows || 1))
            readonly property color separatorColor: "#546577"

            Repeater {
                model: Math.max(0, multiviewGridSeparators.columns - 1)
                delegate: Rectangle {
                    required property int index

                    width: 1
                    height: multiviewGridSeparators.height
                    x: Math.floor(((index + 1) * multiviewGridSeparators.width) / multiviewGridSeparators.columns)
                    y: 0
                    color: multiviewGridSeparators.separatorColor
                }
            }

            Repeater {
                model: Math.max(0, multiviewGridSeparators.rows - 1)
                delegate: Rectangle {
                    required property int index

                    width: multiviewGridSeparators.width
                    height: 1
                    x: 0
                    y: Math.floor(((index + 1) * multiviewGridSeparators.height) / multiviewGridSeparators.rows)
                    color: multiviewGridSeparators.separatorColor
                }
            }
        }

        Rectangle {
            anchors.fill: parent
            color: "#000000"
            visible: root.showChannelSwitchBlackout
            z: 10
        }
    }

    MpvVideoItem {
        objectName: "seamlessStandbyPrewarmVideo"
        x: -2
        y: -2
        width: 1
        height: 1
        visible: root.player.seamlessStandbyPrewarmActive
        opacity: 0
        enabled: false
        playerObject: root.player.seamlessStandbyPlayerObject
    }

    Item {
        id: debugBubble
        objectName: "ui.region.debug_bubble"
        x: root.debugBubbleX
        y: root.debugBubbleY
        width: Math.min(root.width - Theme.spacingL * 2, root.shell.layoutBand === "compact" ? 420 : 560)
        height: Math.ceil(debugBubbleGrid.implicitHeight + (debugBubble.contentVerticalMargin * 2))
        clip: true
        visible: root.debugBubbleVisible
        opacity: visible ? 1 : 0
        z: 1
        property int rowCount: 31
        property real contentHorizontalMargin: 10
        property real contentVerticalMargin: 10
        property real labelColumnWidth: root.shell.layoutBand === "compact" ? 172 : 214
        readonly property real valueColumnWidth: Math.max(
            120,
            debugBubble.width - (debugBubble.contentHorizontalMargin * 2) - debugBubble.labelColumnWidth - 10)
        property real dragOffsetX: 0
        property real dragOffsetY: 0

        onWidthChanged: root.clampDebugBubblePosition()
        onHeightChanged: root.clampDebugBubblePosition()

        Behavior on opacity {
            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
        }

        GlassPanel {
            anchors.fill: parent
            fillColor: "#82070d12"
            strokeColor: "transparent"
        }

        Grid {
            id: debugBubbleGrid
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: debugBubble.contentHorizontalMargin
            anchors.rightMargin: debugBubble.contentHorizontalMargin
            anchors.topMargin: debugBubble.contentVerticalMargin
            rows: debugBubble.rowCount
            columns: 2
            rowSpacing: 4
            columnSpacing: 10

            Text {
                text: "Stream host"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("streamHost")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "Stream ID"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("streamId")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "Streams"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("streamsText")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "Current/Source resolution"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugBubbleCurrentResolution + " / " + root.debugFieldValue("sourceResolution")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "Scanning"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("scanningText")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "Volume"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("volumeText")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "Codec"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("videoCodec") + " / " + root.debugFieldValue("audioCodec")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "Framerate"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("frameRateText")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "Bitrate"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Item {
                width: debugBubble.valueColumnWidth
                implicitWidth: debugBubble.valueColumnWidth
                implicitHeight: debugBitrateChart.height + 2 + debugBitrateValueText.implicitHeight
                clip: true

                Canvas {
                    id: debugBitrateChart
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 18
                    antialiasing: true
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()

                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)

                        ctx.fillStyle = "#1a2c1a"
                        ctx.fillRect(0, 0, width, height)

                        const capacity = Math.max(8, Math.floor(root.debugBitrateSampleCapacity))
                        const samples = root.debugBitrateSamples
                        if (samples.length === 0) {
                            return
                        }

                        const slotWidth = width / capacity
                        const scaleMax = Math.max(1.0, root.debugBitrateScaleMaxKbps)

                        let started = false
                        ctx.strokeStyle = "#4caf50"
                        ctx.lineWidth = 1.5
                        ctx.beginPath()
                        for (let index = 0; index < capacity; ++index) {
                            const sample = Number(samples[index])
                            if (!Number.isFinite(sample) || sample < 0) {
                                started = false
                                continue
                            }

                            const normalized = Math.max(0, Math.min(1, sample / scaleMax))
                            const x = index * slotWidth + slotWidth * 0.5
                            const y = (height - 1) - normalized * (height - 1)
                            if (!started) {
                                ctx.moveTo(x, y)
                                started = true
                            } else {
                                ctx.lineTo(x, y)
                            }
                        }
                        ctx.stroke()

                        const headX = root.debugBitrateWriteIndex * slotWidth
                        ctx.strokeStyle = "#66ffffff"
                        ctx.lineWidth = 1
                        ctx.beginPath()
                        ctx.moveTo(headX + 0.5, 0)
                        ctx.lineTo(headX + 0.5, height)
                        ctx.stroke()
                    }
                }

                Text {
                    id: debugBitrateValueText
                    anchors.left: parent.left
                    anchors.top: debugBitrateChart.bottom
                    anchors.topMargin: 2
                    text: root.debugFieldValue("bitrateText")
                    color: Theme.textPrimary
                    font.pixelSize: 14
                }
            }

            Text {
                text: "Buffer Health"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Item {
                width: debugBubble.valueColumnWidth
                implicitWidth: debugBubble.valueColumnWidth
                implicitHeight: debugBufferChart.height + 2 + debugBufferMetricsColumn.implicitHeight
                clip: true

                Canvas {
                    id: debugBufferChart
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 18
                    antialiasing: true
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()

                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)

                        ctx.fillStyle = "#2c2020"
                        ctx.fillRect(0, 0, width, height)

                        ctx.strokeStyle = "#5e4444"
                        ctx.lineWidth = 1
                        ctx.beginPath()
                        ctx.moveTo(0, Math.floor(height * 0.5) + 0.5)
                        ctx.lineTo(width, Math.floor(height * 0.5) + 0.5)
                        ctx.stroke()

                        const capacity = Math.max(8, Math.floor(root.debugBufferSampleCapacity))
                        const samples = root.debugBufferSamples
                        if (samples.length === 0) {
                            return
                        }

                        const slotWidth = width / capacity
                        const scaleMax = Math.max(0.1, root.debugBufferScaleMaxSeconds)

                        const targetSeconds = Number(root.debugBubbleData.minBufferNeededSeconds)
                        if (Number.isFinite(targetSeconds) && targetSeconds > 0) {
                            const targetNormalized = Math.max(0, Math.min(1, targetSeconds / scaleMax))
                            const targetY = (height - 1) - targetNormalized * (height - 1)
                            ctx.strokeStyle = "#8f9aa8"
                            ctx.lineWidth = 1
                            ctx.beginPath()
                            ctx.moveTo(0, targetY + 0.5)
                            ctx.lineTo(width, targetY + 0.5)
                            ctx.stroke()
                        }

                        let started = false
                        ctx.strokeStyle = "#f6bf58"
                        ctx.lineWidth = 1.5
                        ctx.beginPath()
                        for (let index = 0; index < capacity; ++index) {
                            const sample = Number(samples[index])
                            if (!Number.isFinite(sample) || sample < 0) {
                                started = false
                                continue
                            }

                            const normalized = Math.max(0, Math.min(1, sample / scaleMax))
                            const x = index * slotWidth + slotWidth * 0.5
                            const y = (height - 1) - normalized * (height - 1)
                            if (!started) {
                                ctx.moveTo(x, y)
                                started = true
                            } else {
                                ctx.lineTo(x, y)
                            }
                        }
                        ctx.stroke()

                        const headX = root.debugBufferWriteIndex * slotWidth
                        ctx.strokeStyle = "#66ffffff"
                        ctx.lineWidth = 1
                        ctx.beginPath()
                        ctx.moveTo(headX + 0.5, 0)
                        ctx.lineTo(headX + 0.5, height)
                        ctx.stroke()
                    }
                }

                Column {
                    id: debugBufferMetricsColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: debugBufferChart.bottom
                    anchors.topMargin: 2
                    spacing: 1

                    Text {
                        text: root.debugFieldValue("bufferDurationSourceText") + ": " + root.debugFieldValue("bufferDurationText")
                        color: Theme.textPrimary
                        font.pixelSize: 13
                        width: parent.width
                        elide: Text.ElideRight
                    }

                    Text {
                        text: "TS to edge: " + root.debugFieldValue("timeshiftBufferToLiveText")
                        color: Theme.textSecondary
                        font.pixelSize: 12
                        width: parent.width
                        elide: Text.ElideRight
                    }

                    Text {
                        text: "mpv cache: " + root.debugFieldValue("mpvBufferDurationText")
                        color: Theme.textSecondary
                        font.pixelSize: 12
                        width: parent.width
                        elide: Text.ElideRight
                        visible: root.player.playbackMode !== "catchup"
                    }
                }
            }

            Text {
                text: "Layout"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.multiviewLayoutText
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "Timeshift"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("timeshiftMode")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "Behind Live"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("timeshiftBehindLiveText")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "Window"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("timeshiftWindowText")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "TS Tracks"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("timeshiftTracksText")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "TS Drops"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("timeshiftDroppedSubsText")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "Seekable"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("timeshiftSeekableText")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "TS Attached"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("timeshiftAttachedText")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "TS Point"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("timeshiftCurrentPointText")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "TS Seek"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("timeshiftSeekModeText")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "Focused Tile"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.multiView.tileCount > 0
                    ? String(Number(root.multiView.focusedTileIndex || 0) + 1)
                    : "N/A"
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "Active Tiles"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: String(root.multiView.tileCount || 0) + " / " + String(root.multiView.maxTiles || 1)
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }

            Text {
                text: "Timestamp"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.bold: true
                width: debugBubble.labelColumnWidth
                elide: Text.ElideRight
            }
            Text {
                text: root.debugFieldValue("timestamp")
                color: Theme.textPrimary
                font.pixelSize: 14
                elide: Text.ElideRight
                width: debugBubble.valueColumnWidth
            }
        }

        MouseArea {
            id: debugBubbleDragArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.OpenHandCursor
            onPressed: function(mouse) {
                debugBubble.dragOffsetX = mouse.x
                debugBubble.dragOffsetY = mouse.y
            }
            onPositionChanged: function(mouse) {
                if (!pressed) {
                    return
                }

                const pointInRoot = debugBubbleDragArea.mapToItem(root, mouse.x, mouse.y)
                const minMargin = Theme.spacingL
                const maxX = Math.max(minMargin, root.width - debugBubble.width - minMargin)
                const maxY = Math.max(minMargin, root.height - debugBubble.height - minMargin)
                root.debugBubbleX = Math.max(minMargin, Math.min(pointInRoot.x - debugBubble.dragOffsetX, maxX))
                root.debugBubbleY = Math.max(minMargin, Math.min(pointInRoot.y - debugBubble.dragOffsetY, maxY))
            }
        }
    }

    BusyIndicator {
        anchors.centerIn: parent
        width: 54
        height: 54
        running: root.showPlaybackSpinner
        visible: running
        z: 3
    }

    Item {
        anchors.centerIn: parent
        visible: root.showChannelLoadError
        z: 3
        width: root.shell.layoutBand === "compact" ? 200 : 260
        height: iconBlock.implicitHeight

        Column {
            id: iconBlock
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 14

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                width: root.shell.layoutBand === "compact" ? 128 : 176
                height: width
                fillMode: Image.PreserveAspectFit
                sourceSize.width: Math.round(width * Screen.devicePixelRatio)
                sourceSize.height: Math.round(height * Screen.devicePixelRatio)
                smooth: true
                source: root.iconPath("channel-error.svg")
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Channel couldn't be loaded"
                color: Theme.textPrimary
                font.pixelSize: root.shell.layoutBand === "compact" ? 22 : 28
                font.bold: true
                renderType: Text.NativeRendering
            }
        }
    }

    Item {
        anchors.centerIn: parent
        visible: !root.hasPlaybackChannel
            && !root.multiviewActive
            && !root.multiviewRetainedSelectionVisible
        z: 3
        width: root.shell.layoutBand === "compact" ? 224 : 304
        height: stoppedBlock.implicitHeight

        Column {
            id: stoppedBlock
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 14

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                width: root.shell.layoutBand === "compact" ? 205 : 282
                height: width
                fillMode: Image.PreserveAspectFit
                sourceSize.width: Math.round(width * Screen.devicePixelRatio)
                sourceSize.height: Math.round(height * Screen.devicePixelRatio)
                smooth: true
                source: root.iconPath("app.png")
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Select a channel and start your journey!"
                color: Theme.textPrimary
                font.pixelSize: root.shell.layoutBand === "compact" ? 14 : 16
                font.bold: true
                renderType: Text.NativeRendering
            }
        }
    }

    Item {
        id: interactionFocusTarget
        anchors.fill: parent
        focus: true
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        cursorShape: (!root.showHoverUi && !root.pendingEmptyPipActive) ? Qt.BlankCursor : Qt.ArrowCursor
        onClicked: root.revealUi("pointer")
        onPositionChanged: {
            if (root.chromeAnimationsRunning) {
                return
            }
            root.revealUi("pointer")
        }
    }

    Item {
        id: leftChrome
        objectName: "ui.region.left_pane"
        readonly property bool leftPaneVisible: root.showShellChrome || root.leftPickerOpen || root.pendingEmptyPipActive
        width: root.leftPanelWidth
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.topMargin: root.topBarReservedHeight
        anchors.bottom: parent.bottom
        opacity: leftPaneVisible ? 1 : 0
        enabled: leftPaneVisible && !root.chromeAnimationsRunning
        z: 2

        transform: Translate {
            id: leftChromeShift
            x: leftChrome.leftPaneVisible ? 0 : -leftChrome.width
            Behavior on x {
                NumberAnimation {
                    id: leftChromeSlideAnimation
                    duration: Theme.transitionMs
                    easing.type: Easing.OutCubic
                }
            }
        }

        Behavior on opacity {
            NumberAnimation {
                id: leftChromeOpacityAnimation
                duration: Theme.transitionMs * 0.8
            }
        }

        onVisibleChanged: root.clearLeftPaneHideIfSettled()

        Connections {
            target: leftChromeSlideAnimation

            function onRunningChanged() {
                root.clearLeftPaneHideIfSettled()
            }
        }

        Connections {
            target: leftChromeOpacityAnimation

            function onRunningChanged() {
                root.clearLeftPaneHideIfSettled()
            }
        }

        Rectangle {
            anchors.fill: parent
            color: "#82070d12"
        }

        HoverHandler {
            id: leftChromeHover
            target: leftChrome
            onHoveredChanged: root.updateAutoHide()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 4
            anchors.rightMargin: 8
            anchors.topMargin: Theme.spacingM
            anchors.bottomMargin: Theme.spacingM
            spacing: Theme.spacingS

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 42
                radius: 4
                color: "#960d1822"
                border.width: searchField.activeFocus ? 1 : 0
                border.color: Theme.borderStrong
                visible: root.leftPaneDisplayMode === "channels" && root.hasAnyChannels

                TextField {
                    id: searchField
                    anchors.fill: parent
                    anchors.margins: 1
                    leftPadding: 12
                    rightPadding: 12
                    topPadding: 10
                    bottomPadding: 10
                    text: root.channelList.searchText
                    placeholderText: "Search channels"
                    placeholderTextColor: Theme.textMuted
                    color: Theme.textPrimary
                    font.pixelSize: 14
                    hoverEnabled: true
                    background: Item {}
                    onTextEdited: root.channelList.searchText = text
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Down
                            || event.key === Qt.Key_Tab
                            || event.key === Qt.Key_Return
                            || event.key === Qt.Key_Enter) {
                            event.accepted = true
                            root.noteKeyboardNavigationKey()
                            root.focusTopChannel("keyboard")
                        }
                    }
                    onActiveFocusChanged: {
                        if (activeFocus) {
                            root.setPlayerKeyboardFocusArea("search")
                            root.noteBrowseInteraction()
                        } else if (root.playerKeyboardFocusArea === "search") {
                            root.playerKeyboardFocusArea = "none"
                        }
                        if (!activeFocus && !channelListHover.hovered) {
                            previewHoldTimer.stop()
                        } else if (!channelListHover.hovered) {
                            previewHoldTimer.restart()
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 42
                radius: 4
                color: "#960d1822"
                border.width: groupPickerSearchField.activeFocus ? 1 : 0
                border.color: Theme.borderStrong
                visible: root.leftPaneDisplayMode === "group"

                TextField {
                    id: groupPickerSearchField
                    anchors.fill: parent
                    anchors.margins: 1
                    leftPadding: 12
                    rightPadding: 12
                    topPadding: 10
                    bottomPadding: 10
                    text: root.groupPickerSearchText
                    placeholderText: "Search groups"
                    placeholderTextColor: Theme.textMuted
                    color: Theme.textPrimary
                    font.pixelSize: 14
                    hoverEnabled: true
                    background: Item {}
                    onTextEdited: {
                        root.groupPickerSearchText = text
                        root.ensureGroupPickerHighlight()
                    }
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Down
                            || event.key === Qt.Key_Tab
                            || event.key === Qt.Key_Return
                            || event.key === Qt.Key_Enter) {
                            event.accepted = true
                            root.noteKeyboardNavigationKey()
                            root.focusTopGroupRow("keyboard")
                        }
                    }
                    onActiveFocusChanged: {
                        if (activeFocus) {
                            root.setPlayerKeyboardFocusArea("groupSearch")
                            root.revealUi("keyboard")
                        } else if (root.playerKeyboardFocusArea === "groupSearch") {
                            root.playerKeyboardFocusArea = "none"
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 42
                radius: 4
                color: "#960d1822"
                border.width: sourcePickerSearchField.activeFocus ? 1 : 0
                border.color: Theme.borderStrong
                visible: root.leftPaneDisplayMode === "source"

                TextField {
                    id: sourcePickerSearchField
                    anchors.fill: parent
                    anchors.margins: 1
                    leftPadding: 12
                    rightPadding: 12
                    topPadding: 10
                    bottomPadding: 10
                    text: root.sourcePickerSearchText
                    placeholderText: "Search sources"
                    placeholderTextColor: Theme.textMuted
                    color: Theme.textPrimary
                    font.pixelSize: 14
                    hoverEnabled: true
                    background: Item {}
                    onTextEdited: {
                        root.sourcePickerSearchText = text
                        root.ensureSourcePickerHighlight()
                    }
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Down
                            || event.key === Qt.Key_Tab
                            || event.key === Qt.Key_Return
                            || event.key === Qt.Key_Enter) {
                            event.accepted = true
                            root.noteKeyboardNavigationKey()
                            root.focusTopSourceRow("keyboard")
                        }
                    }
                    onActiveFocusChanged: {
                        if (activeFocus) {
                            root.setPlayerKeyboardFocusArea("sourceSearch")
                            root.revealUi("keyboard")
                        } else if (root.playerKeyboardFocusArea === "sourceSearch") {
                            root.playerKeyboardFocusArea = "none"
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.leftPaneDisplayMode === "group"

                ListView {
                    id: groupPickerView
                    anchors.fill: parent
                    clip: true
                    spacing: 2
                    model: root.groupPickerVisibleRows
                    visible: root.groupPickerVisibleRows.length > 0

                    delegate: Rectangle {
                        required property var modelData

                        width: ListView.view.width
                        height: 62
                        radius: 4
                        color: root.groupPickerHighlightedId === modelData.id ? "#96182431" : "transparent"
                        border.width: 0
                        border.color: "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 8
                            anchors.topMargin: 8
                            anchors.bottomMargin: 8
                            spacing: 8

                            ChannelBadge {
                                badgeSize: 38
                                imageSource: modelData.id === "__favourites__"
                                    ? root.iconPath("favourites.svg")
                                    : root.iconPath("group-placeholder.svg")
                                label: modelData.name
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.name
                                    color: Theme.textPrimary
                                    font.pixelSize: 14
                                    font.bold: true
                                    elide: Text.ElideRight
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.count === 1 ? "1 channel" : modelData.count + " channels"
                                    color: Theme.textSecondary
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            onEntered: root.groupPickerHighlightedId = modelData.id
                            onPositionChanged: root.revealUi("pointer")
                            onClicked: {
                                root.revealUi("pointer")
                                root.groupPickerHighlightedId = modelData.id
                                root.confirmGroupPickerSelection()
                            }
                        }
                    }
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingM
                    spacing: Theme.spacingS
                    visible: root.groupPickerVisibleRows.length === 0

                    Item { Layout.fillHeight: true }

                    Text {
                        Layout.fillWidth: true
                        text: root.liveGroups.totalCount > 0
                            ? "No groups match this search."
                            : "No selected groups are available for this source."
                        color: Theme.textPrimary
                        font.pixelSize: 20
                        font.bold: true
                        wrapMode: Text.Wrap
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.liveGroups.totalCount > 0
                            ? "Try a different group name or clear the filter."
                            : "Select groups in Source Settings before using the live group picker."
                        color: Theme.textSecondary
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }

                    AppButton {
                        text: root.liveGroups.totalCount > 0 ? "Clear Search" : "Open Source Settings"
                        accent: true
                        onClicked: {
                            if (root.liveGroups.totalCount > 0) {
                                root.groupPickerSearchText = ""
                                groupPickerSearchField.forceActiveFocus()
                                root.ensureGroupPickerHighlight()
                            } else {
                                root.closeGroupPicker(false)
                                root.openSettingsOverlay("sources", "pointer")
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.leftPaneDisplayMode === "source"

                ListView {
                    id: sourcePickerView
                    anchors.fill: parent
                    clip: true
                    spacing: 2
                    model: root.sourcePickerVisibleRows
                    visible: root.sourcePickerVisibleRows.length > 0

                    delegate: Rectangle {
                        required property var modelData

                        width: ListView.view.width
                        height: 62
                        radius: 4
                        color: root.sourcePickerHighlightedId === modelData.id ? "#96182431" : "transparent"
                        border.width: 0
                        border.color: "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 8
                            anchors.topMargin: 8
                            anchors.bottomMargin: 8
                            spacing: 8

                            ChannelBadge {
                                badgeSize: 38
                                imageSource: root.iconPath("sources.svg")
                                label: modelData.name
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.name
                                    color: Theme.textPrimary
                                    font.pixelSize: 14
                                    font.bold: true
                                    elide: Text.ElideRight
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.subtitle
                                    color: Theme.textSecondary
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            onEntered: root.sourcePickerHighlightedId = modelData.id
                            onPositionChanged: root.revealUi("pointer")
                            onClicked: {
                                root.revealUi("pointer")
                                root.sourcePickerHighlightedId = modelData.id
                                root.confirmSourcePickerSelection("pointer")
                            }
                        }
                    }
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingM
                    spacing: Theme.spacingS
                    visible: root.sourcePickerVisibleRows.length === 0

                    Item { Layout.fillHeight: true }

                    Text {
                        Layout.fillWidth: true
                        text: "No sources match this search."
                        color: Theme.textPrimary
                        font.pixelSize: 20
                        font.bold: true
                        wrapMode: Text.Wrap
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "Try a different source name or clear the filter."
                        color: Theme.textSecondary
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }

                    AppButton {
                        text: "Clear Search"
                        accent: true
                        onClicked: {
                            root.sourcePickerSearchText = ""
                            sourcePickerSearchField.forceActiveFocus()
                            root.ensureSourcePickerHighlight()
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.leftPaneDisplayMode === "audio"

                ListView {
                    id: audioPickerView
                    anchors.fill: parent
                    clip: true
                    spacing: 2
                    model: root.audioPickerRows

                    delegate: Rectangle {
                        required property var modelData

                        width: ListView.view.width
                        height: 62
                        radius: 4
                        color: root.audioPickerHighlightedId === modelData.id ? "#96182431" : "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 8
                            anchors.topMargin: 8
                            anchors.bottomMargin: 8
                            spacing: 8

                            ChannelBadge {
                                badgeSize: 38
                                imageSource: root.iconPath("audio-track.svg")
                                label: modelData.name
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.name
                                    color: Theme.textPrimary
                                    font.pixelSize: 14
                                    font.bold: true
                                    elide: Text.ElideRight
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.subtitle
                                    color: Theme.textSecondary
                                    font.pixelSize: 11
                                    font.italic: true
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            onEntered: root.audioPickerHighlightedId = modelData.id
                            onPositionChanged: root.revealUi("pointer")
                            onClicked: {
                                root.revealUi("pointer")
                                root.audioPickerHighlightedId = modelData.id
                                root.confirmAudioPickerSelection()
                            }
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.leftPaneDisplayMode === "subtitle"

                ListView {
                    id: subtitlePickerView
                    anchors.fill: parent
                    clip: true
                    spacing: 2
                    model: root.subtitlePickerRows

                    delegate: Rectangle {
                        required property var modelData

                        width: ListView.view.width
                        height: 62
                        radius: 4
                        color: root.subtitlePickerHighlightedId === modelData.id ? "#96182431" : "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 8
                            anchors.topMargin: 8
                            anchors.bottomMargin: 8
                            spacing: 8

                            ChannelBadge {
                                badgeSize: 38
                                imageSource: root.iconPath("closed-caption.svg")
                                label: modelData.name
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.name
                                    color: Theme.textPrimary
                                    font.pixelSize: 14
                                    font.bold: true
                                    elide: Text.ElideRight
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.subtitle
                                    color: Theme.textSecondary
                                    font.pixelSize: 11
                                    font.italic: true
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            onEntered: root.subtitlePickerHighlightedId = modelData.id
                            onPositionChanged: root.revealUi("pointer")
                            onClicked: {
                                root.revealUi("pointer")
                                root.subtitlePickerHighlightedId = modelData.id
                                root.confirmSubtitlePickerSelection()
                            }
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.leftPaneDisplayMode === "channels" && root.hasAnyChannels && root.hasChannelList

                HoverHandler {
                    id: channelListHover
                    target: channelListContainer
                    onHoveredChanged: {
                        if (hovered) {
                            root.noteBrowseInteraction()
                        } else if (!searchField.activeFocus && !searchField.hovered) {
                            // Keep browse selection stable when pointer leaves the pane.
                            // Default-view reset happens when overlays are hidden.
                            previewHoldTimer.stop()
                        }
                    }
                }

                Item {
                    id: channelListContainer
                    anchors.fill: parent
                }

                ListView {
                    id: channelView
                    anchors.fill: parent
                    clip: true
                    spacing: 2
                    cacheBuffer: 620
                    reuseItems: true
                    model: root.channelList
                    onMovingChanged: root.updateChannelListScrollState()
                    onDraggingChanged: root.updateChannelListScrollState()
                    onFlickingChanged: root.updateChannelListScrollState()
                    ScrollBar.vertical: ScrollBar {
                        id: channelViewScrollBar
                        policy: channelView.contentHeight > channelView.height
                            ? ScrollBar.AlwaysOn
                            : ScrollBar.AlwaysOff
                        interactive: true
                        width: 6
                        z: 3
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        padding: 0
                        onPressedChanged: root.updateChannelListScrollState()
                        background: Rectangle {
                            implicitWidth: 6
                            radius: width / 2
                            color: "#2a20364d"
                        }
                        contentItem: Rectangle {
                            implicitWidth: 6
                            radius: width / 2
                            color: channelViewScrollBar.pressed
                                ? Theme.borderStrong
                                : "#8c4e88b8"
                        }
                    }

                    delegate: Rectangle {
                        property int channelId: model.id
                        property string channelName: model.name
                        property string channelCachedIconPath: model.cachedIconPath
                        property bool channelIsSelected: model.isSelected
                        property bool channelIsFavorite: model.isFavorite
                        property bool channelIsDvrRecording: model.isDvrRecording
                        property string channelProgramTitle: model.currentProgramTitle
                        property string channelProgramTimeRange: model.currentProgramTimeRange
                        property bool rowHovered: false

                        width: ListView.view.width
                        height: 62
                        radius: 4
                        color: channelIsSelected ? "#96182431" : (rowHovered ? "#6d111a24" : "transparent")
                        border.width: 0
                        border.color: "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 4
                            anchors.rightMargin: 8
                            anchors.topMargin: 8
                            anchors.bottomMargin: 8
                            spacing: 8

                            Text {
                                Layout.preferredWidth: 22
                                text: model.source === "M3U"
                                    ? Math.max(1, model.sortOrder)
                                    : (model.sortOrder > 0 ? model.sortOrder : index + 1)
                                color: channelIsSelected ? Theme.textPrimary : Theme.textSecondary
                                font.pixelSize: 12
                                font.bold: channelIsSelected
                                horizontalAlignment: Text.AlignRight
                                verticalAlignment: Text.AlignVCenter
                            }

                            ChannelBadge {
                                badgeSize: 38
                                showFrame: false
                                sourcePath: channelCachedIconPath
                                label: channelName
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1

                                Text {
                                    Layout.fillWidth: true
                                    text: channelName
                                    color: Theme.textPrimary
                                    font.pixelSize: 14
                                    font.bold: true
                                    elide: Text.ElideRight
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: channelProgramTitle || "No programme data"
                                    color: channelProgramTitle.length > 0 ? Theme.textSecondary : Theme.textMuted
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: channelProgramTimeRange
                                    visible: channelProgramTimeRange.length > 0
                                    color: Theme.textMuted
                                    font.pixelSize: 10
                                    elide: Text.ElideRight
                                }
                            }

                            Item {
                                Layout.preferredWidth: channelIsDvrRecording ? 10 : 0
                                Layout.preferredHeight: 10
                                Layout.alignment: Qt.AlignVCenter
                                visible: channelIsDvrRecording

                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 10
                                    height: 10
                                    radius: 5
                                    color: "#ff3a3a"
                                }
                            }

                            Item {
                                Layout.preferredWidth: channelIsFavorite ? 16 : 0
                                Layout.preferredHeight: 16
                                Layout.alignment: Qt.AlignVCenter
                                visible: channelIsFavorite

                                Image {
                                    anchors.fill: parent
                                    fillMode: Image.PreserveAspectFit
                                    source: channelIsFavorite ? root.iconPath("favourites.svg") : ""
                                    sourceSize.width: 16
                                    sourceSize.height: 16
                                }
                            }

                        }

                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            hoverEnabled: true
                            onEntered: {
                                rowHovered = true
                                root.noteBrowseInteraction()
                                if (root.channelListScrollingActive) {
                                    root.queueHoverPreviewChannel(channelId)
                                } else {
                                    root.previewChannel(channelId)
                                }
                            }
                            onExited: rowHovered = false
                            onPositionChanged: root.noteBrowseInteraction()
                            onClicked: function(mouse) {
                                root.noteBrowseInteraction()
                                root.pendingHoverPreviewChannelId = -1
                                root.commitChannelSelection(channelId)
                                if (mouse.button === Qt.RightButton) {
                                    root.channelList.toggleFavorite(channelId)
                                } else if (mouse.button === Qt.LeftButton) {
                                    root.activateChannelById(channelId)
                                }
                            }
                        }

                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.leftPaneDisplayMode === "channels" && root.hasAnyChannels && !root.hasChannelList

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingM
                    spacing: Theme.spacingS

                    Item { Layout.fillHeight: true }

                    Text {
                        Layout.fillWidth: true
                        text: root.channelList.searchText.length > 0
                            ? "No channels match this search."
                            : "This source has no channels ready for live playback."
                        color: Theme.textPrimary
                        font.pixelSize: 20
                        font.bold: true
                        wrapMode: Text.Wrap
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.channelList.searchText.length > 0
                            ? "Try a different channel name or clear the filter."
                            : root.app.statusText
                        color: Theme.textSecondary
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }

                    AppButton {
                        text: root.channelList.searchText.length > 0 ? "Clear Search" : "Open Source Settings"
                        accent: true
                        onClicked: {
                            if (root.channelList.searchText.length > 0) {
                                root.channelList.searchText = ""
                                searchField.forceActiveFocus()
                            } else {
                                root.openSettingsOverlay("sources", "pointer")
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.leftPaneDisplayMode === "channels" && !root.hasAnyChannels

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingM
                    spacing: Theme.spacingM

                    Item { Layout.fillHeight: true }

                    Text {
                        Layout.fillWidth: true
                        text: root.hasActiveProfile
                            ? "This source has no channels ready for live playback."
                            : "Move the mouse to reveal setup, then add or activate a source."
                        color: Theme.textPrimary
                        font.pixelSize: 20
                        font.bold: true
                        wrapMode: Text.Wrap
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.app.statusText || "Use source settings to add a playlist, Xtream account, or M3U file."
                        color: Theme.textSecondary
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }

                    AppButton {
                        text: root.hasActiveProfile ? "Open Source Settings" : "Add Source"
                        accent: true
                        onClicked: root.openSettingsOverlay("sources", "pointer")
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }
    }

    IconActionButton {
        id: guideButtonChrome
        width: (root.shell.layoutBand === "compact" ? 52 : 56) * 4
        height: 48
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: root.topOverlayMargin
        opacity: root.showShellChrome ? 1 : 0
        z: 2
        compact: true
        borderless: true
        glassMode: true
        iconInset: 1
        iconSource: root.iconPath("epg-show.svg")
        caption: "Guide"
        enabled: root.hasAnyChannels && root.showShellChrome && !root.chromeAnimationsRunning
        onClicked: root.openGuideOverlay("pointer")

        HoverHandler {
            id: guideButtonHover
            target: guideButtonChrome
            onHoveredChanged: root.updateAutoHide()
        }

        transform: Translate {
            y: root.showShellChrome ? 0 : -(guideButtonChrome.height + Theme.spacingL)
            Behavior on y {
                NumberAnimation {
                    id: guideButtonSlideAnimation
                    duration: Theme.transitionMs
                    easing.type: Easing.OutCubic
                }
            }
        }

        Behavior on opacity {
            NumberAnimation {
                id: guideButtonOpacityAnimation
                duration: Theme.transitionMs * 0.8
            }
        }
    }

    Item {
        id: rightChrome
        objectName: "ui.region.right_pane"
        width: root.rightPanelWidth
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: root.topBarReservedHeight
        anchors.bottom: parent.bottom
        opacity: root.showShellChrome ? 1 : 0
        enabled: root.showShellChrome && !root.chromeAnimationsRunning
        z: 2

        transform: Translate {
            id: rightChromeShift
            x: root.showShellChrome ? 0 : rightChrome.width
            Behavior on x {
                NumberAnimation {
                    id: rightChromeSlideAnimation
                    duration: Theme.transitionMs
                    easing.type: Easing.OutCubic
                }
            }
        }

        Behavior on opacity {
            NumberAnimation {
                id: rightChromeOpacityAnimation
                duration: Theme.transitionMs * 0.8
            }
        }

        Rectangle {
            anchors.fill: parent
            color: "#78070d12"
        }

        HoverHandler {
            id: rightChromeHover
            target: rightChrome
            onHoveredChanged: root.updateAutoHide()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.topMargin: 14
            anchors.bottomMargin: 14
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    Layout.preferredWidth: 4
                    Layout.preferredHeight: 24
                    radius: 2
                    color: Theme.accent
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1

                    Text {
                        Layout.fillWidth: true
                        text: root.sideNowNextModel.channelName || (root.sideUsesBrowseModel ? "Selected Channel" : "Now Playing")
                        color: Theme.textPrimary
                        font.pixelSize: 17
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.sideUsesBrowseModel ? "Preview EPG" : "Live EPG"
                        color: Theme.textMuted
                        font.pixelSize: 11
                    }
                }

                Rectangle {
                    id: settingsButtonFrame
                    Layout.preferredWidth: 46
                    Layout.preferredHeight: 46
                    radius: 8
                    color: settingsButton.down ? "#ad1f2d3a" : (settingsButton.hovered ? "#a71c2936" : "#96182431")

                    IconActionButton {
                        id: settingsButton
                        anchors.fill: parent
                        compact: true
                        borderless: true
                        barMode: true
                        iconInset: 2
                        iconSource: root.iconPath("settings.svg")
                        caption: "Settings"
                        onClicked: root.openSettingsOverlay("", "pointer")
                    }
                }
            }

            Item {
                id: rightPaneBody
                Layout.fillWidth: true
                Layout.fillHeight: true

                Column {
                    id: rightPaneTopStack
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    spacing: 10

                    Rectangle {
                        id: nowProgramCard
                        width: parent.width
                        height: visible ? implicitHeight : 0
                        implicitHeight: Math.max(94, nowCardContent.implicitHeight + 24)
                        property bool activeState: root.rightPaneProgramActive("now", -1, nowProgramCard)
                        color: activeState ? "#ad1f2d3a" : "#96182431"
                        radius: 6
                        visible: root.sideHasCurrentProgram

                        ColumnLayout {
                            id: nowCardContent
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                Rectangle {
                                    color: Theme.accent
                                    radius: 4
                                    implicitWidth: 48
                                    implicitHeight: 24

                                    Text {
                                        anchors.centerIn: parent
                                        text: "NOW"
                                        color: Theme.textPrimary
                                        font.pixelSize: 11
                                        font.bold: true
                                    }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: root.sideNowNextModel.currentProgram.timeRange || root.currentClockText
                                    color: Theme.textSecondary
                                    font.pixelSize: 12
                                }

                                Rectangle {
                                    Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                                    Layout.preferredWidth: 9
                                    Layout.preferredHeight: 9
                                    radius: 4.5
                                    color: "#ff3a3a"
                                    visible: root.isProgramDvrMarked(
                                        root.sideNowNextModel.currentProgram,
                                        root.sideDvrChannelData())
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: root.sideNowNextModel.currentProgram.title || "No current programme"
                                color: Theme.textPrimary
                                font.pixelSize: 15
                                font.bold: true
                                wrapMode: Text.Wrap
                                maximumLineCount: root.programEpisodeTitle(root.sideNowNextModel.currentProgram).length > 0 ? 1 : 2
                                elide: Text.ElideRight
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: root.programEpisodeTitle(root.sideNowNextModel.currentProgram).length > 0
                                text: root.programEpisodeTitle(root.sideNowNextModel.currentProgram)
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                font.italic: true
                                elide: Text.ElideRight
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 4
                                radius: 2
                                color: "#283542"
                                visible: (root.sideNowNextModel.currentProgram.progressPercent || 0) > 0

                                Rectangle {
                                    width: parent.width * ((root.sideNowNextModel.currentProgram.progressPercent || 0) / 100)
                                    height: parent.height
                                    radius: parent.radius
                                    color: Theme.accent
                                }
                            }
                        }

                        HoverHandler {
                            target: nowProgramCard
                            acceptedDevices: PointerDevice.Mouse
                            onHoveredChanged: {
                                if (hovered) {
                                    root.showProgramHoverBubble(root.sideNowNextModel.currentProgram, nowProgramCard)
                                } else {
                                    root.releaseProgramHoverBubble(nowProgramCard)
                                }
                            }
                        }
                    }

                    Rectangle {
                        id: nextProgramCard
                        width: parent.width
                        height: visible ? implicitHeight : 0
                        implicitHeight: Math.max(86, nextCardContent.implicitHeight + 24)
                        property bool activeState: root.rightPaneProgramActive("next", -1, nextProgramCard)
                        color: activeState ? "#ad1f2d3a" : "#96182431"
                        radius: 6
                        visible: root.sideHasNextProgram

                        ColumnLayout {
                            id: nextCardContent
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                Rectangle {
                                    color: "#273244"
                                    radius: 4
                                    implicitWidth: 52
                                    implicitHeight: 24

                                    Text {
                                        anchors.centerIn: parent
                                        text: "NEXT"
                                        color: Theme.textSecondary
                                        font.pixelSize: 11
                                        font.bold: true
                                    }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: root.sideNowNextModel.nextProgram.timeRange || ""
                                    color: Theme.textSecondary
                                    font.pixelSize: 12
                                }

                                Rectangle {
                                    Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                                    Layout.preferredWidth: 9
                                    Layout.preferredHeight: 9
                                    radius: 4.5
                                    color: "#ff3a3a"
                                    visible: root.isProgramDvrMarked(
                                        root.sideNowNextModel.nextProgram,
                                        root.sideDvrChannelData())
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: root.sideNowNextModel.nextProgram.title || "No upcoming programme"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                                wrapMode: Text.Wrap
                                maximumLineCount: root.programEpisodeTitle(root.sideNowNextModel.nextProgram).length > 0 ? 1 : 2
                                elide: Text.ElideRight
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: root.programEpisodeTitle(root.sideNowNextModel.nextProgram).length > 0
                                text: root.programEpisodeTitle(root.sideNowNextModel.nextProgram)
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                font.italic: true
                                elide: Text.ElideRight
                            }
                        }

                        HoverHandler {
                            target: nextProgramCard
                            acceptedDevices: PointerDevice.Mouse
                            onHoveredChanged: {
                                if (hovered) {
                                    root.showProgramHoverBubble(root.sideNowNextModel.nextProgram, nextProgramCard)
                                } else {
                                    root.releaseProgramHoverBubble(nextProgramCard)
                                }
                            }
                        }
                    }

                    Text {
                        width: parent.width
                        visible: !root.sideHasAnyEpgData
                        text: "Programme data is not available for this channel right now."
                        color: Theme.textMuted
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                }

                ListView {
                    id: upcomingProgramsView
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: rightPaneTopStack.bottom
                    anchors.topMargin: (root.sideHasCurrentProgram || root.sideHasNextProgram) && root.sideHasUpcomingPrograms ? 10 : 0
                    anchors.bottom: parent.bottom
                    clip: true
                    spacing: 0
                    model: root.sideNowNextModel.upcomingPrograms
                    interactive: false
                    visible: root.sideHasUpcomingPrograms
                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AlwaysOff
                    }

                    delegate: Item {
                        id: upcomingProgramRow
                        property bool activeState: root.rightPaneProgramActive("upcoming", index, upcomingProgramRow)
                        width: ListView.view.width
                        height: root.rightPaneUpcomingRowHeight

                        Rectangle {
                            anchors.fill: parent
                            color: activeState ? "#96182431" : "transparent"
                            radius: 4
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 2
                            anchors.rightMargin: 2
                            spacing: 2

                            Text {
                                Layout.preferredWidth: 40
                                text: modelData.startTimeLabel || ""
                                color: Theme.textSecondary
                                font.pixelSize: 10
                            }

                            Rectangle {
                                Layout.alignment: Qt.AlignVCenter
                                Layout.preferredWidth: 8
                                Layout.preferredHeight: 8
                                radius: 4
                                color: "#ff3a3a"
                                visible: root.isProgramDvrMarked(modelData, root.sideDvrChannelData())
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.title
                                    color: Theme.textPrimary
                                    font.pixelSize: 11
                                    font.bold: true
                                    wrapMode: Text.Wrap
                                    maximumLineCount: root.programEpisodeTitle(modelData).length > 0 ? 1 : 2
                                    elide: Text.ElideRight
                                }

                                Text {
                                    Layout.fillWidth: true
                                    visible: root.programEpisodeTitle(modelData).length > 0
                                    text: root.programEpisodeTitle(modelData)
                                    color: Theme.textSecondary
                                    font.pixelSize: 11
                                    font.italic: true
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: "#263240"
                            opacity: 0.85
                        }

                        HoverHandler {
                            target: upcomingProgramRow
                            acceptedDevices: PointerDevice.Mouse
                            onHoveredChanged: {
                                if (hovered) {
                                    root.showProgramHoverBubble(modelData, upcomingProgramRow)
                                } else {
                                    root.releaseProgramHoverBubble(upcomingProgramRow)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    EpgHoverBubble {
        id: epgHoverBubble
        visible: root.hoverBubbleVisible
            && root.showShellChrome
            && root.shell.activeOverlay === "none"
            && root.hoverAnchorItem !== null
        opacity: visible ? 1 : 0
        z: 4
        programData: root.hoveredProgramData
        maxWidth: Math.max(220, Math.min(360, root.width - root.rightPanelWidth - Theme.spacingM))

        onHoveredChanged: {
            if (hovered) {
                hoverBubbleHideTimer.stop()
            } else if (!root.hoverBubbleRowActive && !root.keyboardProgramBubbleActive) {
                hoverBubbleHideTimer.restart()
            }
        }

        onVisibleChanged: {
            if (visible) {
                Qt.callLater(function() {
                    root.repositionProgramHoverBubble()
                })
            }
        }

        onWidthChanged: root.repositionProgramHoverBubble()
        onHeightChanged: root.repositionProgramHoverBubble()

        Behavior on opacity {
            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
        }
    }

    Item {
        id: keyboardBubbleAnchor
        visible: false
        width: 0
        height: 0
    }

    Item {
        id: hoverBubbleBridge
        visible: root.hoverBubbleVisible
            && root.showShellChrome
            && root.shell.activeOverlay === "none"
            && root.hoverAnchorItem !== null
            && root.hoverBubbleBridgeWidth > 0
        x: rightChrome.x
        y: root.hoverBubbleBridgeY
        width: root.hoverBubbleBridgeWidth
        height: root.hoverBubbleBridgeHeight
        z: 4

        HoverHandler {
            id: hoverBubbleBridgeHover
            target: hoverBubbleBridge
            acceptedDevices: PointerDevice.Mouse
            onHoveredChanged: {
                if (hovered) {
                    hoverBubbleHideTimer.stop()
                } else if (!root.hoverBubbleRowActive && !root.keyboardProgramBubbleActive && !epgHoverBubble.hovered) {
                    hoverBubbleHideTimer.restart()
                }
            }
        }
    }

    Item {
        id: bottomChrome
        objectName: "ui.region.bottom_controls"
        property bool bottomChromeInputEnabled: root.showShellChrome && !root.chromeAnimationsRunning
        width: root.bottomPanelWidth
        height: {
            const buttonRowHeight = bottomChrome.mediaButtonSize
            const verticalPadding = bottomChrome.controlsTopPadding + bottomChrome.controlsBottomPadding
            if (!root.transportTimelineActive) {
                return buttonRowHeight + verticalPadding
            }
            const noticeHeight = bottomChrome.timeshiftNoticeVisible ? 22 : 0
            const verticalGapCount = bottomChrome.timeshiftNoticeVisible ? 2 : 1
            return buttonRowHeight
                + verticalPadding
                + bottomChrome.timeshiftTimelineBlockHeight
                + noticeHeight
                + bottomChrome.controlsVerticalGap * verticalGapCount
        }
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spacingM
        opacity: root.showShellChrome ? 1 : 0
        enabled: bottomChrome.bottomChromeInputEnabled
        z: 2

        onBottomChromeInputEnabledChanged: {
            if (!bottomChrome.bottomChromeInputEnabled) {
                root.timeshiftTimelineHoverVisible = false
                volumePopoverHideTimer.stop()
            }
        }

        transform: Translate {
            y: root.showShellChrome ? 0 : (bottomChrome.height + Theme.spacingM)
            Behavior on y {
                NumberAnimation {
                    id: bottomChromeSlideAnimation
                    duration: Theme.transitionMs
                    easing.type: Easing.OutCubic
                }
            }
        }

        Behavior on opacity {
            NumberAnimation {
                id: bottomChromeOpacityAnimation
                duration: Theme.transitionMs * 0.8
            }
        }

        // Keep the transport controls stable when the panel grows for the timeshift timeline.
        readonly property int mediaButtonSize: root.shell.layoutBand === "compact" ? 38 : 42
        readonly property int controlsTopPadding: root.transportTimelineActive ? 8 : 4
        readonly property int controlsBottomPadding: 4
        readonly property int controlsVerticalGap: root.transportTimelineActive ? 3 : 0
        readonly property bool timeshiftNoticeVisible: (root.transportTimelineUsingCatchup
                ? root.player.catchupTimelineNoticeText
                : (root.transportTimelineUsingLiveProgram
                    ? root.liveTimelineNoticeText
                    : root.player.timeshiftNoticeText)).length > 0
        readonly property int timeshiftTimelineBlockHeight: 38

        GlassPanel {
            anchors.fill: parent
            fillColor: "#82070d12"
            strokeColor: "transparent"
            radiusSize: 8
        }

        HoverHandler {
            id: bottomChromeHover
            target: bottomChrome
            onHoveredChanged: root.updateAutoHide()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            anchors.topMargin: bottomChrome.controlsTopPadding
            anchors.bottomMargin: bottomChrome.controlsBottomPadding
            spacing: bottomChrome.controlsVerticalGap

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: bottomChrome.timeshiftNoticeVisible ? 22 : 0
                visible: bottomChrome.timeshiftNoticeVisible

                Rectangle {
                    anchors.fill: parent
                    radius: 6
                    color: "#b51d5a7a"
                    border.width: 1
                    border.color: "#7fd4f3"

                        Text {
                            anchors.centerIn: parent
                            width: parent.width - 16
                            text: root.transportTimelineUsingCatchup
                                ? root.player.catchupTimelineNoticeText
                                : (root.transportTimelineUsingLiveProgram
                                    ? root.liveTimelineNoticeText
                                    : root.player.timeshiftNoticeText)
                            color: "#ffffff"
                        font.pixelSize: 11
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                        renderType: Text.NativeRendering
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: root.transportTimelineActive ? bottomChrome.timeshiftTimelineBlockHeight : 0
                visible: root.transportTimelineActive

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter
                            visible: root.transportTimelineUsingTimeshift
                            Layout.preferredWidth: visible ? implicitWidth : 0
                            radius: 11
                            color: root.timeshiftBadgeShowBehindLive ? "#4f5a64" : "#c62828"
                            implicitWidth: 72
                            implicitHeight: 22

                            Text {
                                id: statusText
                                anchors.centerIn: parent
                                anchors.horizontalCenterOffset: -1
                                anchors.verticalCenterOffset: -1
                                text: "LIVE"
                                color: "#ffffff"
                                font.pixelSize: 12
                                font.bold: true
                                renderType: Text.NativeRendering
                            }

                            MouseArea {
                                anchors.fill: parent
                                enabled: root.timeshiftBadgeShowBehindLive
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: root.jumpToLiveEdgeWithBadge()
                            }
                        }

                        Text {
                            Layout.alignment: Qt.AlignVCenter
                            text: root.formatTimeshiftClock(root.transportTimelineUsingCatchup
                                ? root.player.catchupTimelineStartEpochMs
                                : (root.transportTimelineUsingTimeshift
                                    ? root.player.timeshiftWindowStartEpochMs
                                    : Date.parse(String((root.playbackNowNext.currentProgram || {}).start || ""))))
                            color: Theme.textSecondary
                            font.pixelSize: 11
                            renderType: Text.NativeRendering
                        }

                        Text {
                            Layout.alignment: Qt.AlignVCenter
                            Layout.fillWidth: true
                            visible: root.transportTimelineUsingCatchup || root.transportTimelineUsingLiveProgram
                            text: root.transportTimelineUsingCatchup
                                ? (root.player.catchupProgramLabel || "")
                                : String((root.playbackNowNext.currentProgram || {}).title || "")
                            color: Theme.textPrimary
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignHCenter
                            renderType: Text.NativeRendering
                        }

                        Item {
                            Layout.fillWidth: root.transportTimelineUsingTimeshift
                            Layout.preferredWidth: root.transportTimelineUsingTimeshift ? undefined : 0
                        }

                        Text {
                            Layout.alignment: Qt.AlignVCenter
                            text: root.formatTimeshiftClock(root.transportTimelineUsingCatchup
                                ? root.player.catchupTimelineAvailableEdgeEpochMs
                                : (root.transportTimelineUsingTimeshift
                                    ? root.player.timeshiftLiveEdgeEpochMs
                                    : Math.min(Date.now(), Date.parse(String((root.playbackNowNext.currentProgram || {}).stop || "")))))
                            color: Theme.textSecondary
                            font.pixelSize: 11
                            renderType: Text.NativeRendering
                        }

                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter
                            visible: root.transportTimelineUsingCatchup || root.transportTimelineUsingLiveProgram
                            radius: 11
                            implicitWidth: 86
                            implicitHeight: 22
                            color: root.transportTimelineUsingCatchup
                                ? "#5e656d"
                                : (root.timeshiftBadgeShowBehindLive ? "#4f5a64" : "#c62828")
                            border.width: root.transportTimelineUsingCatchup ? 1 : 0
                            border.color: root.transportTimelineUsingCatchup ? "#7a828b" : "transparent"

                            Text {
                                anchors.centerIn: parent
                                text: root.transportTimelineUsingCatchup
                                    ? "GO LIVE"
                                    : (root.timeshiftBadgeShowBehindLive ? "GO LIVE" : "LIVE")
                                color: "#ffffff"
                                font.pixelSize: 12
                                font.bold: true
                                renderType: Text.NativeRendering
                            }

                            MouseArea {
                                anchors.fill: parent
                                enabled: root.transportTimelineUsingCatchup || root.timeshiftBadgeShowBehindLive
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: {
                                    if (root.transportTimelineUsingCatchup) {
                                        root.player.returnToLiveFromCatchup()
                                    } else {
                                        root.jumpToLiveEdgeWithBadge()
                                    }
                                }
                            }
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 12

                        Rectangle {
                            id: timeshiftTrack
                            objectName: "ui.region.timeshift_timeline"
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            height: 6
                            radius: 3
                            color: "#36454f"

                            Rectangle {
                                width: root.timeshiftBadgeShowBehindLive
                                    ? (timeshiftThumb.x + timeshiftThumb.width * 0.5)
                                    : parent.width
                                height: parent.height
                                radius: parent.radius
                                color: "#a9d8ff"
                            }

                            Rectangle {
                                id: timeshiftThumb
                                width: 12
                                height: 12
                                radius: 6
                                color: "#ffffff"
                                border.width: 1
                                border.color: "#26445b"
                                anchors.verticalCenter: parent.verticalCenter
                                x: {
                                    const available = Math.max(1, timeshiftTrack.width - width)
                                    if (!root.timeshiftBadgeShowBehindLive) {
                                        return available
                                    }
                                    const program = root.playbackNowNext.currentProgram || ({})
                                    const startMs = Date.parse(String(program.start || ""))
                                    const stopMs = Date.parse(String(program.stop || ""))
                                    const liveProgramTotal = (Number.isFinite(startMs) && Number.isFinite(stopMs) && stopMs > startMs)
                                        ? Math.max(0, (Math.min(Date.now(), stopMs) - startMs) / 1000.0)
                                        : 0
                                    const total = root.transportTimelineUsingCatchup
                                        ? Number(root.player.catchupTimelineAvailableSeconds || 0)
                                        : (root.transportTimelineUsingTimeshift
                                            ? Number(root.player.timeshiftAvailableSeconds || 0)
                                            : liveProgramTotal)
                                    const current = root.transportTimelineUsingCatchup
                                        ? Number(root.player.catchupTimelinePositionSeconds || 0)
                                        : (root.transportTimelineUsingTimeshift
                                            ? Number(root.player.timeshiftPositionSeconds || 0)
                                            : Math.max(0, liveProgramTotal - Number(root.player.liveBufferBehindLiveSeconds || 0)))
                                    const fraction = total > 0 ? Math.max(0, Math.min(1, current / total)) : 1
                                    return available * fraction
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true

                                function updateHover(mouseX) {
                                    root.timeshiftTimelineHoverFraction = Math.max(0, Math.min(1, mouseX / Math.max(1, width)))
                                    root.timeshiftTimelineHoverVisible = true
                                }

                                function updateSeek(mouseX) {
                                    updateHover(mouseX)
                                    root.seekTimeshiftToFractionWithBadge(root.timeshiftTimelineHoverFraction)
                                }

                                onPressed: function(mouse) { updateHover(mouse.x) }
                                onPositionChanged: function(mouse) {
                                    updateHover(mouse.x)
                                }
                                onExited: root.timeshiftTimelineHoverVisible = false
                                onReleased: function(mouse) {
                                    updateSeek(mouse.x)
                                    root.timeshiftTimelineHoverVisible = containsMouse
                                }
                            }

                            Rectangle {
                                visible: root.timeshiftTimelineHoverVisible
                                anchors.bottom: parent.top
                                anchors.bottomMargin: 6
                                x: Math.max(0, Math.min(parent.width - width, parent.width * root.timeshiftTimelineHoverFraction - width * 0.5))
                                radius: 6
                                color: "#d9131a22"
                                border.width: 1
                                border.color: "#4dffffff"
                                implicitWidth: hoverTimeText.implicitWidth + 12
                                implicitHeight: hoverTimeText.implicitHeight + 8

                                Text {
                                    id: hoverTimeText
                                    anchors.centerIn: parent
                                    text: root.timeshiftHoverClockText(root.timeshiftTimelineHoverFraction)
                                    color: Theme.textPrimary
                                    font.pixelSize: 11
                                    renderType: Text.NativeRendering
                                }
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: bottomChrome.mediaButtonSize
                Layout.minimumHeight: bottomChrome.mediaButtonSize
                Layout.maximumHeight: bottomChrome.mediaButtonSize
                spacing: 4

                IconActionButton {
                    compact: true
                    borderless: true
                    barMode: true
                    implicitWidth: bottomChrome.mediaButtonSize
                    implicitHeight: bottomChrome.mediaButtonSize
                    iconInset: 1
                    iconSource: root.iconPath("previous-channel.svg")
                    caption: "Previous channel"
                    enabled: root.hasChannelList
                    onClicked: {
                        root.activateChannelRelative(-1, false)
                        root.revealUi("pointer")
                    }
                }

                IconActionButton {
                    compact: true
                    borderless: true
                    barMode: true
                    implicitWidth: bottomChrome.mediaButtonSize
                    implicitHeight: bottomChrome.mediaButtonSize
                    iconInset: 1
                    iconSource: root.iconPath(root.hasPlaybackChannel && root.player.isPlaying ? "pause.svg" : "play.svg")
                    caption: root.hasPlaybackChannel && root.player.isPlaying ? "Pause" : "Play"
                    enabled: root.hasPlaybackChannel || root.hasSelectedChannel
                    onClicked: {
                        if (root.hasPlaybackChannel) {
                            root.togglePauseWithBadge()
                        } else {
                            root.playSelection("pointer")
                        }
                    }
                }

                IconActionButton {
                    compact: true
                    borderless: true
                    barMode: true
                    implicitWidth: bottomChrome.mediaButtonSize
                    implicitHeight: bottomChrome.mediaButtonSize
                    iconInset: 1
                    iconSource: root.iconPath("stop.svg")
                    caption: "Stop"
                    enabled: root.canStopPlaybackAction
                    onClicked: root.handleStopPlaybackAction()
                }

                IconActionButton {
                    compact: true
                    borderless: true
                    barMode: true
                    implicitWidth: bottomChrome.mediaButtonSize
                    implicitHeight: bottomChrome.mediaButtonSize
                    iconInset: 1
                    iconSource: root.iconPath("next-channel.svg")
                    caption: "Next channel"
                    enabled: root.hasChannelList
                    onClicked: {
                        root.activateChannelRelative(1, false)
                        root.revealUi("pointer")
                    }
                }

                Item { Layout.fillWidth: true }

                IconActionButton {
                    compact: true
                    borderless: true
                    barMode: true
                    implicitWidth: bottomChrome.mediaButtonSize
                    implicitHeight: bottomChrome.mediaButtonSize
                    iconInset: 1
                    iconSource: root.iconPath("start-from-beginning.svg")
                    caption: "Start programme from beginning"
                    visible: root.liveCatchupButtonVisible
                    enabled: root.liveCatchupButtonEnabled
                    onClicked: {
                        root.app.playCatchup(
                            root.player.currentChannel || ({}),
                            root.playbackNowNext.currentProgram || ({}))
                        root.revealUi("pointer")
                    }
                }

                Item {
                    id: volumeControl
                    Layout.preferredWidth: bottomChrome.mediaButtonSize
                    Layout.preferredHeight: bottomChrome.mediaButtonSize
                    property bool hoverActive: volumeButton.hovered || volumePopoverHover.hovered || volumeSlider.pressed
                    property bool popoverVisible: bottomChrome.bottomChromeInputEnabled
                        && (hoverActive || volumePopoverHideTimer.running)

                    onHoverActiveChanged: {
                        if (hoverActive) {
                            volumePopoverHideTimer.stop()
                        } else {
                            volumePopoverHideTimer.restart()
                        }
                        root.updateAutoHide()
                    }

                    Timer {
                        id: volumePopoverHideTimer
                        interval: 160
                        repeat: false
                    }

                    IconActionButton {
                        id: volumeButton
                        anchors.centerIn: parent
                        compact: true
                        borderless: true
                        barMode: true
                        implicitWidth: bottomChrome.mediaButtonSize
                        implicitHeight: bottomChrome.mediaButtonSize
                        iconInset: 1
                        iconSource: root.iconPath(root.volumeIconFile())
                        caption: root.player.muted || root.player.volume <= 0 ? "Unmute" : "Mute"
                        onClicked: {
                            root.player.toggleMute()
                            root.revealUi("pointer")
                        }
                    }

                    Item {
                        id: volumePopover
                        width: 56
                        height: root.shell.layoutBand === "compact" ? 178 : 202
                        anchors.horizontalCenter: volumeButton.horizontalCenter
                        anchors.bottom: volumeButton.top
                        anchors.bottomMargin: 6
                        visible: volumeControl.popoverVisible
                        opacity: visible ? 1 : 0
                        z: 12

                        Behavior on opacity {
                            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                        }

                        HoverHandler {
                            id: volumePopoverHover
                            target: volumePopover
                            acceptedDevices: PointerDevice.Mouse
                        }

                        GlassPanel {
                            anchors.fill: parent
                            fillColor: "#82070d12"
                            strokeColor: "transparent"
                            radiusSize: 6
                        }

                        Slider {
                            id: volumeSlider
                            anchors.centerIn: parent
                            orientation: Qt.Vertical
                            height: parent.height - 26
                            width: 22
                            from: 0
                            to: 100
                            value: root.player.volume
                            onMoved: {
                                root.player.volume = value
                                root.revealUi("pointer")
                            }

                            background: Rectangle {
                                x: volumeSlider.leftPadding + volumeSlider.availableWidth / 2 - width / 2
                                y: volumeSlider.topPadding
                                width: 4
                                height: volumeSlider.availableHeight
                                radius: 2
                                color: "#5af4f7fb"

                                Rectangle {
                                    x: 0
                                    y: volumeSlider.visualPosition * parent.height
                                    width: parent.width
                                    height: parent.height - y
                                    radius: parent.radius
                                    color: Theme.accent
                                }
                            }

                            handle: Rectangle {
                                x: volumeSlider.leftPadding + volumeSlider.availableWidth / 2 - width / 2
                                y: volumeSlider.topPadding + volumeSlider.visualPosition * (volumeSlider.availableHeight - height)
                                width: 14
                                height: 14
                                radius: 4
                                color: Theme.textPrimary
                                border.width: 1
                                border.color: Theme.borderStrong
                            }
                        }
                    }
                }

                IconActionButton {
                    compact: true
                    borderless: true
                    barMode: true
                    implicitWidth: bottomChrome.mediaButtonSize
                    implicitHeight: bottomChrome.mediaButtonSize
                    iconInset: 1
                    iconSource: root.iconPath(root.shell.fullscreen ? "fullscreen-close.svg" : "fullscreen-enter.svg")
                    caption: root.shell.fullscreen ? "Exit fullscreen" : "Fullscreen"
                    onClicked: root.toggleFullscreen()
                }
            }
        }
    }

    Item {
        anchors.fill: parent
        visible: guideOverlayMounted || root.shell.activeOverlay === "settings"
        z: 6

        MouseArea {
            anchors.fill: parent
            onClicked: {
                if (root.shell.activeOverlay === "guide" || root.guideOverlayMounted) {
                    root.closeGuideOverlay(false)
                } else {
                    settingsPage.requestClose()
                }
            }
        }

        Rectangle {
            anchors.fill: parent
            color: "#36000000"
        }

        Item {
            anchors.fill: parent
            visible: root.guideOverlayMounted

            Item {
                id: guideOverlayFrame
                objectName: "ui.region.guide_overlay"
                anchors.fill: parent
                anchors.topMargin: root.topBarReservedHeight

                transform: Translate {
                    y: root.guideOverlayOpen ? 0 : guideOverlayFrame.height
                    Behavior on y {
                        NumberAnimation {
                            duration: root.overlayAnimationDuration
                            easing.type: Easing.OutCubic
                        }
                    }
                }

                GlassPanel {
                    anchors.fill: parent
                    fillColor: "#dd09131c"
                    strokeColor: "transparent"
                    radiusSize: 0
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {}
                }

                Item {
                    id: guideCollapseStrip
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: root.shell.layoutBand === "compact" ? 28 : 30

                    IconActionButton {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset: 6
                        width: guideButtonChrome.width
                        height: guideButtonChrome.height
                        compact: true
                        borderless: true
                        glassMode: true
                        iconInset: 1
                        iconSource: root.iconPath("epg-hide.svg")
                        caption: "Collapse Guide"
                        onClicked: root.collapseGuideOverlayToVideoOnly()
                    }
                }

                GuidePage {
                    id: guidePage
                    anchors.fill: parent
                    anchors.topMargin: root.shell.layoutBand === "compact" ? 28 : 30
                    overlayMode: true
                    onCollapseRequested: root.collapseGuideOverlayToVideoOnly()
                    onPlayChannelRequested: root.activateChannelById(channelId)
                    onPlayCatchupRequested: root.app.playCatchup(channel, program)
                }
            }
        }

        Item {
            anchors.fill: parent
            visible: root.shell.activeOverlay === "settings"

            MouseArea {
                anchors.fill: parent
                onClicked: settingsPage.requestClose()
            }

            GlassPanel {
                id: settingsOverlayFrame
                objectName: "ui.region.settings_overlay"
                width: root.settingsOverlayTargetWidth
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: root.topBarReservedHeight
                anchors.bottom: parent.bottom
                fillColor: "#88091119"
                strokeColor: "transparent"
                radiusSize: 0

                Behavior on width {
                    NumberAnimation {
                        duration: Theme.transitionMs + 40
                        easing.type: Easing.OutCubic
                    }
                }
            }

            MouseArea {
                anchors.fill: settingsOverlayFrame
                onClicked: {}
            }

            SettingsPage {
                id: settingsPage
                anchors.fill: settingsOverlayFrame
                overlayMode: true
            }
        }
    }

    Dialog {
        id: multiviewDegradeDialog
        modal: true
        focus: true
        title: "Reduce multiview load"
        standardButtons: Dialog.Yes | Dialog.No
        closePolicy: Popup.NoAutoClose

        contentItem: Text {
            width: 360
            text: root.multiView.pendingDegradeLayout === "pip"
                ? "Dropped frames stayed high on the focused tile. Step down to PiP?"
                : "Dropped frames stayed high on the focused tile. Exit multiview and keep the focused tile as primary playback?"
            wrapMode: Text.Wrap
            color: Theme.textPrimary
        }

        onAccepted: root.multiView.acceptPendingDegrade()
        onRejected: root.multiView.declinePendingDegrade()
    }

    Connections {
        target: root.multiView

        function onLayoutModeChanged() {
            if (root.multiviewActive) {
                root.channelChangeBubbleVisible = false
            }
            if (!root.multiviewActive) {
                root.multiviewSelectionMode = false
                root.multiviewSelectionIndex = 0
                root.pendingEmptyPipAssignment = false
                return
            }
            if (root.multiviewSelectionMode) {
                root.multiviewSelectionIndex = Math.max(
                    0,
                    Math.min(root.multiviewVisibleSlots - 1, root.multiviewSelectionIndex))
            }
            root.syncPendingEmptyPipAssignment()
        }

        function onFocusedTileIndexChanged() {
            if (!root.multiviewSelectionMode) {
                root.multiviewSelectionIndex = Math.max(
                    0,
                    Math.min(root.multiviewVisibleSlots - 1, Number(root.multiView.focusedTileIndex || 0)))
            }
        }

        function onDegradePromptVisibleChanged() {
            if (root.multiView.degradePromptVisible) {
                multiviewDegradeDialog.open()
            } else if (multiviewDegradeDialog.visible) {
                multiviewDegradeDialog.close()
            }
        }

        function onTilesChanged() {
            root.syncPendingEmptyPipAssignment()
        }
    }

    Item {
        id: channelChangeBubble
        readonly property real contentWidth: width - channelChangeBubbleContent.anchors.leftMargin - channelChangeBubbleContent.anchors.rightMargin
        readonly property real contentHeight: height - channelChangeBubbleContent.anchors.topMargin - channelChangeBubbleContent.anchors.bottomMargin
        readonly property real contentHalfWidth: (contentWidth - channelChangeBubbleContent.spacing) / 2
        readonly property int badgeSlotSize: Math.max(
            44,
            Math.floor(Math.min(channelChangeBubble.contentHeight * 0.78, channelChangeBubble.contentHalfWidth * 0.32)))
        width: Math.min(
            root.width - Theme.spacingL * 2,
            root.shell.layoutBand === "compact" ? 460 : 540)
        height: root.shell.layoutBand === "compact" ? 124 : 136
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.showShellChrome
            ? bottomChrome.height + Theme.spacingM + 12
            : Theme.spacingL
        visible: root.channelChangeBubbleVisible && root.hasPlaybackChannel && !root.leftPickerOpen && !root.multiviewActive
        opacity: visible ? 1 : 0
        z: 7

        Behavior on opacity {
            NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
        }

        GlassPanel {
            anchors.fill: parent
            fillColor: "#8c09131c"
            strokeColor: "transparent"
            radiusSize: 10
        }

        RowLayout {
            id: channelChangeBubbleContent
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            anchors.topMargin: 10
            anchors.bottomMargin: 10
            spacing: 16

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: channelChangeBubble.contentHalfWidth
                Layout.alignment: Qt.AlignVCenter
                spacing: 14

                Item {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredWidth: channelChangeBubble.badgeSlotSize
                    Layout.preferredHeight: channelChangeBubble.badgeSlotSize

                    ChannelBadge {
                        anchors.centerIn: parent
                        badgeSize: parent.width
                        showFrame: false
                        imageMargin: 0
                        sourcePath: root.player.currentChannel.cachedIconPath || ""
                        label: root.player.currentChannel.name || "Channel"
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 5

                    Text {
                        Layout.fillWidth: true
                        text: root.player.currentChannel.name || "Channel"
                        color: Theme.textPrimary
                        font.pixelSize: root.shell.layoutBand === "compact" ? 20 : 22
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.channelDisplayNumber(root.player.currentChannel)
                        visible: text.length > 0
                        color: Theme.textSecondary
                        font.pixelSize: 12
                        font.bold: false
                        elide: Text.ElideRight
                    }
                }
            }

            ColumnLayout {
                Layout.preferredWidth: channelChangeBubble.contentHalfWidth
                Layout.alignment: Qt.AlignVCenter
                spacing: 8

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Rectangle {
                            color: Theme.accent
                            radius: 4
                            implicitWidth: 42
                            implicitHeight: 22

                            Text {
                                anchors.centerIn: parent
                                text: "NOW"
                                color: Theme.textPrimary
                                font.pixelSize: 10
                                font.bold: true
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: root.playbackBubbleEpgLoading
                                ? ""
                                : (root.playbackNowNext.currentProgram.timeRange || "")
                            visible: text.length > 0
                            color: Theme.textSecondary
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.playbackBubbleEpgLoading
                            ? "Loading EPG..."
                            : (root.playbackNowNext.currentProgram.title || "No EPG data")
                        color: Theme.textPrimary
                        font.pixelSize: 14
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 4
                        radius: 2
                        visible: !root.playbackBubbleEpgLoading
                            && (root.playbackNowNext.currentProgram.progressPercent || 0) > 0
                        color: "#2f455a"

                        Rectangle {
                            width: parent.width * ((root.playbackNowNext.currentProgram.progressPercent || 0) / 100)
                            height: parent.height
                            radius: parent.radius
                            color: Theme.accent
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Rectangle {
                            color: "#273244"
                            radius: 4
                            implicitWidth: 46
                            implicitHeight: 22

                            Text {
                                anchors.centerIn: parent
                                text: "NEXT"
                                color: Theme.textSecondary
                                font.pixelSize: 10
                                font.bold: true
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: root.playbackBubbleEpgLoading
                                ? ""
                                : (root.playbackNowNext.nextProgram.timeRange || "")
                            visible: text.length > 0
                            color: Theme.textSecondary
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.playbackBubbleEpgLoading
                            ? "Loading EPG..."
                            : (root.playbackNowNext.nextProgram.title || "No EPG data")
                        color: Theme.textPrimary
                        font.pixelSize: 13
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }

    Item {
        id: multiviewRetainedBadge
        width: 44
        height: 44
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: Theme.spacingS
        anchors.bottomMargin: Theme.spacingS
        visible: root.multiviewRetainedSelectionVisible
        opacity: visible ? 1 : 0
        z: 0
        transformOrigin: Item.Center
        property real pulseScale: 1.0
        scale: pulseScale

        Behavior on opacity {
            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
        }

        SequentialAnimation on pulseScale {
            running: multiviewRetainedBadge.visible
            loops: Animation.Infinite
            NumberAnimation { from: 1.0; to: 1.07; duration: 1160; easing.type: Easing.OutCubic }
            NumberAnimation { from: 1.07; to: 1.0; duration: 1240; easing.type: Easing.InOutCubic }
        }

        GlassPanel {
            anchors.fill: parent
            radiusSize: width / 2
            fillColor: "#8a121d2a"
            strokeColor: "transparent"
            opacity: 0.5
        }

        Image {
            anchors.centerIn: parent
            width: 25
            height: 25
            source: root.iconPath("multiview.svg")
            fillMode: Image.PreserveAspectFit
            smooth: true
            opacity: 0.3
        }
    }

    Item {
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: root.topOverlayMargin
        visible: root.multiviewSelectionMode
        opacity: visible ? 1 : 0
        z: 9

        Behavior on opacity {
            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
        }

        Rectangle {
            anchors.centerIn: parent
            width: selectionModePillText.implicitWidth + 28
            height: selectionModePillText.implicitHeight + 14
            radius: height / 2
            color: "#b3241a0c"
            border.width: 1
            border.color: "#f59e0b"

            Text {
                id: selectionModePillText
                anchors.centerIn: parent
                text: "Select tile  Arrows / Enter / Esc"
                color: Theme.textPrimary
                font.pixelSize: 13
                font.bold: true
            }
        }
    }

    Item {
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: root.topOverlayMargin
        visible: root.numericHudVisible
        opacity: visible ? 1 : 0
        z: 9

        Behavior on opacity {
            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.numericHudText
            color: "#ffffff"
            style: Text.Outline
            styleColor: "#000000"
            font.family: numericOsdFont.status === FontLoader.Ready ? numericOsdFont.name : ""
            font.pixelSize: root.shell.layoutBand === "compact" ? 38 : 56
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
        }
    }

    Component {
        id: videoSurfaceComponent

        MpvVideoItem {
            anchors.fill: parent
            playerController: root.player
            playerObject: root.player.playbackPlayerObject
        }
    }

    Item {
        width: 25
        height: 25
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: Theme.spacingS
        anchors.topMargin: Theme.spacingS
        visible: root.mainWindow
            && root.mainWindow.alwaysOnTop
            && root.settings.showOnTopModeIndicator
        z: 0

        Image {
            anchors.fill: parent
            source: root.iconPath("on-top.svg")
            fillMode: Image.PreserveAspectFit
            smooth: true
            opacity: 0.3
        }
    }

    Item {
        width: 100
        height: 100
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: Theme.spacingL
        anchors.topMargin: root.topOverlayMargin
        visible: root.keyboardVolumeHudVisible && !root.leftPickerOpen
        opacity: visible ? 1 : 0
        z: 8

        Behavior on opacity {
            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
        }

        GlassPanel {
            anchors.fill: parent
            fillColor: "#82070d12"
            strokeColor: "transparent"
            radiusSize: 10
        }

        Image {
            anchors.centerIn: parent
            width: 52
            height: 52
            source: root.iconPath(root.keyboardVolumeHudIconFile)
            fillMode: Image.PreserveAspectFit
            smooth: true
        }
    }

    Rectangle {
        width: 14
        height: 14
        radius: 7
        color: "#e53935"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: Theme.spacingL + 8
        anchors.topMargin: root.topOverlayMargin + 8
        visible: root.player.isRecording && !root.leftPickerOpen
        z: 9

        SequentialAnimation on opacity {
            running: parent.visible
            loops: Animation.Infinite
            NumberAnimation { to: 0.3; duration: 600 }
            NumberAnimation { to: 1.0; duration: 600 }
        }
    }

    BusyIndicator {
        width: 24
        height: 24
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: Theme.spacingL + 1
        anchors.topMargin: root.topOverlayMargin + 1
        running: root.player.isRemuxing && !root.leftPickerOpen
        visible: running
        z: 9
    }
}
