pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme/Theme.js" as Theme

FocusScope {
    id: root
    property var catalog: null
    property int uiTransparency: 100
    property bool shown: false
    property bool inputAllowed: true
    property bool searchPending: false
    property bool listPending: false
    readonly property bool searchActive: searchHeader.field.activeFocus
    readonly property bool listActive: list.activeFocus
    readonly property bool hovered: panelHover.hovered
    readonly property bool animating: slideAnimation.running || fadeAnimation.running
    signal groupsRequested()
    signal interaction(string source)
    opacity: shown ? 1 : 0
    enabled: shown && inputAllowed && !animating
    clip: true
    readonly property real slideOffset: panelShift.x
    transform: Translate {
        id: panelShift
        x: root.shown ? 0 : -root.width
        Behavior on x { NumberAnimation { id: slideAnimation; duration: Theme.transitionMs; easing.type: Easing.OutCubic } }
    }
    Behavior on opacity { NumberAnimation { id: fadeAnimation; duration: Theme.transitionMs * 0.8 } }
    onEnabledChanged: { if (enabled && searchPending) focusSearch(); else if (enabled && listPending) focusList() }
    onShownChanged: {
        if (!shown) { searchPending = false; listPending = false; root.focus = false }
    }
    function focusSearch() {
        listPending = false
        searchPending = true
        if (!enabled) return
        searchPending = false
        searchHeader.field.forceActiveFocus()
    }
    function focusList() {
        searchPending = false
        listPending = true
        if (!enabled) return
        listPending = false
        if (list.count && list.currentIndex < 0) list.currentIndex = 0
        list.forceActiveFocus()
    }
    function navigate(delta) {
        root.interaction("keyboard")
        if (list.count) list.currentIndex = Math.max(0, Math.min(list.count - 1, list.currentIndex < 0 ? 0 : list.currentIndex + delta))
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
        focusList()
    }
    function activate() {
        if (root.catalog && !root.catalog.busy && list.currentIndex >= 0) root.catalog.playRow(list.currentIndex)
    }
    function fetchMore() {
        if (root.shown && root.catalog && root.catalog.hasMore && !root.catalog.busy && list.atYEnd)
            root.catalog.fetchMoreMovies()
    }
    Rectangle { anchors.fill: parent; color: Theme.uiBackground(Theme.overlaySidebar, root.uiTransparency) }
    HoverHandler { id: panelHover }
    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 4
        anchors.rightMargin: 8
        anchors.topMargin: Theme.spacingM
        anchors.bottomMargin: Theme.spacingM
        spacing: 12
        PlaybackSearchHeader {
            id: searchHeader
            Layout.fillWidth: true
            searchObjectName: "ui.vod.playback.search"
            switchActionObjectName: "ui.vod.playback.openGroups"
            placeholderText: "Search movies"
            switchText: "← Groups"
            text: root.catalog ? root.catalog.searchText : ""
            uiTransparency: root.uiTransparency
            onTextEdited: function(text) {
                if (root.catalog) root.catalog.searchText = text
                root.interaction("keyboard")
            }
            onSearchFocusChanged: root.interaction("focus")
            onSearchKeyPressed: function(event) {
                if (event.modifiers === Qt.NoModifier && (event.key === Qt.Key_Down || event.key === Qt.Key_Tab
                    || event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                    event.accepted = true
                    root.interaction("keyboard")
                    root.focusList()
                }
            }
            onPointerMoved: root.interaction("pointer")
            onSwitchRequested: root.groupsRequested()
        }
        Label {
            Layout.leftMargin: 8
            Layout.rightMargin: 4
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.catalog ? root.catalog.errorText : ""
            color: Theme.overlayTextSecondary
            wrapMode: Text.WordWrap
            font.pixelSize: 12
        }
        ListView {
            id: list
            objectName: "ui.vod.playback.list"
            Layout.leftMargin: 8
            Layout.rightMargin: 4
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.catalog
            currentIndex: -1
            spacing: 2
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            onAtYEndChanged: Qt.callLater(root.fetchMore)
            onCountChanged: Qt.callLater(root.fetchMore)
            onContentYChanged: { if (moving) root.interaction("pointer") }
            Keys.onUpPressed: root.navigate(-1)
            Keys.onDownPressed: root.navigate(1)
            Keys.onReturnPressed: root.activate()
            Keys.onEnterPressed: root.activate()
            delegate: Rectangle {
                id: row
                required property int index
                required property string movieKey
                required property string title
                required property string poster
                required property bool available
                readonly property bool playing: root.catalog !== null && movieKey === root.catalog.playingMovieKey
                readonly property bool inViewport: y + height > list.contentY && y < list.contentY + list.height
                objectName: "ui.vod.playback.row." + index
                width: list.width
                height: 124
                color: list.currentIndex === index ? Theme.uiBackground(Theme.overlaySurfaceInteractive, root.uiTransparency) : "transparent"
                Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 3; visible: row.playing; color: Theme.accent }
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 12
                    Rectangle {
                        Layout.preferredWidth: 68
                        Layout.preferredHeight: 96
                        color: Theme.uiBackground(Theme.overlaySurface, root.uiTransparency)
                        Image { id: poster; anchors.fill: parent; source: row.poster; fillMode: Image.PreserveAspectFit; asynchronous: true }
                        Text { anchors.centerIn: parent; visible: poster.status !== Image.Ready; text: "FILM"; color: Theme.overlayTextMuted; font.pixelSize: 11 }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label { Layout.fillWidth: true; text: row.title; color: row.available ? Theme.overlayTextPrimary : Theme.overlayTextMuted; font.pixelSize: 14; wrapMode: Text.WordWrap; maximumLineCount: 3; elide: Text.ElideRight }
                        Label { visible: row.playing; text: "Now playing"; color: Theme.overlayTextSecondary; font.pixelSize: 11 }
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    onClicked: { list.currentIndex = row.index; root.focusList(); root.interaction("keyboard") }
                    onDoubleClicked: { list.currentIndex = row.index; root.activate() }
                    onWheel: function(event) { root.interaction("pointer"); event.accepted = false }
                }
                Timer {
                    interval: 250
                    repeat: true
                    running: root.shown && row.inViewport && row.poster.length === 0
                    triggeredOnStart: true
                    onTriggered: { if (root.catalog) root.catalog.requestPoster(row.index) }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: list.count === 0
                text: root.catalog && root.catalog.busy ? "Loading movies…" : "No movies found"
                color: Theme.overlayTextMuted
            }
        }
    }
    Connections {
        target: root.catalog
        function onChanged() { Qt.callLater(root.fetchMore) }
    }
}
