import QtQuick
import QtQuick.Controls
import "../theme/Theme.js" as Theme

Switch {
    id: control

    // qmllint disable unqualified
    property int uiTransparency: (typeof settingsController !== "undefined") ? settingsController.uiTransparency : 100
    // qmllint enable unqualified

    implicitWidth: 52
    implicitHeight: 30
    hoverEnabled: true
    spacing: 0

    indicator: Rectangle {
        implicitWidth: 52
        implicitHeight: 30
        radius: height / 2
        color: {
            if (!control.enabled)
                return Theme.uiBackground("#48303942", control.uiTransparency)
            if (control.checked)
                return "#7f94a3"
            if (control.hovered)
                return Theme.uiBackground("#5b4a5763", control.uiTransparency)
            return Theme.uiBackground("#4c424e59", control.uiTransparency)
        }
        border.width: control.visualFocus ? 1 : 0
        border.color: control.visualFocus ? "#96acbc" : "transparent"

        Rectangle {
            width: 22
            height: 22
            radius: 11
            x: control.checked ? parent.width - width - 4 : 4
            y: 4
            color: "#f4f7fb"

            Behavior on x {
                NumberAnimation {
                    duration: Theme.transitionMs
                }
            }
        }
    }

    contentItem: Item {}
    background: Item {}
}
