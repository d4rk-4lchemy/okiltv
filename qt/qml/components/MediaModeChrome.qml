import QtQuick
import "../theme/Theme.js" as Theme

Item {
    id: root

    property bool targetVisible: false
    property string selectedMode: "live"
    property bool navigationEnabled: true
    property int uiTransparency: 100
    readonly property bool navigationFocused: modeSwitch.activeFocus
    readonly property real navigationWidth: modeSwitch.width
    readonly property bool animating: fadeAnimation.running || slideAnimation.running
    readonly property int occupiedHeight: visible ? height : 0
    readonly property bool interactionActive: enabled && modeSwitch.interactionActive
    signal modeRequested(string mode)
    signal navigationDismissed()

    height: Theme.mediaModeChromeHeight
    visible: targetVisible || opacity > 0.01
    enabled: targetVisible && opacity > 0.01 && !animating
    opacity: targetVisible ? 1 : 0

    function focusNavigation() { modeSwitch.forceActiveFocus(Qt.TabFocusReason) }

    Behavior on opacity {
        NumberAnimation { id: fadeAnimation; duration: Theme.transitionMs * 0.8; easing.type: Easing.OutCubic }
    }
    transform: Translate {
        y: root.targetVisible ? 0 : -(root.height + Theme.spacingS)
        Behavior on y {
            NumberAnimation { id: slideAnimation; duration: Theme.transitionMs; easing.type: Easing.OutCubic }
        }
    }

    // Disabled navigation consumes clicks instead of closing an overlay below it.
    MouseArea {
        anchors.fill: modeSwitch
        acceptedButtons: Qt.AllButtons
    }
    MediaModeSwitch {
        id: modeSwitch
        z: 1
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(Theme.mediaModeMinimumWidth, Math.min(Theme.mediaModeWidth, root.width - 24))
        height: Theme.mediaModeHeight
        selectedMode: root.selectedMode
        uiTransparency: root.uiTransparency
        enabled: root.navigationEnabled
        onModeRequested: mode => root.modeRequested(mode)
        onDismissed: root.navigationDismissed()
    }
}
