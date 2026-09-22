import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme/Theme.js" as Theme

Item {
    id: control

    // qmllint disable unqualified
    property int uiTransparency: (typeof settingsController !== "undefined") ? settingsController.uiTransparency : 100
    // qmllint enable unqualified

    required property var epgModel
    required property var appController
    required property var dvrController
    property string datePattern: "dddd dd.MM"
    property string timePattern: "HH:mm"
    onDatePatternChanged: rebuild()

    property url catchupIconSource: "qrc:/resources/icons/catch-up-indicator.svg"
    property url continueCatchupIconSource: "qrc:/resources/icons/continue-catch-up.svg"
    property int catchupProgressRevision: 0
    property string selectedKey: ""
    property bool keyboardSelectionActive: false
    property bool catchupActive: false
    property var playbackChannel: ({})
    property var playbackProgram: ({})
    property string centeredPlaybackKey: ""
    property string catchupSelectionKey: ""
    property bool pointerSelectionActive: false
    readonly property int catchupSelectionIndex: catchupActive
        ? entries.findIndex(function(entry) { return entry.key === control.catchupSelectionKey }) : -1
    readonly property string playbackKey: catchupActive
        && String(playbackChannel.profileId || "") === String(channelData.profileId || "")
        && playbackChannel.id !== undefined && playbackChannel.id === channelData.id
        && playbackProgram.start ? programKey(playbackProgram) : ""
    readonly property int playbackIndex: playbackKey.length > 0
        ? entries.findIndex(function(entry) { return entry.key === control.playbackKey }) : -1
    readonly property int preferredIndex: catchupSelectionIndex >= 0 ? catchupSelectionIndex
        : (playbackIndex >= 0 ? playbackIndex : defaultIndex)
    property var entries: []
    property int defaultIndex: -1
    property bool followingNow: true
    property string channelKey: ""
    property bool updating: false
    property bool updateRequested: false
    property int revision: 0
    readonly property var channelData: epgModel ? epgModel.channel : ({})

    signal selectionRequested(int index)
    signal activationRequested(int index)
    signal downloadRequested(int index)
    signal programHovered(var program, Item anchor)
    signal programReleased(Item anchor)
    signal scrolling()
    signal refreshed()

    function programKey(program) {
        return String(program.channelId || "").trim().toLowerCase() + ":" + String(program.start || "")
    }

    function indexForProgram(program) {
        const key = programKey(program)
        return entries.findIndex(function(entry) { return entry.key === key })
    }

    function isSpecialEntry(index) {
        const entry = entries[index]
        return Boolean(entry && (entry.kind === "now" || entry.kind === "next"
            || (playbackKey.length > 0 && entry.key === playbackKey)))
    }

    function itemAt(index) {
        return timeline.itemAtIndex(index)
    }

    function showIndex(index) {
        if (index < 0 || index >= entries.length)
            return
        timeline.positionViewAtIndex(index, ListView.Contain)
        timeline.forceLayout()
    }

    function centerPlayback(force) {
        if (force) {
            centeredPlaybackKey = ""
            catchupSelectionKey = playbackKey
            pointerSelectionActive = false
        }
        if (updating || playbackIndex < 0 || centeredPlaybackKey === playbackKey)
            return false
        updating = true
        timeline.cancelFlick()
        timeline.forceLayout()
        timeline.positionViewAtIndex(playbackIndex, ListView.Center)
        timeline.forceLayout()
        centeredPlaybackKey = playbackKey
        if (catchupSelectionIndex < 0)
            catchupSelectionKey = playbackKey
        followingNow = false
        updating = false
        scrolling()
        return true
    }

    function goToNow() {
        if (defaultIndex < 0)
            return
        updating = true
        timeline.cancelFlick()
        timeline.forceLayout()
        timeline.positionViewAtIndex(defaultIndex, ListView.Beginning)
        timeline.forceLayout()
        followingNow = true
        updating = false
        scrolling()
    }

    function updateFollowingNow() {
        if (updating || defaultIndex < 0)
            return
        const item = timeline.itemAtIndex(defaultIndex)
        // A short list may clamp to its beginning instead of aligning NOW at the top.
        followingNow = item !== null
            && (Math.abs(item.y - timeline.contentY) < 2
                || (timeline.atYEnd && item.y >= timeline.contentY
                    && item.y + item.height <= timeline.contentY + timeline.height))
    }

    function rebuild() {
        // Finish restoring the previous layout before capturing another anchor.
        // Defer any reentrant model notification until this update is complete.
        if (updating) {
            updateRequested = true
            return
        }
        if (!epgModel)
            return
        const nextChannelKey = String(channelData.profileId || "") + ":" + String(channelData.id)
        const changedChannel = nextChannelKey !== channelKey
        const keepNow = changedChannel || followingNow
        if (changedChannel)
            centeredPlaybackKey = ""
        let anchorKey = ""
        let anchorOffset = 0
        let anchorStart = 0
        if (!keepNow) {
            // Use instantiated delegate geometry: NOW/NEXT and date headers have variable heights.
            for (let i = 0; i < entries.length; ++i) {
                const item = timeline.itemAtIndex(i)
                if (item && item.y + item.height > timeline.contentY) {
                    anchorKey = entries[i].key
                    anchorStart = new Date(entries[i].program.start).getTime()
                    anchorOffset = item.y - timeline.contentY
                    break
                }
            }
        }
        const rows = []
        let lastDay = ""
        const seen = new Set()
        function append(program, kind) {
            if (!program || !program.title)
                return
            const key = control.programKey(program)
            if (seen.has(key))
                return
            seen.add(key)
            const date = new Date(program.start)
            const day = Qt.formatDateTime(date, "yyyy-MM-dd")
            rows.push({ key: key, program: program, kind: kind, listIndex: rows.length,
                          dateLabel: day !== lastDay ? Qt.locale("en_US").toString(date, control.datePattern) : "" })
            lastDay = day
        }
        const oldest = Date.now() - Number(channelData.catchupWindowHours || 0) * 3600000
        const past = epgModel.pastPrograms
        for (let i = 0; i < past.length; ++i) {
            if (new Date(past[i].start).getTime() >= oldest)
                append(past[i], "past")
        }
        const historyCount = rows.length
        append(epgModel.currentProgram, "now")
        append(epgModel.nextProgram, "next")
        const upcoming = epgModel.upcomingPrograms
        for (let i = 0; i < upcoming.length; ++i)
            append(upcoming[i], "upcoming")

        updating = true
        scrolling() // Destroyed/recycled delegates must not remain bubble anchors.
        followingNow = keepNow
        channelKey = nextChannelKey
        entries = rows
        defaultIndex = rows.length > historyCount ? historyCount : rows.length - 1
        ++revision
        // Apply the new model and restore its position in the same turn, before rendering.
        // Deferring this exposes the oldest archive rows for a frame on channel changes.
        timeline.forceLayout()
        control.updating = false
        const centeredPlayback = control.centerPlayback(false)
        control.updating = true
        if (!centeredPlayback && (keepNow || !anchorKey)) {
            control.goToNow()
        } else if (!centeredPlayback) {
            let index = control.entries.findIndex(function(entry) { return entry.key === anchorKey })
            if (index < 0) {
                index = control.entries.findIndex(function(entry) {
                    return new Date(entry.program.start).getTime() >= anchorStart
                })
                if (index < 0)
                    index = control.entries.length - 1
            }
            if (index >= 0) {
                timeline.positionViewAtIndex(index, ListView.Beginning)
                timeline.forceLayout()
                const item = timeline.itemAtIndex(index)
                if (item) {
                    const minY = timeline.originY
                    const maxY = Math.max(minY, timeline.originY + timeline.contentHeight - timeline.height)
                    timeline.contentY = Math.max(minY, Math.min(maxY, item.y - anchorOffset))
                }
            }
            control.followingNow = false
        }
        control.updating = false
        if (control.updateRequested) {
            control.updateRequested = false
            control.rebuild()
        } else {
            control.refreshed()
        }
    }

    onSelectedKeyChanged: {
        if (keyboardSelectionActive) {
            pointerSelectionActive = false
        }
        if (catchupActive && keyboardSelectionActive) {
            catchupSelectionKey = selectedKey
        }
    }
    onKeyboardSelectionActiveChanged: {
        if (keyboardSelectionActive) {
            pointerSelectionActive = false
        }
        if (catchupActive && keyboardSelectionActive && selectedKey.length > 0) {
            catchupSelectionKey = selectedKey
        }
    }
    onEpgModelChanged: rebuild()
    onPlaybackKeyChanged: {
        if (!catchupSelectionKey.length || catchupSelectionKey === centeredPlaybackKey || !playbackKey.length)
            catchupSelectionKey = playbackKey
        centeredPlaybackKey = ""
        Qt.callLater(function() { control.centerPlayback(false) })
    }
    onCatchupActiveChanged: {
        if (!catchupActive)
            Qt.callLater(function() { control.goToNow() })
    }
    onVisibleChanged: {
        if (visible)
            Qt.callLater(function() { control.centerPlayback(true) })
    }
    Component.onCompleted: rebuild()
    Connections {
        target: control.appController
        function onCatchupProgressChanged() { ++control.catchupProgressRevision }
    }
    Connections {
        target: control.epgModel
        function onDataChanged() { control.rebuild() }
        function onChannelChanged() { control.rebuild() }
    }

    Text {
        anchors.left: parent.left
        anchors.right: parent.right
        visible: control.entries.length === 0 && control.epgModel && !control.epgModel.loading
        text: "Programme data is not available for this channel right now."
        color: Theme.textMuted
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }

    HoverHandler {
        property point lastScenePosition: Qt.point(-1, -1)
        onPointChanged: {
            const position = point.scenePosition
            if (position.x === lastScenePosition.x && position.y === lastScenePosition.y)
                return
            lastScenePosition = position
            if (!hovered || control.updating)
                return
            control.pointerSelectionActive = true
            if (!control.catchupActive)
                return
            const local = control.mapToItem(timeline.contentItem, point.position.x, point.position.y)
            const item = timeline.itemAtIndex(timeline.indexAt(local.x, local.y))
            if (item) {
                item.selectPointerProgram()
            }
        }
    }

    ListView {
        id: timeline
        objectName: "ui.epg.timeline"
        anchors.fill: parent
        clip: true
        model: control.entries
        boundsBehavior: Flickable.StopAtBounds
        onMovementStarted: control.scrolling()
        onContentYChanged: {
            control.updateFollowingNow()
            if (!control.updating)
                control.scrolling()
        }
        ScrollBar.vertical: ScrollBar {
            id: scrollBar
            policy: timeline.contentHeight > timeline.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            width: 6
            padding: 0
            background: Rectangle { implicitWidth: 6; radius: 3; color: "#2a20364d" }
            contentItem: Rectangle {
                implicitWidth: 6
                radius: 3
                color: scrollBar.pressed ? Theme.borderStrong : "#8c4e88b8"
            }
        }

        delegate: Item {
            id: row
            required property var modelData
            required property int index
            readonly property var program: modelData.program
            readonly property bool card: control.catchupActive ? playing
                : modelData.kind === "now" || modelData.kind === "next"
            readonly property bool past: modelData.kind === "past"
            readonly property bool playing: control.playbackKey.length > 0 && control.playbackKey === modelData.key
            readonly property bool primaryCard: playing || modelData.kind === "now"
            readonly property bool inlineNow: control.catchupActive && modelData.kind === "now"
            readonly property real progressPercent: Math.min(100, Math.max(0,
                Number((playing ? control.playbackProgram : program).progressPercent || 0)))
            readonly property string episodeTitle: String(program.subTitle || "").trim()
            readonly property bool active: control.catchupActive
                ? control.catchupSelectionKey === modelData.key
                : ((control.pointerSelectionActive && rowHover.hovered)
                    || (control.keyboardSelectionActive && control.selectedKey === modelData.key))
            readonly property var catchupState: {
                // Re-evaluate on minute refresh and bookmark updates, without duplicating eligibility rules.
                const revision = control.revision
                const progressRevision = control.catchupProgressRevision
                return past ? control.appController.catchupActionState(control.channelData, program) : ({})
            }
            readonly property bool scheduled: {
                const count = control.dvrController.scheduledCount
                return control.dvrController.isProgramScheduled(
                    control.channelData, program)
            }
            width: ListView.view.width
            height: dateHeader.height + body.height + (card ? 10 : 0)
            objectName: "ui.epg.program." + index

            function selectPointerProgram() {
                control.catchupSelectionKey = row.modelData.key
                control.programHovered(row.program, body)
                control.pointerSelectionActive = true
            }


            Item {
                id: dateHeader
                width: parent.width
                height: visible ? 30 : 0
                visible: row.modelData.dateLabel.length > 0
                    && (row.index > 0 || Boolean(control.channelData.catchupSupported))
                RowLayout {
                    anchors.fill: parent
                    anchors.rightMargin: 9
                    spacing: 8
                    Text {
                        text: row.modelData.dateLabel
                        color: Theme.textSecondary
                        font.pixelSize: 10
                        font.bold: true
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: "#405164"
                        opacity: control.isSpecialEntry(row.index) || control.isSpecialEntry(row.index - 1)
                            ? 0 : 1
                    }
                }
            }
            Rectangle {
                id: body
                anchors.top: dateHeader.bottom
                width: parent.width
                height: row.card ? Math.max(row.primaryCard ? 94 : 86, cardContent.implicitHeight + 24) : 52
                radius: row.card ? 6 : 4
                color: row.active ? Theme.uiBackground("#ad1f2d3a", control.uiTransparency) : (row.card ? Theme.uiBackground("#96182431", control.uiTransparency) : "transparent")

                ColumnLayout {
                    id: cardContent
                    visible: row.card
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Rectangle {
                            implicitWidth: row.playing ? cardBadge.implicitWidth + 20
                                : (row.primaryCard ? 48 : 52)
                            implicitHeight: 24
                            radius: 4
                            color: row.primaryCard ? Theme.accent : "#273244"
                            Text {
                                id: cardBadge
                                objectName: "ui.epg.cardBadge"
                                anchors.centerIn: parent
                                text: row.playing ? "CATCH-UP" : row.modelData.kind.toUpperCase()
                                color: row.primaryCard ? Theme.textPrimary : Theme.textSecondary
                                font.pixelSize: 11
                                font.bold: true
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: row.program.timeRange || ""
                            color: Theme.textSecondary
                            font.pixelSize: 12
                        }
                        Rectangle {
                            Layout.preferredWidth: 9
                            Layout.preferredHeight: 9
                            radius: 4.5
                            color: "#ff3a3a"
                            visible: row.scheduled
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 5
                        Text {
                            Layout.fillWidth: true
                            text: row.program.title
                            color: Theme.textPrimary
                            font.pixelSize: row.primaryCard ? 15 : 14
                            font.bold: true
                            wrapMode: Text.Wrap
                            maximumLineCount: row.episodeTitle.length > 0 ? 1 : 2
                            elide: Text.ElideRight
                        }
                        Text {
                            objectName: "ui.epg.cardNow"
                            visible: row.inlineNow
                            text: "NOW"
                            color: Theme.textMuted
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: row.episodeTitle.length > 0
                        text: row.episodeTitle
                        color: Theme.textSecondary
                        font.pixelSize: 12
                        font.italic: true
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        objectName: "ui.epg.progressTrack"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 4
                        radius: 2
                        color: "#283542"
                        visible: row.playing || (row.modelData.kind === "now" && row.progressPercent > 0)
                        Rectangle {
                            objectName: "ui.epg.progressFill"
                            width: parent.width * row.progressPercent / 100
                            height: parent.height
                            radius: parent.radius
                            color: Theme.accent
                        }
                    }
                }

                RowLayout {
                    visible: !row.card
                    anchors.fill: parent
                    anchors.leftMargin: 2
                    anchors.rightMargin: 9
                    spacing: 2
                    Text {
                        Layout.preferredWidth: control.timePattern.indexOf("AP") >= 0 ? 65 : 40
                        text: row.program.startTimeLabel || ""
                        color: Theme.textSecondary
                        font.pixelSize: 10
                    }
                    Rectangle {
                        Layout.preferredWidth: 8
                        Layout.preferredHeight: 8
                        radius: 4
                        color: "#ff3a3a"
                        visible: row.scheduled
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 5
                            Text {
                                Layout.fillWidth: true
                                text: row.program.title
                                color: Theme.textPrimary
                                font.pixelSize: 11
                                font.bold: true
                                wrapMode: Text.Wrap
                                maximumLineCount: row.episodeTitle.length > 0 ? 1 : 2
                                elide: Text.ElideRight
                            }
                            Text {
                                objectName: "ui.epg.rowNow"
                                visible: row.inlineNow
                                text: "NOW"
                                color: Theme.textMuted
                                font.pixelSize: 10
                                font.bold: true
                            }
                            Image {
                                visible: row.past && Boolean(row.catchupState.resumeAvailable)
                                Layout.preferredWidth: 14
                                Layout.preferredHeight: 14
                                Layout.rightMargin: 5
                                source: visible ? control.continueCatchupIconSource : ""
                                fillMode: Image.PreserveAspectFit
                            }
                            Image {
                                visible: row.past
                                Layout.preferredWidth: 14
                                Layout.preferredHeight: 14
                                source: row.past ? control.catchupIconSource : ""
                                opacity: row.catchupState.enabled ? 1 : 0.35
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: row.episodeTitle.length > 0
                            text: row.episodeTitle
                            color: Theme.textSecondary
                            font.pixelSize: 11
                            font.italic: true
                            elide: Text.ElideRight
                        }
                    }
                }
                Rectangle {
                    visible: !control.isSpecialEntry(row.index) && !control.isSpecialEntry(row.index + 1)
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: "#263240"
                    opacity: 0.85
                }
                HoverHandler {
                    id: rowHover
                    acceptedDevices: PointerDevice.Mouse
                    onHoveredChanged: {
                        if (hovered) {
                            if (!control.pointerSelectionActive)
                                return
                            // Catch-up selection follows actual scene-position changes
                            // in the outer HoverHandler. Layout/scrolling can hover a
                            // different delegate beneath a stationary pointer.
                            if (!control.catchupActive) {
                                control.programHovered(row.program, body)
                            }
                        } else
                            control.programReleased(body)
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                    onClicked: function(mouse) {
                        control.selectionRequested(row.index)
                        if (mouse.button === Qt.MiddleButton)
                            control.downloadRequested(row.index)
                    }
                    onDoubleClicked: function(mouse) {
                        if (mouse.button === Qt.LeftButton)
                            control.activationRequested(row.index)
                    }
                }
            }
        }
    }
}
