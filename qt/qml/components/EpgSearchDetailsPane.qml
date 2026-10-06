pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import "../theme/Theme.js" as Theme

FocusScope {
    id: root
    objectName: "ui.epgSearch.details"
    required property var controller
    property bool inputBlocked: false
    property int uiTransparency: 100
    property bool compact: false
    property string subTitle: ""
    readonly property var details: controller.selectedDetails || ({})
    readonly property var focusTargets: [descriptionView, primaryButton, beginningButton, recordingButton, downloadButton]
    implicitHeight: column.implicitHeight
    signal tabRequested(bool backwards)
    signal controlFocused(var item)
    function focusDetails() { descriptionView.forceActiveFocus() }
    function handleTab(event) {
        if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
            && (event.modifiers === Qt.NoModifier || event.modifiers === Qt.ShiftModifier)) {
            if (!event.isAutoRepeat) root.tabRequested(event.key === Qt.Key_Backtab || event.modifiers === Qt.ShiftModifier)
            event.accepted = true
        }
    }
    component Action: Button {
        id: action
        required property url iconSource
        width: 28
        height: 28
        padding: 5
        hoverEnabled: true
        activeFocusOnTab: true
        Accessible.name: text
        ToolTip.text: text
        ToolTip.visible: hovered
        ToolTip.delay: 400
        onActiveFocusChanged: if (activeFocus) root.controlFocused(action)
        Keys.onPressed: event => {
            root.handleTab(event)
            if (!event.accepted && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                && event.modifiers === Qt.NoModifier && !event.isAutoRepeat) {
                action.clicked()
                event.accepted = true
            }
        }
        contentItem: Image {
            objectName: action.objectName + ".icon"
            source: action.iconSource
            fillMode: Image.PreserveAspectFit
            smooth: true
            mipmap: true
            opacity: action.enabled ? 1 : 0.36
        }
        background: Rectangle {
            color: Theme.uiBackground(action.down ? Theme.liveRailPressed
                : action.hovered ? Theme.liveRailHover : Theme.liveRailBackground, root.uiTransparency)
            border.width: action.activeFocus ? 1 : 0
            border.color: Theme.borderStrong
            radius: 4
        }
    }
    Column {
        id: column
        width: parent.width
        spacing: 6
        ScrollView {
            id: descriptionView
            objectName: "ui.epgSearch.description"
            width: parent.width
            height: Math.min(root.compact ? 40 : 48, Math.max(18, descriptionColumn.implicitHeight))
            clip: true
            activeFocusOnTab: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            Keys.onPressed: event => root.handleTab(event)
            onActiveFocusChanged: if (activeFocus) root.controlFocused(descriptionView)
            Accessible.name: qsTr("Selected programme details")
            background: Rectangle {
                color: "transparent"
                border.width: descriptionView.activeFocus ? 1 : 0
                border.color: Theme.borderStrong
            }
            Column {
                id: descriptionColumn
                width: descriptionView.availableWidth
                spacing: 4
                Text {
                    width: parent.width
                    visible: root.subTitle.length > 0
                    text: root.subTitle
                    textFormat: Text.RichText
                    color: Theme.textSecondary
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    running: root.controller.detailsBusy
                    visible: running
                    width: 24
                    height: 24
                }
                Text {
                    width: parent.width
                    text: root.controller.selectedKey.length > 0 ? String(root.details.description || qsTr("No description available.")) : ""
                    textFormat: Text.PlainText
                    color: Theme.textSecondary
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                Text {
                    width: parent.width
                    text: String(root.details.reason || "")
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    color: Theme.textMuted
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
            }
        }
        Flow {
            id: actions
            width: parent.width
            spacing: 6
            Action {
                id: primaryButton
                objectName: "ui.epgSearch.primary"
                text: String(root.details.primaryLabel || qsTr("Show details"))
                iconSource: root.details.actionKind === "live" || root.details.actionKind === "catchup"
                    ? "qrc:/resources/icons/play.svg" : "qrc:/resources/icons/movie-info.svg"
                enabled: !root.inputBlocked && root.controller.resultsCurrent && Boolean(root.details.primaryEnabled)
                onClicked: root.controller.activateSelected()
            }
            Action {
                id: beginningButton
                objectName: "ui.epgSearch.beginning"
                text: qsTr("Play from beginning")
                iconSource: "qrc:/resources/icons/start-from-beginning.svg"
                visible: Boolean(root.details.fromBeginningEnabled)
                enabled: !root.inputBlocked && root.controller.resultsCurrent
                onClicked: root.controller.playSelectedFromBeginning()
            }
            Action {
                id: recordingButton
                objectName: "ui.epgSearch.recording"
                text: String(root.details.recordingLabel || qsTr("Schedule recording"))
                iconSource: "qrc:/resources/icons/dvr.svg"
                visible: Boolean(root.details.recordingVisible)
                enabled: !root.inputBlocked && root.controller.resultsCurrent && Boolean(root.details.recordingEnabled)
                onClicked: root.controller.toggleSelectedRecording()
            }
            Action {
                id: downloadButton
                objectName: "ui.epgSearch.download"
                text: qsTr("Download")
                iconSource: "qrc:/resources/icons/download.svg"
                visible: Boolean(root.details.downloadEnabled)
                enabled: !root.inputBlocked && root.controller.resultsCurrent
                onClicked: root.controller.downloadSelected()
            }
        }
    }
}
