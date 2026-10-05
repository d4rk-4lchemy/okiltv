pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import "../theme/Theme.js" as Theme

ComboBox {
    id: combo
    property bool optionActionsEnabled: false
    signal removeOption(int index)
    property real popupHeightLimit: 450
    property int uiTransparency: 100
    implicitWidth: 200
    implicitHeight: 44
    font.pixelSize: 14
    leftPadding: 14
    rightPadding: 30
    hoverEnabled: true
    ToolTip.visible: hovered && comboText.truncated
    ToolTip.text: displayText
    contentItem: Text {
        id: comboText
        text: combo.displayText
        font: combo.font
        color: combo.enabled ? Theme.overlayTextSecondary : Theme.overlayTextMuted
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: Theme.radiusS
        color: Theme.uiBackground(combo.hovered ? Theme.overlaySurfaceInteractive : Theme.overlaySurfaceRaised, combo.uiTransparency)
        border.width: 1
        border.color: combo.visualFocus ? Theme.overlayTextSecondary : Theme.overlayBorder
    }
    delegate: ItemDelegate {
        id: option
        required property int index
        readonly property var row: combo.optionActionsEnabled ? combo.model[index] : null
        enabled: !row || row.optionEnabled !== false
        width: combo.popup.availableWidth
        height: combo.height
        padding: 0
        leftPadding: combo.leftPadding
        rightPadding: removeButton.visible ? combo.height : combo.rightPadding
        hoverEnabled: true
        highlighted: combo.highlightedIndex === index
        contentItem: Text {
            id: optionText
            text: combo.textAt(option.index)
            font: combo.font
            color: Theme.overlayTextPrimary
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            color: option.highlighted || option.hovered ? Theme.overlaySurfaceInteractive : "transparent"
        }
        ToolButton {
            id: removeButton
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: combo.height
            height: combo.height
            visible: Boolean(option.row && option.row.externalId)
            text: "×"
            Accessible.name: "Remove uploaded subtitles"
            ToolTip.visible: hovered
            ToolTip.text: "Remove uploaded subtitles"
            onClicked: combo.removeOption(option.index)
        }
        ToolTip.visible: hovered && optionText.truncated
        ToolTip.text: combo.textAt(index)
    }
    popup: Popup {
        y: combo.height
        width: combo.width
        padding: 1
        implicitHeight: Math.min(options.contentHeight + topPadding + bottomPadding,
            Math.max(combo.height, combo.popupHeightLimit))
        contentItem: ListView {
            id: options
            clip: true
            model: combo.popup.visible ? combo.delegateModel : null
            currentIndex: combo.highlightedIndex
            ScrollBar.vertical: VodScrollBar {
                policy: options.contentHeight > options.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            }
        }
        background: Rectangle {
            color: Theme.uiBackground(Theme.overlaySurface, combo.uiTransparency)
            border.color: Theme.overlayBorder
            radius: Theme.radiusS
        }
    }
    Keys.onDeletePressed: {
        if (combo.optionActionsEnabled && combo.currentIndex >= 0 && combo.model[combo.currentIndex].externalId)
            combo.removeOption(combo.currentIndex)
    }
    palette.button: Theme.overlaySurfaceRaised
    palette.buttonText: Theme.overlayTextPrimary
    palette.base: Theme.overlaySurface
    palette.text: Theme.overlayTextPrimary
    palette.window: Theme.overlaySurface
    palette.windowText: Theme.overlayTextPrimary
    palette.highlight: Theme.overlaySurfaceInteractive
    palette.highlightedText: Theme.overlayTextPrimary
}
