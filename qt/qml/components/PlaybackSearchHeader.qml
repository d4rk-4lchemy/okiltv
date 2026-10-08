pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme/Theme.js" as Theme

Item {
    id: root

    property alias text: search.text
    property alias placeholderText: search.placeholderText
    readonly property alias field: search
    property string searchObjectName: ""
    property string switchObjectName: ""
    property string switchActionObjectName: ""
    property string switchText: ""
    property int uiTransparency: 100
    property bool neutralPalette: true
    implicitHeight: 42

    signal textEdited(string text)
    signal searchFocusChanged(bool focused)
    signal searchKeyPressed(var event)
    signal switchRequested()
    signal pointerMoved()

    RowLayout {
        anchors.fill: parent
        spacing: Theme.spacingS

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 4
            color: Theme.uiBackground(root.neutralPalette ? Theme.overlaySurface : Theme.playbackSearchBackground, root.uiTransparency)
            border.width: search.activeFocus ? 1 : 0
            border.color: root.neutralPalette ? Theme.overlayBorder : Theme.borderStrong

            TextField {
                id: search
                objectName: root.searchObjectName
                anchors.fill: parent
                anchors.margins: 1
                leftPadding: 12
                rightPadding: 12
                topPadding: 10
                bottomPadding: 10
                placeholderTextColor: root.neutralPalette ? Theme.overlayTextMuted : Theme.textMuted
                color: root.neutralPalette ? Theme.overlayTextPrimary : Theme.textPrimary
                font.pixelSize: Theme.playbackSearchFontSize
                selectByMouse: true
                hoverEnabled: true
                background: Item {}
                onTextEdited: root.textEdited(text)
                onActiveFocusChanged: root.searchFocusChanged(activeFocus)
                Keys.onPressed: function(event) { root.searchKeyPressed(event) }
            }
        }

        Rectangle {
            objectName: root.switchActionObjectName
            readonly property string caption: root.switchText
            Layout.preferredWidth: 78
            Layout.fillHeight: true
            radius: 4
            color: switchArea.containsMouse
                ? Theme.uiBackground(root.neutralPalette ? Theme.overlaySurfaceInteractive : "#6d111a24", root.uiTransparency)
                : "transparent"

            Text {
                anchors.centerIn: parent
                objectName: root.switchObjectName
                text: root.switchText
                color: root.neutralPalette
                    ? (switchArea.containsMouse ? Theme.overlayTextPrimary : Theme.overlayTextSecondary)
                    : (switchArea.containsMouse ? Theme.textPrimary : Theme.textSecondary)
                font.pixelSize: 13
            }

            MouseArea {
                id: switchArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onPositionChanged: root.pointerMoved()
                onClicked: root.switchRequested()
            }
        }
    }
}
