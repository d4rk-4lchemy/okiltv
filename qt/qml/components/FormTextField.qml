import QtQuick
import QtQuick.Controls
import "../theme/Theme.js" as Theme

TextField {
    id: control

    implicitHeight: 44
    hoverEnabled: true
    leftPadding: 14
    rightPadding: 14
    topPadding: 10
    bottomPadding: 10
    color: Theme.textPrimary
    placeholderTextColor: Theme.textMuted
    selectionColor: Theme.accentMuted
    selectedTextColor: Theme.textPrimary
    font.pixelSize: 14

    background: Rectangle {
        radius: Theme.radiusM
        color: {
            if (!control.enabled)
                return "#53232c34"
            if (control.activeFocus)
                return "#8a2b343d"
            if (control.hovered)
                return "#7d273039"
            return "#71242d35"
        }
        border.width: control.activeFocus ? 1 : 0
        border.color: control.activeFocus ? "#96acbc" : "transparent"
    }
}
