pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme/Theme.js" as Theme

Dialog {
    id: root
    required property var controller
    property bool allowedToOpen: true
    property int uiTransparency: 100
    readonly property bool compactWindow: parent ? parent.height < 320 : false

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    focus: true
    title: "Update available"
    closePolicy: Popup.CloseOnEscape
    width: Math.min(440, parent ? parent.width - 32 : 440)
    height: Math.min(implicitHeight, parent ? parent.height - 32 : implicitHeight)
    padding: compactWindow ? 12 : 20
    spacing: compactWindow ? 8 : 16

    function syncVisibility() {
        if (root.controller.pending && root.allowedToOpen) {
            if (!root.visible)
                root.open()
        } else if (root.visible) {
            root.close()
        }
    }

    onAllowedToOpenChanged: syncVisibility()
    Component.onCompleted: syncVisibility()
    onOpened: notNowButton.forceActiveFocus()
    onRejected: root.controller.dismiss()

    Connections {
        target: root.controller
        function onChanged() { root.syncVisibility() }
    }

    background: Rectangle {
        radius: 8
        color: Theme.uiBackground("#e6070d12", root.uiTransparency)
    }

    header: Text {
        text: root.title
        color: Theme.textPrimary
        font.pixelSize: 16
        font.bold: true
        leftPadding: root.leftPadding
        rightPadding: root.rightPadding
        topPadding: root.topPadding
        wrapMode: Text.Wrap
    }

    contentItem: ScrollView {
        id: messageScroll
        objectName: "updateMessage"
        implicitHeight: messageBody.implicitHeight
        contentWidth: availableWidth
        contentHeight: messageBody.implicitHeight
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ColumnLayout {
            id: messageBody
            width: messageScroll.availableWidth
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: "A new version of OKILTV is available.\n"
                    + "Current version: " + root.controller.currentVersion + "\n"
                    + "Latest version: " + root.controller.latestVersion + (root.compactWindow ? "\n" : "\n\n")
                    + "Open the release page?"
                font.pixelSize: 14
                wrapMode: Text.Wrap
                color: Theme.textPrimary
            }
            Text {
                Layout.fillWidth: true
                visible: text.length > 0
                text: root.controller.errorText
                font.pixelSize: 14
                wrapMode: Text.Wrap
                color: Theme.textPrimary
            }
        }
    }

    component UpdateAction: AppButton {
        id: action
        compact: true
        borderless: true
        Keys.onReturnPressed: action.clicked()
        Keys.onEnterPressed: action.clicked()
        background: Rectangle {
            radius: 4
            color: action.down ? "#35ffffff"
                : (action.hovered || action.visualFocus ? "#20ffffff" : "transparent")
        }
    }

    footer: Flow {
        padding: root.padding
        spacing: 4
        layoutDirection: Qt.LeftToRight
        UpdateAction {
            objectName: "updateYes"
            text: "Yes"
            onClicked: root.controller.openRelease()
        }
        UpdateAction {
            objectName: "updateSkip"
            text: "Skip this version"
            onClicked: root.controller.skipVersion()
        }
        UpdateAction {
            id: notNowButton
            objectName: "updateNotNow"
            text: "Not now"
            onClicked: root.controller.dismiss()
        }
    }
}
