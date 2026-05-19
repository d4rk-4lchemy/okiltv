import QtQuick
import QtQuick.Controls
import "../theme/Theme.js" as Theme

Switch {
    id: control

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
                return "#48303942"
            if (control.checked)
                return "#7f94a3"
            if (control.hovered)
                return "#5b4a5763"
            return "#4c424e59"
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
