pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme/Theme.js" as Theme

Item {
    id: root
    property var movie: ({})
    property int uiTransparency: 100
    property bool shown: false
    property bool inputAllowed: true
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
        anchors.fill: parent
        anchors.margins: 16
        contentWidth: availableWidth
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ColumnLayout {
            width: parent.width
            spacing: 14
            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: Math.min(199, root.width - 32)
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
}
