pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "../theme/Theme.js" as Theme

FocusScope {
    id: root

    property string selectedMode: "live"
    property int uiTransparency: 100
    property string actionPrefix: "ui.navigation"
    readonly property bool interactionActive: hover.hovered || root.activeFocus
    readonly property int selectedIndex: selectedMode === "movies" ? 0 : selectedMode === "series" ? 2 : 1
    property int focusedIndex: selectedIndex
    signal modeRequested(string mode)
    signal dismissed()

    implicitWidth: Theme.mediaModeWidth
    implicitHeight: Theme.mediaModeHeight
    activeFocusOnTab: true

    function choose(index) {
        if (!root.enabled) return;
        root.focusedIndex = index;
        root.modeRequested(index === 0 ? "movies" : index === 2 ? "series" : "live");
    }

    onActiveFocusChanged: {
        if (activeFocus) root.focusedIndex = root.selectedIndex;
    }
    onEnabledChanged: {
        if (!enabled && root.activeFocus) root.dismissed();
    }

    Keys.onShortcutOverride: event => {
        if (event.key === Qt.Key_Left || event.key === Qt.Key_Right || event.key === Qt.Key_Return
            || event.key === Qt.Key_Enter || event.key === Qt.Key_Space || event.key === Qt.Key_Escape) event.accepted = true;
    }
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
            root.focusedIndex = Math.max(0, Math.min(2, root.focusedIndex + (event.key === Qt.Key_Left ? -1 : 1)));
            event.accepted = true;
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
            root.choose(root.focusedIndex);
            event.accepted = true;
        } else if (event.key === Qt.Key_Escape) {
            root.dismissed();
            event.accepted = true;
        }
    }

    HoverHandler { id: hover }

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: Theme.uiBackground(Theme.mediaModeBackground, root.uiTransparency)
    }

    Rectangle {
        id: indicator
        objectName: root.actionPrefix + ".indicator"
        x: Theme.mediaModeInset + root.selectedIndex * width
        y: Theme.mediaModeInset
        width: (root.width - Theme.mediaModeInset * 2) / 3
        height: root.height - Theme.mediaModeInset * 2
        radius: height / 2
        color: Theme.mediaModeSelection
        Behavior on x { NumberAnimation { duration: Theme.transitionMs; easing.type: Easing.OutCubic } }
    }

    Repeater {
        model: ["Movies", "Live TV", "Series"]
        delegate: AbstractButton {
            id: segment
            required property int index
            required property string modelData
            readonly property string mode: index === 0 ? "movies" : index === 2 ? "series" : "live"
            objectName: root.actionPrefix + "." + mode
            x: Theme.mediaModeInset + index * width
            y: Theme.mediaModeInset
            width: (root.width - Theme.mediaModeInset * 2) / 3
            height: root.height - Theme.mediaModeInset * 2
            text: modelData
            hoverEnabled: true
            focusPolicy: Qt.NoFocus
            Accessible.role: Accessible.RadioButton
            Accessible.name: text
            Accessible.checkable: true
            Accessible.checked: root.selectedIndex === index
            onClicked: root.choose(index)
            background: Rectangle {
                radius: height / 2
                color: segment.down ? "#20ffffff" : segment.hovered ? "#0dffffff" : "transparent"
                border.width: root.activeFocus && root.focusedIndex === segment.index ? 1 : 0
                border.color: Theme.overlayTextSecondary
            }
            contentItem: Text {
                text: segment.text
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                color: root.enabled ? (root.selectedIndex === segment.index ? Theme.overlayTextPrimary : Theme.overlayTextSecondary) : Theme.overlayTextMuted
                font.pixelSize: Theme.mediaModeFontSize
                font.bold: root.selectedIndex === segment.index
            }
        }
    }
}
