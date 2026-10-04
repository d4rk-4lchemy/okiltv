pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme/Theme.js" as Theme

Item {
    id: root
    property var movie: ({})
    property var episodeModel: null
    property int uiTransparency: 100
    property bool shown: false
    property bool inputAllowed: true
    property real topContentInset: 0
    readonly property bool episodesActive: seriesEpisodes.listActive
    readonly property bool seasonFocused: Boolean(episodeModel) && seriesEpisodes.searchActive
    readonly property bool seasonPopupOpen: seriesEpisodes.seasonPopupOpen
    property bool pendingEpisodesFocus: false
    function focusEpisodes() {
        pendingEpisodesFocus = true
        if (enabled && Boolean(episodeModel)) {
            seriesEpisodes.focusEpisodes(); pendingEpisodesFocus = false
        }
    }
    onEnabledChanged: { if (enabled && pendingEpisodesFocus) focusEpisodes() }
    onShownChanged: { if (!shown) pendingEpisodesFocus = false }
    function handleEpisodeKey(event) { return Boolean(episodeModel) && episodesActive && seriesEpisodes.handleKey(event) }
    function closeSeasonPopup() { seriesEpisodes.closePopup() }
    signal settingsRequested()
    readonly property bool hovered: panelHover.hovered
    readonly property bool animating: slideAnimation.running || fadeAnimation.running
    opacity: shown ? 1 : 0
    enabled: shown && inputAllowed && !animating
    clip: true
    transform: Translate {
        x: root.shown ? 0 : root.width
        Behavior on x { NumberAnimation { id: slideAnimation; duration: Theme.transitionMs; easing.type: Easing.OutCubic } }
    }
    Behavior on opacity { NumberAnimation { id: fadeAnimation; duration: Theme.transitionMs * 0.8 } }
    Rectangle { anchors.fill: parent; color: Theme.uiBackground(Theme.overlaySidebar, root.uiTransparency) }
    HoverHandler { id: panelHover }
    ScrollView {
        id: movieInfo
        anchors.fill: parent
        anchors.margins: 16
        anchors.topMargin: 16 + root.topContentInset
        rightPadding: movieInfo.ScrollBar.vertical.width + Theme.vodScrollBarGap
        contentWidth: availableWidth
        visible: !root.episodeModel
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical: VodScrollBar {
            parent: movieInfo
            x: movieInfo.width - width
            y: movieInfo.topPadding
            height: movieInfo.availableHeight
        }
        ColumnLayout {
            width: movieInfo.availableWidth
            spacing: 14
            Rectangle {
                Layout.alignment: Qt.AlignLeft
                Layout.preferredWidth: Math.min(199, Math.max(0, root.width - 16 - 12 - 46 - 8))
                Layout.preferredHeight: width * 282 / 199
                color: Theme.uiBackground(Theme.overlaySurface, root.uiTransparency)
                Image { id: poster; anchors.fill: parent; source: root.movie.poster || ""; fillMode: Image.PreserveAspectFit; asynchronous: true }
                Text { anchors.centerIn: parent; visible: poster.status !== Image.Ready; text: "FILM"; color: Theme.overlayTextMuted; font.pixelSize: 16 }
            }
            Label { objectName: "ui.vod.playback.title"; Layout.fillWidth: true; text: root.movie.title || ""; color: Theme.overlayTextPrimary; font.pixelSize: 20; font.bold: true; wrapMode: Text.WordWrap }
            Label {
                Layout.fillWidth: true
                text: [root.movie.year || "", root.movie.durationMinutes > 0 ? root.movie.durationMinutes + " min" : "", root.movie.genres || ""].filter(part => part.length > 0).join(" · ")
                visible: text.length > 0
                color: Theme.overlayTextSecondary
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
            Label { Layout.fillWidth: true; text: root.movie.description || ""; visible: text.length > 0; color: Theme.overlayTextPrimary; font.pixelSize: 14; wrapMode: Text.WordWrap }
            Label { text: "CAST"; visible: Boolean(root.movie.cast); color: Theme.overlayTextMuted; font.pixelSize: 11 }
            Label { Layout.fillWidth: true; text: root.movie.cast || ""; visible: text.length > 0; color: Theme.overlayTextSecondary; font.pixelSize: 13; wrapMode: Text.WordWrap }
        }
    }
    Label {
        anchors.top: parent.top; anchors.topMargin: 16 + root.topContentInset
        anchors.left: parent.left; anchors.leftMargin: 16
        anchors.right: parent.right; anchors.rightMargin: 70
        height: 46; visible: Boolean(root.episodeModel)
        text: root.movie.title || ""; color: Theme.overlayTextPrimary
        font.pixelSize: 18; font.bold: true; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter
    }
    VodEpisodesList {
        id: seriesEpisodes
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 78 + root.topContentInset
        anchors.bottomMargin: 16
        visible: Boolean(root.episodeModel)
        episodeModel: root.episodeModel
        directPlay: true
        marqueeEnabled: true
        uiTransparency: root.uiTransparency
    }
    Rectangle {
        width: 46
        height: 46
        anchors.top: parent.top
        anchors.topMargin: 14 + root.topContentInset
        anchors.right: parent.right
        anchors.rightMargin: 12
        radius: 8
        color: settingsButton.down ? Theme.uiBackground("#ad1f2d3a", root.uiTransparency) : (settingsButton.hovered ? Theme.uiBackground("#a71c2936", root.uiTransparency) : Theme.uiBackground("#96182431", root.uiTransparency))

        IconActionButton {
            id: settingsButton
            objectName: "ui.vod.playback.settingsButton"
            anchors.fill: parent
            uiTransparency: root.uiTransparency
            compact: true
            borderless: true
            barMode: true
            iconInset: 2
            iconSource: "qrc:/resources/icons/settings.svg"
            caption: "Settings"
            onClicked: root.settingsRequested()
        }
    }
}
