pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Window
import "../theme/Theme.js" as Theme

Item {
    id: root
    objectName: "ui.epgSearch.result." + resultKey
    required property string resultKey
    required property string title
    required property string subTitle
    required property string episodeNum
    required property string channelName
    required property string channelLogo
    required property string timeLabel
    required property string statusLabel
    required property string broadcastState
    required property var titleHighlights
    required property var subTitleHighlights
    property bool selected: false
    property bool actionsCurrent: true
    property bool compact: false
    signal chosen()
    signal activated()
    required property var controller
    property int uiTransparency: 100
    property bool showDetails: false
    readonly property real headerHeight: compact ? 56 : 64
    readonly property real informationInset: channelLogo.length > 0 ? (compact ? 54 : 64) : 12
    readonly property var detailsPane: detailsLoader.item
    property bool pointerHovered: false
    property bool marqueeIndicated: selected || hovered
    property bool inViewport: true
    readonly property string marqueeKey: resultKey
    readonly property bool hovered: pointerHovered || header.hovered
    implicitHeight: headerHeight + (detailsLoader.active ? detailsLoader.height + 8 : 0)
    height: implicitHeight
    signal tabRequested(bool backwards)
    signal controlFocused(var item)

    // Core supplies offsets mapped back to the original UTF-16 text. Escape all
    // provider text before inserting our own, controlled emphasis markup.
    function escaped(value) {
        return String(value).replace(/&/g, "&amp;").replace(/</g, "&lt;")
            .replace(/>/g, "&gt;").replace(/\"/g, "&quot;").replace(/'/g, "&#39;")
    }
    function emphasized(value, ranges) {
        let result = ""
        let offset = 0
        for (let i = 0; i < ranges.length; ++i) {
            const start = Math.max(offset, Math.min(value.length, Number(ranges[i].start)))
            const end = Math.min(value.length, start + Math.max(0, Number(ranges[i].length)))
            if (!Number.isFinite(start) || !Number.isFinite(end)) continue
            result += escaped(value.slice(offset, start)) + "<b>" + escaped(value.slice(start, end)) + "</b>"
            offset = end
        }
        return result + escaped(value.slice(offset))
    }
    Rectangle {
        anchors.fill: parent
        radius: 4
        color: Theme.uiBackground(root.selected ? Theme.liveRailSelection
            : root.hovered ? Theme.liveRailHover : "transparent", root.uiTransparency)
        Rectangle {
            objectName: root.objectName + ".indicator"
            width: 3
            height: root.headerHeight - 24
            y: 12
            radius: 1
            color: root.broadcastState === "upcoming" ? Theme.accent
                : root.broadcastState === "now" ? Theme.success : Theme.epgSearchPast
            visible: root.broadcastState === "upcoming" || root.broadcastState === "now" || root.broadcastState === "past"
        }
    }
    ItemDelegate {
        id: header
        width: parent.width
        height: root.headerHeight
        padding: 12
        hoverEnabled: true
        activeFocusOnTab: false
        Accessible.name: root.title + ", " + root.channelName + ", " + root.timeLabel + ", " + root.statusLabel
        onClicked: root.chosen()
        background: Item {}
        contentItem: Item {
            Image {
                id: logo
                objectName: root.objectName + ".logo"
                width: root.compact ? 32 : 40
                height: width
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                source: root.channelLogo
                asynchronous: true
                fillMode: Image.PreserveAspectFit
                sourceSize.width: Math.round(width * Screen.devicePixelRatio)
                sourceSize.height: Math.round(height * Screen.devicePixelRatio)
                visible: status === Image.Ready
            }
            Column {
                anchors.left: parent.left
                anchors.leftMargin: root.informationInset - header.padding
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4
                VodMarqueeTitle {
                    objectName: root.objectName + ".title"
                    width: parent.width
                    height: implicitHeight
                    text: root.emphasized(root.title, root.titleHighlights)
                    textFormat: Text.RichText
                    contentIdentity: root.resultKey
                    indicated: root.marqueeIndicated && root.actionsCurrent && root.inViewport
                    color: Theme.textPrimary
                    font.pixelSize: root.compact ? 16 : 18
                }
                Item {
                    width: parent.width
                    height: metadata.implicitHeight
                    Text {
                        id: metadata
                        anchors.left: parent.left
                        anchors.right: airingStatus.left
                        anchors.rightMargin: airingStatus.visible ? 12 : 0
                        text: root.channelName + " · " + root.timeLabel
                        textFormat: Text.PlainText
                        color: Theme.textSecondary
                        font.pixelSize: root.compact ? 11 : 12
                        elide: Text.ElideRight
                    }
                    Text {
                        id: airingStatus
                        objectName: root.objectName + ".status"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: visible ? Math.min(Math.ceil(airingStatusMetrics.width) + 2, parent.width * 0.32) : 0
                        visible: !root.compact
                        text: root.statusLabel
                        textFormat: Text.PlainText
                        color: Theme.textMuted
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                    TextMetrics {
                        id: airingStatusMetrics
                        text: root.statusLabel
                        font: airingStatus.font
                    }
                }
            }
        }
    }
    Loader {
        id: detailsLoader
        x: root.informationInset
        y: root.headerHeight
        width: parent.width - x - 12
        active: root.showDetails
        sourceComponent: EpgSearchDetailsPane {
            controller: root.controller
            inputBlocked: !root.actionsCurrent
            uiTransparency: root.uiTransparency
            compact: root.compact
            subTitle: root.emphasized(root.subTitle, root.subTitleHighlights)
                + (root.episodeNum.length > 0 ? " · " + root.escaped(root.episodeNum) : "")
            onTabRequested: backwards => root.tabRequested(backwards)
            onControlFocused: item => root.controlFocused(item)
        }
        // Release keyboard ownership when the selected delegate is recycled.
        onActiveChanged: if (!active && root.activeFocus) root.focus = false
    }
}
