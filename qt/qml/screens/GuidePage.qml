pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OKILTV
import "../theme/Theme.js" as Theme

Item {
    id: root

    signal collapseRequested()
    signal playChannelRequested(int channelId)
    signal playCatchupRequested(var channel, var program)

    property bool overlayMode: false
    // qmllint disable unqualified
    readonly property var shell: shellController
    readonly property var epgGrid: epgGridModel
    readonly property var guideState: guideStateModel
    readonly property var channelList: channelListModel
    readonly property var app: appController
    readonly property var player: appPlayerController
    readonly property var dvr: dvrController
    // qmllint enable unqualified
    property real channelColumnWidth: root.shell.layoutBand === "compact" ? 246 : 284
    property real pixelsPerMinute: root.shell.layoutBand === "compact" ? 3.0 : 3.6
    property real timelineVisibleWidth: root.epgGrid.windowSpanMinutes * root.pixelsPerMinute
    property real timelineOverflowWidth: root.shell.layoutBand === "compact" ? 120 : 144
    property real timelineContentWidth: Math.max(timelineViewport.width, root.timelineVisibleWidth + root.timelineOverflowWidth)
    property real maxTimelineContentX: Math.max(0, root.timelineContentWidth - timelineFlick.width)
    property real maxTimelineHeaderX: Math.max(0, root.timelineVisibleWidth - timelineViewport.width)
    property real firstVisibleHourOffsetMinutes: {
        const slots = root.epgGrid.visibleTimeSlots
        return slots.length > 0 ? Number(slots[0].offsetMinutes || 0) : 0
    }
    property int collapsedRowHeight: root.shell.layoutBand === "compact" ? 72 : 76
    property int detailBandHeight: root.shell.layoutBand === "compact" ? 188 : 204
    property int channelIconSize: root.shell.layoutBand === "compact" ? 46 : 50
    property int channelNamePixelSize: root.shell.layoutBand === "compact" ? 16 : 17
    property int rowSpacing: 0
    property color hourShadeA: "#10161d"
    property color hourShadeB: "#10161d"
    property color channelShadeA: "#111821"
    property color channelShadeB: "#111821"
    property color rowSelectionAccent: "#9aa3ad"
    property color hourOverlayA: "#05000000"
    property color hourOverlayB: "#05000000"
    property color programTileShade: "#18212b"
    property color programTileNowShade: "#202b36"
    property color programTileSelectedShade: "#2a3541"
    property color separatorShade: "#2a3541"
    property color separatorSelectedShade: "#9aa3ad"
    property color headerMarkerColor: "#35414e"
    property int headerMarkerWidth: 1
    property int headerHalfHourMarkerHeight: root.shell.layoutBand === "compact" ? 8 : 10
    property int headerMarkerXOffset: -1
    property color currentTimeLineColor: "#b0bac4"
    property real currentTimeLineOpacity: 0.58
    property int pendingGuideRowIndex: -1
    property int pendingGuidePositionMode: ListView.Contain
    property var pendingGuideProgram: ({})
    property bool pendingGuideSyncRows: true
    property var hoveredProgram: ({})
    property var hoveredProgramChannel: ({})
    property var selectedCatchupState: ({ "visible": false, "enabled": false, "reason": "" })
    property int selectedDetailRowIndex: {
        const _timeSlotsRevision = root.epgGrid.timeSlots.length
        return root.epgGrid.rowIndexForChannelId(root.guideState.selectedChannelId)
    }
    property bool currentTimeLineHasDetailGap: selectedDetailRowIndex >= 0 && rowExpanded(root.guideState.selectedChannelId)

    function currentTimeLineGapTop(contentY) {
        if (!root.currentTimeLineHasDetailGap) {
            return 0
        }
        return root.selectedDetailRowIndex * root.collapsedRowHeight + root.collapsedRowHeight - contentY
    }

    function currentTimeLineGapBottom(contentY) {
        return root.currentTimeLineGapTop(contentY) + root.detailBandHeight
    }

    function rowExpanded(channelId) {
        return root.guideState.detailsExpanded
            && root.guideState.selectedChannelId === channelId
            && (root.guideState.selectedProgram.title || "").length > 0
    }

    function rowHeightForChannel(channelId) {
        return root.rowExpanded(channelId)
            ? root.collapsedRowHeight + root.detailBandHeight
            : root.collapsedRowHeight
    }

    function durationLabel(program) {
        if (!program || !program.start || !program.stop) {
            return ""
        }
        const start = new Date(program.start)
        const stop = new Date(program.stop)
        if (isNaN(start.getTime()) || isNaN(stop.getTime())) {
            return ""
        }
        const minutes = Math.max(1, Math.round((stop - start) / 60000))
        return minutes + " min"
    }

    function normalizeDescription(text) {
        const source = (text || "").trim()
        if (!source.length) {
            return "No programme description available."
        }
        return source.replace(/\n[ \t]*\n+/g, "\n")
    }

    function episodeTitle(program) {
        if (!program) {
            return ""
        }
        return (program.subTitle || "").trim()
    }

    function programVisualWidth(program) {
        const offset = Number(program.offsetMinutes || 0) * root.pixelsPerMinute
        const actualWidth = Number(program.displayDurationMinutes || program.durationMinutes || 0) * root.pixelsPerMinute
        return Math.max(10, Math.min(root.timelineContentWidth - offset - 1, actualWidth))
    }

    function selectedProgramData() {
        const selectedProgram = root.epgGrid.selectedProgram
        if ((selectedProgram.start || "").length > 0) {
            return selectedProgram
        }
        return root.guideState.selectedProgram
    }

    function selectChannel(channelId) {
        if (channelId < 0) {
            return
        }
        if (root.channelList.selectedChannelId !== channelId) {
            root.channelList.selectById(channelId)
        }
        if (root.guideState.selectedChannelId !== channelId) {
            root.guideState.selectChannel(channelId)
        }
        if (root.epgGrid.selectedChannelId !== channelId) {
            root.epgGrid.selectedChannelId = channelId
        }
    }

    function scheduleGuideViewportSync(channelId, program, positionMode, syncRows) {
        const rowIndex = root.epgGrid.rowIndexForChannelId(channelId)
        if (rowIndex < 0) {
            return false
        }

        pendingGuideRowIndex = rowIndex
        pendingGuidePositionMode = positionMode !== undefined ? positionMode : ListView.Contain
        pendingGuideProgram = program ? program : ({})
        pendingGuideSyncRows = syncRows !== undefined ? !!syncRows : true
        guideViewportSyncTimer.restart()
        return true
    }

    function applyGuideViewportSync() {
        if (pendingGuideRowIndex < 0) {
            return
        }

        const rowIndex = pendingGuideRowIndex
        const positionMode = pendingGuidePositionMode
        const program = pendingGuideProgram
        const syncRows = pendingGuideSyncRows
        pendingGuideRowIndex = -1
        pendingGuideProgram = ({})
        pendingGuideSyncRows = true

        if (syncRows) {
            if (channelColumn.forceLayout) {
                channelColumn.forceLayout()
            }
            if (timelineRows.forceLayout) {
                timelineRows.forceLayout()
            }

            channelColumn.positionViewAtIndex(rowIndex, positionMode)
            timelineRows.positionViewAtIndex(rowIndex, positionMode)

            const maxY = Math.max(0, channelColumn.contentHeight - channelColumn.height)
            channelColumn.contentY = Math.max(0, Math.min(maxY, timelineRows.contentY))
        }

        if (!program || (program.start || "").length === 0) {
            return
        }

        const programLeft = Number(program.offsetMinutes || 0) * root.pixelsPerMinute
        const programRight = programLeft + root.programVisualWidth(program)
        let nextContentX = timelineFlick.contentX
        if (programLeft < nextContentX) {
            nextContentX = programLeft
        } else if (programRight > nextContentX + timelineViewport.width) {
            nextContentX = programRight - timelineViewport.width
        }

        nextContentX = Math.max(0, Math.min(root.maxTimelineContentX, nextContentX))
        timelineFlick.contentX = nextContentX
        timeHeaderFlick.contentX = Math.min(nextContentX, root.maxTimelineHeaderX)
    }

    function ensureProgramVisible(channelId, program) {
        return root.scheduleGuideViewportSync(channelId, program, ListView.Contain, true)
    }

    function focusProgram(channelId, program) {
        if (channelId < 0 || !program || (program.start || "").length === 0) {
            return false
        }

        const sameChannel = root.guideState.selectedChannelId === channelId
        if (!sameChannel) {
            root.selectChannel(channelId)
        }
        root.guideState.selectProgram(program)
        root.epgGrid.selectedProgramStart = program.start || ""
        root.scheduleGuideViewportSync(channelId, program, ListView.Contain, !sameChannel)
        return true
    }

    function selectProgram(channelId, program, expandDetails) {
        const selectedStart = root.selectedProgramData().start || ""
        const programStart = program.start || ""
        const sameProgramSelected = root.guideState.selectedChannelId === channelId
            && selectedStart.length > 0
            && selectedStart === programStart
        if (expandDetails && sameProgramSelected && root.guideState.detailsExpanded) {
            root.guideState.detailsExpanded = false
            root.ensureProgramVisible(channelId, program)
            return true
        }

        if (!root.focusProgram(channelId, program)) {
            return false
        }
        if (expandDetails) {
            root.guideState.detailsExpanded = true
        }
        return true
    }

    function focusChannel(channelId, expandDetails) {
        if (channelId < 0) {
            root.guideState.detailsExpanded = false
            return
        }

        root.selectChannel(channelId)
        if (expandDetails) {
            root.guideState.detailsExpanded = true
        }

        const nowProgram = root.epgGrid.programForChannelAtTimestamp(channelId, root.currentUtcIso())
        if ((nowProgram.start || "").length > 0) {
            root.guideState.selectProgram(nowProgram)
            root.epgGrid.selectedProgramStart = nowProgram.start || ""
            root.scheduleGuideViewportSync(channelId, nowProgram, ListView.Center)
            return
        }

        root.scheduleGuideViewportSync(channelId, root.selectedProgramData(), ListView.Center, true)
    }

    function currentUtcIso() {
        return new Date().toISOString()
    }

    function programIsFullyPast(program) {
        if (!program || !program.stop) {
            return false
        }
        const stop = new Date(program.stop)
        return !isNaN(stop.getTime()) && stop.getTime() <= Date.now()
    }

    function syncGuideRenderViewport() {
        const timelineDuration = timelineViewport.width > 0
            ? timelineViewport.width / root.pixelsPerMinute
            : 0
        const timelineStart = timelineFlick.contentX > 0
            ? timelineFlick.contentX / root.pixelsPerMinute
            : 0
        root.epgGrid.setRenderViewport(timelineStart, timelineDuration)

        if (timelineRows.count <= 0) {
            return
        }

        const verticalPadding = 2
        const probeX = Math.max(1, timelineFlick.contentX + 1)
        const topProbeY = Math.max(1, timelineRows.contentY + 1)
        const bottomProbeY = Math.max(topProbeY, timelineRows.contentY + timelineRows.height - 2)
        let firstRow = timelineRows.indexAt(probeX, topProbeY)
        let lastRow = timelineRows.indexAt(probeX, bottomProbeY)
        if (firstRow < 0 || lastRow < 0) {
            const approximateFirst = Math.max(0, Math.floor(timelineRows.contentY / root.collapsedRowHeight) - verticalPadding)
            const approximateLast = Math.min(
                timelineRows.count - 1,
                Math.ceil((timelineRows.contentY + timelineRows.height) / root.collapsedRowHeight) + verticalPadding)
            root.epgGrid.setVisibleRowRange(approximateFirst, approximateLast)
            return
        }

        firstRow = Math.max(0, firstRow - verticalPadding)
        lastRow = Math.min(timelineRows.count - 1, lastRow + verticalPadding)
        root.epgGrid.setVisibleRowRange(firstRow, lastRow)
    }

    function scheduleGuideRenderViewportSync() {
        guideRenderViewportTimer.restart()
    }

    function moveSelectionHorizontally(delta) {
        if (root.guideState.selectedChannelId < 0) {
            root.focusInitialChannel()
            return true
        }

        const currentProgram = root.selectedProgramData()
        const nextProgram = root.epgGrid.adjacentProgram(
            root.guideState.selectedChannelId,
            currentProgram.start || "",
            delta)
        return root.focusProgram(root.guideState.selectedChannelId, nextProgram)
    }

    function moveSelectionVertically(delta) {
        const currentChannelId = root.guideState.selectedChannelId
        if (currentChannelId < 0) {
            root.focusInitialChannel()
            return true
        }

        const currentRow = root.epgGrid.rowIndexForChannelId(currentChannelId)
        if (currentRow < 0) {
            root.focusInitialChannel()
            return true
        }

        const nextChannelId = root.epgGrid.adjacentChannelId(currentChannelId, delta)
        if (nextChannelId < 0) {
            return false
        }

        const nextRow = root.epgGrid.rowIndexForChannelId(nextChannelId)
        if (currentRow >= 0 && currentRow === nextRow) {
            return false
        }

        const nextProgram = root.epgGrid.programForChannelAtTimestamp(nextChannelId, root.currentUtcIso())
        if ((nextProgram.start || "").length > 0) {
            return root.focusProgram(nextChannelId, nextProgram)
        }

        root.focusChannel(nextChannelId, root.guideState.detailsExpanded)
        return true
    }

    function focusInitialChannel() {
        let targetChannelId = -1
        const currentPlaybackId = root.player.currentChannel.id !== undefined
            ? Number(root.player.currentChannel.id)
            : -1
        if (currentPlaybackId >= 0 && root.epgGrid.rowIndexForChannelId(currentPlaybackId) >= 0) {
            targetChannelId = currentPlaybackId
        } else if (root.guideState.selectedChannelId >= 0
            && root.epgGrid.rowIndexForChannelId(root.guideState.selectedChannelId) >= 0) {
            targetChannelId = root.guideState.selectedChannelId
        } else {
            targetChannelId = root.epgGrid.channelIdAt(0)
        }

        root.focusChannel(targetChannelId, true)
    }

    function prepareForOpen() {
        root.guideState.selectedGroupId = root.channelList.selectedCategoryId
        root.scheduleGuideRenderViewportSync()
        Qt.callLater(root.focusInitialChannel)
    }

    function handleKeyboardEvent(event) {
        if (!root.overlayMode) {
            return false
        }

        const ctrlPressed = (event.modifiers & Qt.ControlModifier) !== 0
        if (ctrlPressed && event.key === Qt.Key_R) {
            return root.togglePreferredProgramDvrSchedule(true)
        }
        if (ctrlPressed && event.key === Qt.Key_Down) {
            root.collapseRequested()
            return true
        }
        if (ctrlPressed && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
            const selectedProgram = root.selectedProgramData()
            if (root.selectedCatchupState.enabled) {
                root.playCatchupRequested(root.guideState.selectedChannel, selectedProgram)
                return true
            }
            return false
        }
        if (ctrlPressed) {
            return false
        }

        switch (event.key) {
        case Qt.Key_Left:
            return root.moveSelectionHorizontally(-1)
        case Qt.Key_Right:
            return root.moveSelectionHorizontally(1)
        case Qt.Key_Up:
            return root.moveSelectionVertically(-1)
        case Qt.Key_Down:
            return root.moveSelectionVertically(1)
        case Qt.Key_Space:
            root.guideState.detailsExpanded = !root.guideState.detailsExpanded
            if (root.guideState.detailsExpanded) {
                root.ensureProgramVisible(root.guideState.selectedChannelId, root.selectedProgramData())
            }
            return true
        case Qt.Key_Return:
        case Qt.Key_Enter:
            if (root.guideState.selectedChannelId < 0) {
                return false
            }
            const selectedProgram = root.selectedProgramData()
            if (root.programIsFullyPast(selectedProgram) && root.selectedCatchupState.enabled) {
                root.playCatchupRequested(root.guideState.selectedChannel, selectedProgram)
                return true
            }
            root.playChannelRequested(root.guideState.selectedChannelId)
            return true
        default:
            return false
        }
    }

    function refreshSelectedCatchupState() {
        root.selectedCatchupState = root.app.catchupActionState(
            root.guideState.selectedChannel || ({}),
            root.selectedProgramData() || ({}))
    }

    function clearHoveredProgram(channelId, startIso) {
        if ((root.hoveredProgram.start || "") !== (startIso || "")) {
            return
        }
        if (Number(root.hoveredProgramChannel.id) !== Number(channelId)) {
            return
        }
        root.hoveredProgram = ({})
        root.hoveredProgramChannel = ({})
    }

    function toggleSelectedProgramDvrSchedule() {
        const program = root.selectedProgramData() || ({})
        const channel = root.guideState.selectedChannel || ({})
        if ((program.start || "").length === 0 || (program.stop || "").length === 0) {
            return false
        }
        if (channel.id === undefined
            || (channel.profileId || "").length === 0
            || (channel.streamUrl || "").length === 0) {
            return false
        }
        return root.dvr.toggleProgramSchedule(channel, program)
    }

    function toggleHoveredProgramDvrSchedule() {
        const program = root.hoveredProgram || ({})
        const channel = root.hoveredProgramChannel || ({})
        if ((program.start || "").length === 0 || (program.stop || "").length === 0) {
            return false
        }
        if (channel.id === undefined
            || (channel.profileId || "").length === 0
            || (channel.streamUrl || "").length === 0) {
            return false
        }
        return root.dvr.toggleProgramSchedule(channel, program)
    }

    function togglePreferredProgramDvrSchedule(preferKeyboardSelection) {
        if (root.toggleSelectedProgramDvrSchedule()) {
            return true
        }
        if (root.toggleHoveredProgramDvrSchedule()) {
            return true
        }
        return false
    }

    Connections {
        target: root.guideState

        function onSelectedChannelIdChanged() {
            root.refreshSelectedCatchupState()
        }

        function onSelectedProgramChanged() {
            root.refreshSelectedCatchupState()
        }

        function onSelectedChannelChanged() {
            root.refreshSelectedCatchupState()
        }
    }
    Component.onCompleted: root.refreshSelectedCatchupState()

    Timer {
        id: guideViewportSyncTimer
        interval: 0
        repeat: false
        onTriggered: root.applyGuideViewportSync()
    }

    Timer {
        id: guideRenderViewportTimer
        interval: 16
        repeat: false
        onTriggered: root.syncGuideRenderViewport()
    }

    Rectangle {
        anchors.fill: parent
        visible: !root.overlayMode
        color: Theme.window
    }

    Item {
        anchors.fill: parent
        anchors.margins: root.overlayMode ? Theme.spacingM : Theme.spacingL

        Item {
            id: gridHeader
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: root.shell.layoutBand === "compact" ? 30 : 34

            Rectangle {
                id: leftHeaderPane
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: root.channelColumnWidth
                radius: 0
                color: root.channelShadeA

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Channels"
                    color: Theme.textPrimary
                    font.pixelSize: 11
                    font.bold: true
                }
            }

            Item {
                id: timelineViewport
                anchors.left: leftHeaderPane.right
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                clip: true

                Flickable {
                    id: timeHeaderFlick
                    anchors.fill: parent
                    contentWidth: root.timelineVisibleWidth
                    interactive: false
                    clip: true

                    Item {
                        anchors.fill: parent

                        Repeater {
                            model: root.epgGrid.visibleTimeSlots

                            delegate: Rectangle {
                                id: headerSlot
                                required property int index
                                required property var modelData
                                property color headerFill: "#10161d"

                                x: modelData.offsetMinutes * root.pixelsPerMinute
                                width: 60 * root.pixelsPerMinute
                                height: timelineViewport.height
                                color: headerFill

                                Text {
                                    anchors.centerIn: parent
                                    text: headerSlot.modelData.label
                                    color: Theme.textPrimary
                                    font.pixelSize: 11
                                    font.bold: headerSlot.modelData.isHour || headerSlot.modelData.isNow
                                }
                            }
                        }
                    }

                    Repeater {
                        model: root.epgGrid.visibleTimeSlots

                        delegate: Rectangle {
                            required property int index
                            required property var modelData

                            visible: index > 0 && index < (root.epgGrid.visibleTimeSlots.length - 1)
                            x: modelData.offsetMinutes * root.pixelsPerMinute + root.headerMarkerXOffset
                            y: 0
                            width: root.headerMarkerWidth
                            height: timeHeaderFlick.height
                            color: root.headerMarkerColor
                        }
                    }

                    Repeater {
                        model: Math.max(0, root.epgGrid.visibleTimeSlots.length - 1)

                        delegate: Rectangle {
                            required property int index
                            x: (root.firstVisibleHourOffsetMinutes + index * 60 + 30) * root.pixelsPerMinute + root.headerMarkerXOffset
                            y: 0
                            width: root.headerMarkerWidth
                            height: root.headerHalfHourMarkerHeight
                            color: root.headerMarkerColor
                        }
                    }
                }
            }
        }

        Item {
            id: contentArea
            anchors.top: gridHeader.bottom
            anchors.topMargin: root.rowSpacing
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom

            Row {
                anchors.fill: parent
                spacing: root.rowSpacing
                visible: channelColumn.count > 0

                ListView {
                    id: channelColumn
                    width: root.channelColumnWidth
                    height: parent.height
                    clip: true
                    interactive: false
                    spacing: root.rowSpacing
                    model: root.epgGrid

                    delegate: Rectangle {
                        id: guideChannelRow
                        required property int index
                        required property int channelId
                        required property string channelName
                        required property string channelIconPath
                        property color rowFill: "#111821"

                        width: ListView.view.width
                        height: root.rowHeightForChannel(guideChannelRow.channelId)
                        radius: 0
                        color: rowFill

                        Rectangle {
                            visible: guideChannelRow.channelId === root.guideState.selectedChannelId
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 3
                            color: root.rowSelectionAccent
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: Theme.border
                            opacity: 0.9
                        }

                        Column {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            anchors.topMargin: 12
                            spacing: 8

                            RowLayout {
                                width: parent.width
                                spacing: 10

                                ChannelBadge {
                                    badgeSize: root.channelIconSize
                                    showFrame: false
                                    sourcePath: guideChannelRow.channelIconPath
                                    label: guideChannelRow.channelName
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2

                                    Text {
                                        Layout.fillWidth: true
                                        text: guideChannelRow.channelName
                                        color: Theme.textPrimary
                                        font.pixelSize: root.channelNamePixelSize
                                        font.bold: true
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: guideChannelRow.channelId === root.guideState.selectedChannelId
                                            ? (root.guideState.selectedProgram.timeRange || "")
                                            : ""
                                        visible: text.length > 0
                                        color: Theme.textSecondary
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }
                            }

                            Text {
                                width: parent.width
                                visible: root.rowExpanded(guideChannelRow.channelId)
                                text: root.guideState.selectedProgram.title || "Programme details"
                                color: Theme.textPrimary
                                font.pixelSize: 13
                                font.bold: true
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }

                            Text {
                                width: parent.width
                                visible: root.rowExpanded(guideChannelRow.channelId)
                                    && root.episodeTitle(root.guideState.selectedProgram).length > 0
                                text: root.episodeTitle(root.guideState.selectedProgram)
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                font.italic: true
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.focusChannel(guideChannelRow.channelId, true)
                            onDoubleClicked: root.playChannelRequested(guideChannelRow.channelId)
                        }
                    }

                    WheelHandler {
                        target: null
                        onWheel: function(event) {
                            const maxY = Math.max(0, timelineRows.contentHeight - timelineRows.height)
                            const nextY = Math.max(0, Math.min(maxY, timelineRows.contentY - event.angleDelta.y))
                            timelineRows.contentY = nextY
                            event.accepted = true
                        }
                    }
                }

                Item {
                    width: parent.width - root.channelColumnWidth - root.rowSpacing
                    height: parent.height

                    Flickable {
                        id: timelineFlick
                        anchors.fill: parent
                        clip: true
                        contentWidth: root.timelineContentWidth
                        boundsBehavior: Flickable.StopAtBounds
                        flickableDirection: Flickable.HorizontalFlick
                        onContentXChanged: {
                            if (contentX > root.maxTimelineContentX) {
                                contentX = root.maxTimelineContentX
                            }
                            if (contentX < 0) {
                                contentX = 0
                            }
                            timeHeaderFlick.contentX = Math.min(contentX, root.maxTimelineHeaderX)
                            root.scheduleGuideRenderViewportSync()
                        }
                        onWidthChanged: root.scheduleGuideRenderViewportSync()
                        Component.onCompleted: root.scheduleGuideRenderViewportSync()

                        ScrollBar.horizontal: ScrollBar { }

                        ListView {
                            id: timelineRows
                            objectName: "ui.region.guide_grid"
                            width: timelineFlick.contentWidth
                            height: timelineFlick.height
                            spacing: root.rowSpacing
                            interactive: true
                            flickableDirection: Flickable.VerticalFlick
                            boundsBehavior: Flickable.StopAtBounds
                            clip: true
                            model: root.epgGrid
                            reuseItems: true
                            cacheBuffer: 1600
                            onContentYChanged: {
                                const maxY = Math.max(0, channelColumn.contentHeight - channelColumn.height)
                                channelColumn.contentY = Math.max(0, Math.min(maxY, contentY))
                                root.scheduleGuideRenderViewportSync()
                            }
                            onHeightChanged: root.scheduleGuideRenderViewportSync()
                            onCountChanged: root.scheduleGuideRenderViewportSync()
                            ScrollBar.vertical: ScrollBar {
                                id: guideGridScrollBar
                                policy: timelineRows.contentHeight > timelineRows.height
                                    ? ScrollBar.AlwaysOn
                                    : ScrollBar.AlwaysOff
                                interactive: true
                                width: 6
                                z: 3
                                anchors.right: parent.right
                                anchors.rightMargin: 0
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                padding: 0
                                background: Rectangle {
                                    implicitWidth: 6
                                    radius: width / 2
                                    color: "#2a20364d"
                                }
                                contentItem: Rectangle {
                                    implicitWidth: 6
                                    radius: width / 2
                                    color: guideGridScrollBar.pressed
                                        ? Theme.borderStrong
                                        : "#8c4e88b8"
                                }
                            }

                            delegate: Item {
                                id: timelineRow
                                required property int index
                                required property int channelId
                                required property string channelName
                                required property string channelProfileId
                                required property string channelStreamUrl
                                required property string channelTvgId
                                required property var programs
                                property color rowFill: "#111821"

                                width: timelineFlick.contentWidth
                                height: root.rowHeightForChannel(timelineRow.channelId)
                                clip: false

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    height: root.collapsedRowHeight
                                    radius: 0
                                    color: timelineRow.rowFill
                                }

                                Rectangle {
                                    visible: timelineRow.channelId === root.guideState.selectedChannelId
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    width: parent.width
                                    height: 2
                                    color: root.rowSelectionAccent
                                }

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    height: 1
                                    color: Theme.border
                                    opacity: 0.9
                                }

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    height: root.collapsedRowHeight
                                    color: "transparent"

                                    Repeater {
                                        model: root.epgGrid.visibleTimeSlots

                                        delegate: Rectangle {
                                            required property int index
                                            required property var modelData
                                            property color overlayFill: index % 2 === 0 ? root.hourOverlayA : root.hourOverlayB

                                            x: modelData.offsetMinutes * root.pixelsPerMinute
                                            width: 60 * root.pixelsPerMinute
                                            height: root.collapsedRowHeight
                                            color: overlayFill
                                        }
                                    }
                                }

                                Repeater {
                                    model: timelineRow.programs

                                    delegate: Rectangle {
                                        id: guideProgramTile
                                        required property var modelData

                                        x: guideProgramTile.modelData.offsetMinutes * root.pixelsPerMinute
                                        y: 0
                                        width: root.programVisualWidth(guideProgramTile.modelData)
                                        height: root.collapsedRowHeight
                                        radius: 0
                                        clip: true
                                        color: guideProgramTile.modelData.isSelected
                                            ? root.programTileSelectedShade
                                            : (guideProgramTile.modelData.isNow ? root.programTileNowShade : root.programTileShade)

                                        readonly property bool dvrMarked: {
                                            const _scheduleCount = root.dvr.scheduledCount
                                            return root.dvr.isProgramScheduledByIdentity(
                                                timelineRow.channelProfileId,
                                                timelineRow.channelId,
                                                guideProgramTile.modelData.start || "",
                                                guideProgramTile.modelData.stop || "",
                                                guideProgramTile.modelData.title || "")
                                        }

                                        Column {
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.top: parent.top
                                            anchors.bottom: parent.bottom
                                            anchors.leftMargin: 8
                                            anchors.rightMargin: 8
                                            anchors.topMargin: 8
                                            anchors.bottomMargin: 8
                                            spacing: 3

                                            Text {
                                                width: parent.width
                                                text: guideProgramTile.modelData.startTimeLabel
                                                color: Theme.textSecondary
                                                font.pixelSize: 10
                                                elide: Text.ElideRight
                                            }

                                            Text {
                                                width: parent.width
                                                text: guideProgramTile.modelData.title
                                                color: Theme.textPrimary
                                                font.pixelSize: 11
                                                font.bold: guideProgramTile.modelData.isSelected
                                                wrapMode: Text.Wrap
                                                maximumLineCount: 2
                                                elide: Text.ElideRight
                                            }

                                            Rectangle {
                                                width: parent.width
                                                height: 3
                                                radius: 2
                                                visible: guideProgramTile.modelData.isNow
                                                color: "#2b3641"

                                                Rectangle {
                                                    width: parent.width * ((guideProgramTile.modelData.progressPercent || 0) / 100)
                                                    height: parent.height
                                                    radius: parent.radius
                                                    color: "#9ea8b2"
                                                }
                                            }
                                        }

                                        Rectangle {
                                            anchors.top: parent.top
                                            anchors.bottom: parent.bottom
                                            anchors.right: parent.right
                                            width: 1
                                            color: guideProgramTile.modelData.isSelected
                                                ? root.separatorSelectedShade
                                                : root.separatorShade
                                        }

                                        Rectangle {
                                            anchors.top: parent.top
                                            anchors.right: parent.right
                                            anchors.topMargin: 6
                                            anchors.rightMargin: 6
                                            width: 8
                                            height: 8
                                            radius: 4
                                            color: "#ff3a3a"
                                            visible: guideProgramTile.dvrMarked
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            onClicked: root.selectProgram(timelineRow.channelId, guideProgramTile.modelData, true)
                                            onDoubleClicked: root.playChannelRequested(timelineRow.channelId)
                                        }

                                        HoverHandler {
                                            acceptedDevices: PointerDevice.Mouse
                                            onHoveredChanged: {
                                                if (hovered) {
                                                    root.hoveredProgram = guideProgramTile.modelData
                                                    root.hoveredProgramChannel = {
                                                        id: timelineRow.channelId,
                                                        name: timelineRow.channelName,
                                                        profileId: timelineRow.channelProfileId,
                                                        streamUrl: timelineRow.channelStreamUrl,
                                                        tvgId: timelineRow.channelTvgId
                                                    }
                                                } else {
                                                    root.clearHoveredProgram(timelineRow.channelId, guideProgramTile.modelData.start || "")
                                                }
                                            }
                                        }
                                    }
                                }

                                Loader {
                                    active: root.rowExpanded(timelineRow.channelId)
                                    x: timelineFlick.contentX
                                    y: root.collapsedRowHeight
                                    width: timelineViewport.width
                                    height: root.detailBandHeight

                                    sourceComponent: Rectangle {
                                        radius: 0
                                        color: "#de142331"

                                        Column {
                                            id: detailContentColumn
                                            anchors.fill: parent
                                            anchors.margins: 16
                                            spacing: 8

                                            RowLayout {
                                                width: parent.width
                                                spacing: 10

                                                Text {
                                                    text: root.guideState.selectedProgram.title || "Programme Details"
                                                    color: Theme.textPrimary
                                                    font.pixelSize: 17
                                                    font.bold: true
                                                    elide: Text.ElideRight
                                                    Layout.fillWidth: true
                                                }

                                                Text {
                                                    text: root.durationLabel(root.guideState.selectedProgram)
                                                    color: Theme.textSecondary
                                                    font.pixelSize: 12
                                                }
                                            }

                                            Text {
                                                width: parent.width
                                                visible: root.episodeTitle(root.guideState.selectedProgram).length > 0
                                                text: root.episodeTitle(root.guideState.selectedProgram)
                                                color: Theme.textSecondary
                                                font.pixelSize: 13
                                                font.italic: true
                                                elide: Text.ElideRight
                                            }

                                            Text {
                                                width: parent.width
                                                text: (root.guideState.selectedChannel.name || "")
                                                    + ((root.guideState.selectedProgram.timeRange || "").length > 0
                                                        ? "  |  " + root.guideState.selectedProgram.timeRange
                                                        : "")
                                                color: Theme.textSecondary
                                                font.pixelSize: 12
                                                elide: Text.ElideRight
                                            }

                                            Text {
                                                id: descriptionText
                                                width: parent.width
                                                text: root.normalizeDescription(root.guideState.selectedProgram.description)
                                                color: Theme.textPrimary
                                                font.pixelSize: 13
                                                wrapMode: Text.Wrap
                                                maximumLineCount: 4
                                                elide: Text.ElideRight
                                            }

                                            RowLayout {
                                                visible: root.selectedCatchupState.visible
                                                width: parent.width
                                                spacing: 10

                                                AppButton {
                                                    text: "Play Catch-up"
                                                    enabled: root.selectedCatchupState.enabled
                                                    onClicked: root.playCatchupRequested(
                                                        root.guideState.selectedChannel,
                                                        root.selectedProgramData())
                                                }

                                                Text {
                                                    Layout.fillWidth: true
                                                    visible: !root.selectedCatchupState.enabled
                                                    text: root.selectedCatchupState.reason || ""
                                                    color: Theme.textSecondary
                                                    font.pixelSize: 12
                                                    wrapMode: Text.Wrap
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Item {
                            id: currentTimeLineLayer
                            visible: root.epgGrid.currentTimeOffsetMinutes >= 0
                                && root.epgGrid.currentTimeOffsetMinutes <= root.epgGrid.windowSpanMinutes
                            x: root.epgGrid.currentTimeOffsetMinutes * root.pixelsPerMinute
                            y: 0
                            width: 2
                            height: timelineFlick.height
                            clip: true
                            readonly property real gapTop: root.currentTimeLineGapTop(timelineRows.contentY)
                            readonly property real gapBottom: root.currentTimeLineGapBottom(timelineRows.contentY)
                            readonly property real topSegmentHeight: root.currentTimeLineHasDetailGap
                                ? Math.max(0, Math.min(height, gapTop))
                                : height
                            readonly property real bottomSegmentY: root.currentTimeLineHasDetailGap
                                ? Math.max(0, Math.min(height, gapBottom))
                                : height
                            readonly property real bottomSegmentHeight: root.currentTimeLineHasDetailGap
                                ? Math.max(0, height - bottomSegmentY)
                                : 0

                            Rectangle {
                                visible: height > 0
                                x: 0
                                y: 0
                                width: parent.width
                                height: parent.topSegmentHeight
                                color: root.currentTimeLineColor
                                opacity: root.currentTimeLineOpacity
                            }

                            Rectangle {
                                visible: height > 0
                                x: 0
                                y: parent.bottomSegmentY
                                width: parent.width
                                height: parent.bottomSegmentHeight
                                color: root.currentTimeLineColor
                                opacity: root.currentTimeLineOpacity
                            }
                        }
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: channelColumn.count === 0
                text: "No EPG rows available for this selection."
                color: Theme.textSecondary
                font.pixelSize: 14
            }
        }
    }
}
