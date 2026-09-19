import QtQuick
import QtQuick.Controls
import "../theme/Theme.js" as Theme

TextField {
    id: control

    // qmllint disable unqualified
    property int uiTransparency: (typeof settingsController !== "undefined") ? settingsController.uiTransparency : 100
    // qmllint enable unqualified

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
                return Theme.uiBackground("#53232c34", control.uiTransparency)
            if (control.activeFocus)
                return Theme.uiBackground("#8a2b343d", control.uiTransparency)
            if (control.hovered)
                return Theme.uiBackground("#7d273039", control.uiTransparency)
            return Theme.uiBackground("#71242d35", control.uiTransparency)
        }
        border.width: control.activeFocus ? 1 : 0
        border.color: control.activeFocus ? "#96acbc" : "transparent"
    }
}
