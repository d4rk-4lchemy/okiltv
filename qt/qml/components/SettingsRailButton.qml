import QtQuick
import QtQuick.Controls
import "../theme/Theme.js" as Theme

Button {
    id: control

    property string glyph: ""
    property url iconSource: ""
    property string caption: ""
    property bool active: false
    property bool accent: false
    property bool danger: false
    property color activeFillColor: "#ad2b343d"
    property color hoverFillColor: "#68252e37"
    property color pressedFillColor: "#8b29323b"
    property color activeIndicatorColor: "#f4f7fb"

    implicitWidth: Theme.railWidth - 16
    implicitHeight: 70
    hoverEnabled: true

    contentItem: Item {
        Rectangle {
            anchors.left: parent.left
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            width: 4
            height: parent.height - 22
            radius: 2
            visible: control.active
            color: control.activeIndicatorColor
        }

        Image {
            id: iconImage

            anchors.centerIn: parent
            width: 28
            height: 28
            visible: control.iconSource.toString().length > 0
            source: control.iconSource
            fillMode: Image.PreserveAspectFit
            smooth: true
            mipmap: true
            opacity: !control.enabled ? 0.24 : (control.active || control.accent ? 1 : (control.hovered ? 0.92 : 0.82))
        }

        Text {
            anchors.centerIn: parent
            visible: !iconImage.visible
            text: control.glyph
            color: control.enabled ? Theme.textPrimary : Theme.textMuted
            font.pixelSize: control.glyph.length > 1 ? 18 : 22
            font.bold: true
            renderType: Text.NativeRendering
        }
    }

    background: Rectangle {
        radius: Theme.radiusM
        color: {
            if (control.active)
                return control.activeFillColor
            if (control.down)
                return control.pressedFillColor
            if (control.hovered)
                return control.hoverFillColor
            return "transparent"
        }
        border.width: 0
    }

    ToolTip.visible: hovered && caption.length > 0
    ToolTip.text: caption
}
