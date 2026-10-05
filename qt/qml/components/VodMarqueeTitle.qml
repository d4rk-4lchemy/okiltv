pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "../theme/Theme.js" as Theme

Item {
    id: root
    property alias text: fullTitle.text
    property alias font: fullTitle.font
    property alias color: fullTitle.color
    readonly property real availableWidth: width
    implicitWidth: fullTitle.implicitWidth
    implicitHeight: fullTitle.implicitHeight
    property bool indicated: false
    property string contentIdentity: ""
    property real offset: 0
    property bool scrolling: false
    property bool initialized: false
    readonly property bool overflowing: availableWidth > 0 && fullTitle.implicitWidth > availableWidth
    readonly property bool eligible: indicated && overflowing && visible && enabled
    readonly property real cycleDistance: fullTitle.implicitWidth + Theme.vodMarqueeGap
    clip: true
    Accessible.role: Accessible.StaticText
    Accessible.name: text

    function restart() {
        if (!initialized) return
        delay.stop()
        travel.stop()
        scrolling = false
        offset = 0
        if (eligible) delay.start()
    }
    onEligibleChanged: restart()
    onTextChanged: restart()
    onContentIdentityChanged: restart()
    onAvailableWidthChanged: restart()
    onFontChanged: restart()
    onCycleDistanceChanged: restart()
    Component.onCompleted: { initialized = true; restart() }

    Item {
        anchors.fill: parent
        Text {
            anchors.fill: parent
            visible: !root.scrolling
            text: root.text
            textFormat: Text.PlainText
            font: root.font
            color: root.color
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            Accessible.ignored: true
        }
        Label {
            id: fullTitle
            objectName: root.objectName.length > 0 ? root.objectName + ".marqueeText" : ""
            x: -root.offset
            anchors.verticalCenter: parent.verticalCenter
            visible: root.scrolling
            textFormat: Text.PlainText
            color: Theme.overlayTextPrimary
            Accessible.ignored: true
        }
        Text {
            x: root.cycleDistance - root.offset
            anchors.verticalCenter: parent.verticalCenter
            visible: root.scrolling
            text: root.text
            textFormat: Text.PlainText
            font: root.font
            color: root.color
            Accessible.ignored: true
        }
    }
    Timer {
        id: delay
        interval: Theme.vodMarqueeDelayMs
        onTriggered: {
            if (!root.eligible) return
            root.scrolling = true
            travel.start()
        }
    }
    NumberAnimation {
        id: travel
        target: root
        property: "offset"
        from: 0
        to: root.cycleDistance
        duration: Math.max(1, Math.round(root.cycleDistance / Theme.vodMarqueePixelsPerSecond * 1000))
        loops: Animation.Infinite
        easing.type: Easing.Linear
    }
}
