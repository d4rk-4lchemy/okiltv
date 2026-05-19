import QtQuick
import QtQuick.Controls
import "../theme/Theme.js" as Theme

Button {
    id: control

    property bool accent: false
    property bool danger: false
    property bool compact: false
    property bool borderless: false

    implicitWidth: Math.max(compact ? 80 : 96, contentItem.implicitWidth + leftPadding + rightPadding)
    implicitHeight: compact ? 34 : 40

    leftPadding: compact ? 14 : 16
    rightPadding: compact ? 14 : 16
    topPadding: compact ? 7 : 10
    bottomPadding: compact ? 7 : 10
    font.pixelSize: 14
    hoverEnabled: true

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.enabled ? Theme.textPrimary : Theme.textMuted
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        renderType: Text.NativeRendering
    }

    background: Rectangle {
        radius: Theme.radiusS
        color: {
            if (!control.enabled)
                return Theme.surface
            if (control.danger)
                return control.down ? "#a84f4f" : (control.hovered ? "#d56767" : Theme.danger)
            if (control.checked)
                return Theme.accentMuted
            if (control.accent)
                return control.down ? "#4c93d4" : Theme.accent
            if (control.down)
                return Theme.accentMuted
            if (control.hovered)
                return Theme.surfaceInteractive
            return Theme.surfaceRaised
        }
        border.width: control.borderless ? 0 : 1
        border.color: {
            if (control.danger) {
                return control.visualFocus ? "#e3a0a0" : "#b26262"
            }
            return (control.visualFocus || control.checked || control.accent) ? Theme.borderStrong : Theme.border
        }
    }
}
