pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "../theme/Theme.js" as Theme

ComboBox {
    id: control

    implicitHeight: 44
    leftPadding: 14
    rightPadding: 36
    font.pixelSize: 14
    hoverEnabled: true

    contentItem: Text {
        text: control.displayText
        color: Theme.textPrimary
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        leftPadding: 0
        rightPadding: 0
        renderType: Text.NativeRendering
    }

    indicator: Text {
        text: "\u25be"
        color: Theme.textSecondary
        font.pixelSize: 12
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
    }

    background: Rectangle {
        radius: Theme.radiusM
        color: {
            if (!control.enabled)
                return "#53232c34"
            if (control.visualFocus)
                return "#8a2b343d"
            if (control.hovered)
                return "#7d273039"
            return "#71242d35"
        }
        border.width: control.visualFocus ? 1 : 0
        border.color: control.visualFocus ? "#96acbc" : "transparent"
    }

    popup: Popup {
        y: control.height + 6
        width: control.width
        padding: 6

        background: Rectangle {
            radius: Theme.radiusM
            color: "#de202931"
            border.width: 0
        }

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null

            delegate: ItemDelegate {
                id: optionDelegate
                required property int index
                required property string modelData

                width: control.width - 12
                height: 38
                hoverEnabled: true

                contentItem: Text {
                    text: optionDelegate.modelData
                    color: Theme.textPrimary
                    font.pixelSize: 14
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 12
                    renderType: Text.NativeRendering
                }

                background: Rectangle {
                    radius: Theme.radiusS
                    color: optionDelegate.highlighted ? "#8c29343d" : (optionDelegate.hovered ? "#5f252e37" : "transparent")
                    border.width: 0
                }

                onClicked: control.currentIndex = optionDelegate.index
            }
        }
    }
}
