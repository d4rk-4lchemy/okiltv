pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import "../theme/Theme.js" as Theme

Item {
    id: root
    required property var controller
    required property var app
    required property var mainWindow
    property string dateTimePattern: "dd-MM-yyyy HH:mm"
    property Item buttonHost: null
    property Item transportBar: null
    property real topInset: 0
    property int uiTransparency: 100
    property bool shellChromeVisible: true
    onShellChromeVisibleChanged: {
        if (!root.shellChromeVisible) {
            if (cancelDialog.visible)
                cancelDialog.reject()
            queuePanel.close()
        }
    }
    readonly property bool indicatorVisible: root.controller.hasPending || root.controller.unread || queuePanel.visible
    property bool choosingFile: false
    readonly property bool interactionActive: choosingFile || queuePanel.visible || cancelDialog.visible
    property var pendingChannel: ({})
    property var pendingProgram: ({})
    property string notice: ""

    function positionPanel() {
        const overlay = root.Overlay.overlay
        if (!queuePanel.parent || !overlay)
            return
        const anchorItem = root.transportBar || indicator
        const anchor = anchorItem.mapToItem(overlay, 0, 0)
        const topLimit = root.mapToItem(overlay, 0, root.topInset + 8).y
        // Shrink to the space above the entire transport, including its timeline.
        queuePanel.height = Math.min(480, Math.max(0, anchor.y - 10 - topLimit))
        const left = Math.max(8, Math.min(anchor.x, overlay.width - queuePanel.width - 8))
        const position = queuePanel.parent.mapFromItem(overlay, left, anchor.y - queuePanel.height - 10)
        queuePanel.x = position.x
        queuePanel.y = position.y
    }

    onWidthChanged: Qt.callLater(root.positionPanel)
    onHeightChanged: Qt.callLater(root.positionPanel)

    function showNotice(message) {
        root.notice = message
        noticeTimer.restart()
    }

    function requestDownload(channel, program) {
        if (root.interactionActive || root.controller.shuttingDown)
            return
        if (!channel || !program || !program.start) {
            root.showNotice("Select a programme in Guide or the EPG panel first.")
            return
        }
        const state = root.app.catchupDownloadActionState(channel, program)
        if (!state.enabled) {
            root.showNotice(state.reason || "This programme cannot be downloaded.")
            return
        }
        // Keep the exact target while native dialogs run a nested event loop.
        root.pendingChannel = Object.assign({}, channel)
        root.pendingProgram = Object.assign({}, program)
        const destination = root.controller.suggestedDestination(
            String(program.title || "").trim(), String(channel.name || ""), new Date(program.start))
        root.choosingFile = true
        root.controller.chooseDestination(root.mainWindow, destination)
    }

    function handleEscape() {
        if (cancelDialog.visible) {
            cancelDialog.reject()
            return true
        }
        if (root.choosingFile) {
            root.controller.cancelDestination()
            root.choosingFile = false
            return true
        }
        if (queuePanel.visible) {
            queuePanel.close()
            return true
        }
        return false
    }

    component FlatAction: AppButton {
        id: action
        compact: true
        borderless: true
        font.pixelSize: 13
        background: Rectangle {
            radius: 4
            color: action.down || action.hovered
                ? Theme.uiBackground("#6d111a24", root.uiTransparency) : "transparent"
        }
    }

    component CloseAction: IconActionButton {
        implicitWidth: 28
        implicitHeight: 28
        padding: 0
        compact: true
        borderless: true
        barMode: true
        iconInset: 2
        iconSource: "qrc:/resources/icons/close.svg"
        uiTransparency: root.uiTransparency
        Accessible.name: caption
    }

    Dialog {
        id: cancelDialog
        objectName: "ui.downloads.cancelDialog"
        property string jobId: ""
        property string programmeTitle: ""
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        focus: true
        title: "Cancel download?"
        standardButtons: Dialog.Yes | Dialog.No
        implicitWidth: Math.min(440, root.width - 32)
        implicitHeight: cancelHeading.implicitHeight + cancelMessage.implicitHeight
            + cancelButtons.implicitHeight + topPadding + bottomPadding + 2 * spacing
        contentHeight: cancelMessage.implicitHeight
        padding: 20
        spacing: 16
        closePolicy: Popup.CloseOnEscape
        background: Rectangle {
            radius: 8
            color: Theme.uiBackground("#e6070d12", root.uiTransparency)
        }
        header: Text {
            id: cancelHeading
            text: cancelDialog.title
            color: Theme.textPrimary
            font.pixelSize: 16
            font.bold: true
            leftPadding: cancelDialog.leftPadding
            rightPadding: cancelDialog.rightPadding
            topPadding: cancelDialog.topPadding
            wrapMode: Text.Wrap
        }
        contentItem: Text {
            id: cancelMessage
            textFormat: Text.PlainText
            text: "Cancel downloading “" + cancelDialog.programmeTitle
                + "”? The unfinished file will be deleted."
            color: Theme.textPrimary
            font.pixelSize: 14
            wrapMode: Text.Wrap
        }
        footer: DialogButtonBox {
            id: cancelButtons
            implicitHeight: 34 + topPadding + bottomPadding
            standardButtons: cancelDialog.standardButtons
            alignment: Qt.AlignRight
            leftPadding: cancelDialog.leftPadding
            rightPadding: cancelDialog.rightPadding
            bottomPadding: cancelDialog.bottomPadding
            background: Item {}
            delegate: FlatAction {}
        }
        onAccepted: root.controller.cancel(cancelDialog.jobId)
        onClosed: {
            cancelDialog.jobId = ""
            cancelDialog.programmeTitle = ""
        }
    }

    IconActionButton {
        id: indicator
        parent: root.buttonHost || root
        objectName: "ui.downloads.indicator"
        visible: root.indicatorVisible
        width: root.buttonHost ? root.buttonHost.width : 42
        height: root.buttonHost ? root.buttonHost.height : 42
        x: root.buttonHost ? 0 : (root.width - width) / 2
        y: root.buttonHost ? 0 : root.height - height - Theme.spacingM
        compact: true
        borderless: true
        barMode: true
        iconInset: 1
        iconSource: "qrc:/resources/icons/download.svg"
        uiTransparency: root.uiTransparency
        caption: root.controller.hasPending
            ? "Downloads" + (root.controller.paused ? " · Paused" : "")
                + (root.controller.activeProgressKnown ? " · " + Math.floor(root.controller.activeProgress) + "%" : "")
                + (root.controller.queuedCount > 0 ? " · " + root.controller.queuedCount + " queued" : "")
            : "Downloads"
        Accessible.name: "Downloads"
        readonly property bool strongPulse: root.controller.unread && !queuePanel.visible
        readonly property bool pulsing: !queuePanel.visible && ((root.controller.hasPending && !root.controller.paused) || strongPulse)
        SequentialAnimation on opacity {
            running: indicator.pulsing && indicator.visible
            loops: Animation.Infinite
            NumberAnimation {
                to: indicator.strongPulse ? 0.25 : 0.7
                duration: indicator.strongPulse ? 420 : 950
                easing.type: Easing.InOutSine
            }
            NumberAnimation {
                to: 1
                duration: indicator.strongPulse ? 420 : 950
                easing.type: Easing.InOutSine
            }
            onStopped: indicator.opacity = 1
        }
        onClicked: queuePanel.visible ? queuePanel.close() : queuePanel.open()
    }

    Popup {
        id: queuePanel
        objectName: "ui.downloads.panel"
        // Exclude the toggle button from outside-press dismissal so its click
        // sees the open panel and closes it exactly once.
        parent: indicator
        onAboutToShow: root.positionPanel()
        onWidthChanged: Qt.callLater(root.positionPanel)
        onHeightChanged: Qt.callLater(root.positionPanel)
        width: Math.min(440, root.width - 16)
        height: 480
        padding: 12
        modal: false
        dim: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        onOpened: {
            root.controller.markRead()
            Qt.callLater(root.positionPanel)
        }
        background: GlassPanel {
            uiTransparency: root.uiTransparency
            fillColor: "#82070d12"
            strokeColor: "transparent"
            border.width: 0
            radiusSize: 8
        }
        contentItem: ColumnLayout {
            spacing: 8
            RowLayout {
                Layout.fillWidth: true
                IconActionButton {
                    objectName: "ui.downloads.pause"
                    implicitWidth: 28
                    implicitHeight: 28
                    padding: 0
                    compact: true
                    borderless: true
                    barMode: true
                    iconInset: 4
                    visible: root.controller.hasPending
                    enabled: !root.controller.shuttingDown
                    iconSource: root.controller.paused
                        ? "qrc:/resources/icons/play.svg" : "qrc:/resources/icons/pause.svg"
                    caption: root.controller.paused ? "Resume downloads" : "Pause downloads"
                    Accessible.name: caption
                    uiTransparency: root.uiTransparency
                    onClicked: root.controller.paused ? root.controller.resumeAll() : root.controller.pauseAll()
                }
                Item { Layout.fillWidth: true }
                CloseAction {
                    objectName: "ui.downloads.close"
                    caption: "Close"
                    onClicked: queuePanel.close()
                }
            }
            ListView {
                id: tasks
                objectName: "ui.downloads.list"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 8
                model: root.controller
                Label {
                    anchors.centerIn: parent
                    visible: tasks.count === 0
                    text: "No downloads yet"
                    color: Theme.textSecondary
                }
                ScrollBar.vertical: ScrollBar {}
                delegate: ColumnLayout {
                    id: task
                    required property string jobId
                    required property string title
                    required property var programStart
                    required property string channelName
                    required property string destination
                    required property string jobState
                    required property bool progressKnown
                    required property real progress
                    required property real bytes
                    required property string errorText
                    readonly property bool pending: ["queued", "downloading", "resuming", "paused", "finalizing", "verifying", "stopping"].indexOf(jobState) >= 0
                    width: tasks.width
                    spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        Label { textFormat: Text.PlainText; text: task.title; color: Theme.textPrimary; font.bold: true; Layout.fillWidth: true; wrapMode: Text.Wrap }
                        CloseAction {
                            objectName: "ui.downloads.restart." + task.jobId
                            visible: task.jobState === "cancelled" || task.jobState === "failed"
                            enabled: !root.controller.shuttingDown
                            iconSource: "qrc:/resources/icons/restart.svg"
                            iconInset: 8.6
                            caption: "Restart"
                            onClicked: root.controller.restart(task.jobId)
                        }
                        CloseAction {
                            objectName: "ui.downloads.action." + task.jobId
                            iconInset: 6.8
                            caption: task.pending ? "Cancel download" : "Dismiss"
                            enabled: task.jobState !== "stopping"
                            onClicked: {
                                if (!task.pending) {
                                    root.controller.dismiss(task.jobId)
                                } else if (task.jobState === "queued") {
                                    root.controller.cancel(task.jobId)
                                } else {
                                    cancelDialog.jobId = task.jobId
                                    cancelDialog.programmeTitle = task.title
                                    cancelDialog.open()
                                }
                            }
                        }
                    }
                    Label {
                        textFormat: Text.PlainText
                        text: task.channelName + " - " + Qt.locale("en_US").toString(new Date(task.programStart), root.dateTimePattern)
                        color: Theme.textSecondary
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    ProgressBar {
                        id: downloadProgress
                        objectName: "ui.downloads.progress." + task.jobId
                        Layout.fillWidth: true
                        Layout.preferredHeight: 6
                        padding: 0
                        from: 0
                        to: 100
                        value: task.progress
                        visible: task.pending
                        indeterminate: task.jobState === "verifying" || task.jobState === "stopping"
                            || task.jobState === "finalizing"
                            || (!task.progressKnown && (task.jobState === "downloading" || task.jobState === "resuming"))
                        background: Rectangle {
                            implicitHeight: 6
                            radius: 3
                            color: "#283542"
                        }
                        contentItem: Item {
                            Rectangle {
                                id: downloadProgressFill
                                objectName: "ui.downloads.fill." + task.jobId
                                width: task.progressKnown ? parent.width * downloadProgress.visualPosition
                                    : (downloadProgress.indeterminate ? parent.width * 0.2 : 0)
                                height: parent.height
                                radius: 3
                                color: Theme.accent
                                SequentialAnimation on opacity {
                                    running: downloadProgress.visible && downloadProgress.indeterminate
                                    loops: Animation.Infinite
                                    NumberAnimation { to: 0.4; duration: 700; easing.type: Easing.InOutSine }
                                    NumberAnimation { to: 1; duration: 700; easing.type: Easing.InOutSine }
                                    onStopped: downloadProgressFill.opacity = 1
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            color: Theme.textSecondary
                            text: (task.jobState === "queued" && root.controller.paused ? "queued · Paused" : task.jobState)
                                + (task.progressKnown ? " · " + Math.floor(task.progress) + "%" : "")
                                + " · " + (task.bytes / 1048576).toFixed(1) + " MB"
                            elide: Text.ElideRight
                        }
                    }
                    Label { textFormat: Text.PlainText; text: task.destination; color: Theme.textSecondary; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                    Label { textFormat: Text.PlainText; text: task.errorText; visible: text.length > 0; color: Theme.textPrimary; Layout.fillWidth: true; wrapMode: Text.Wrap }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.textSecondary; opacity: 0.2 }
                }
            }
        }
    }

    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: root.topInset
        width: Math.min(440, root.width)
        height: noticeLabel.implicitHeight + 24
        visible: root.notice.length > 0 && !queuePanel.visible
        color: Theme.uiBackground("#82070d12", root.uiTransparency)
        Label {
            id: noticeLabel
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            textFormat: Text.PlainText
            text: root.notice
            color: Theme.textPrimary
            wrapMode: Text.Wrap
        }
    }
    Timer { id: noticeTimer; interval: 6000; onTriggered: root.notice = "" }
    Connections {
        target: root.transportBar
        function onXChanged() { Qt.callLater(root.positionPanel) }
        function onYChanged() { Qt.callLater(root.positionPanel) }
        function onWidthChanged() { Qt.callLater(root.positionPanel) }
        function onHeightChanged() { Qt.callLater(root.positionPanel) }
    }
    Connections {
        target: root.buttonHost
        function onXChanged() { Qt.callLater(root.positionPanel) }
        function onYChanged() { Qt.callLater(root.positionPanel) }
        function onWidthChanged() { Qt.callLater(root.positionPanel) }
        function onHeightChanged() { Qt.callLater(root.positionPanel) }
    }
    Connections {
        target: root.controller
        function onDestinationChosen(destination) {
            root.choosingFile = false
            const error = root.app.enqueueCatchupDownload(root.pendingChannel, root.pendingProgram, destination)
            root.pendingChannel = ({})
            root.pendingProgram = ({})
            if (error.length > 0)
                root.showNotice(error)
        }
        function onDestinationCancelled() {
            root.choosingFile = false
            root.pendingChannel = ({})
            root.pendingProgram = ({})
        }
        function onSummaryChanged() {
            if (queuePanel.visible && root.controller.unread)
                root.controller.markRead()
        }
        function onNotification(message) {
            root.showNotice(message)
            if (queuePanel.visible)
                root.controller.markRead()
        }
    }
}
