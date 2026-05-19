import QtQuick
import QtQuick.Layouts
import "../theme/Theme.js" as Theme

Item {
    id: control

    property var programData: ({})
    property bool hovered: bubbleHover.hovered
    property real maxWidth: 360

    function textValue(key) {
        if (!programData || programData[key] === undefined || programData[key] === null) {
            return ""
        }
        return String(programData[key])
    }

    readonly property string titleText: {
        const value = textValue("title").trim()
        return value.length > 0 ? value : "Programme details"
    }
    readonly property string subTitleText: textValue("subTitle").trim()
    readonly property string timeRangeText: textValue("timeRange").trim()
    readonly property string episodeText: textValue("episodeNum").trim()
    readonly property string descriptionText: {
        const value = textValue("description").trim()
        return value.length > 0 ? value : "No programme description available."
    }

    width: Math.min(maxWidth, Math.max(220, contentColumn.implicitWidth + 24))
    height: contentColumn.implicitHeight + 24

    HoverHandler {
        id: bubbleHover
        target: control
        acceptedDevices: PointerDevice.Mouse
    }

    GlassPanel {
        anchors.fill: parent
        fillColor: Theme.glassStrong
        strokeColor: "transparent"
        radiusSize: 8
    }

    ColumnLayout {
        id: contentColumn
        anchors.fill: parent
        anchors.margins: 12
        spacing: 6

        Text {
            Layout.fillWidth: true
            text: control.titleText
            color: Theme.textPrimary
            font.pixelSize: 15
            font.bold: true
            wrapMode: Text.Wrap
        }

        Text {
            Layout.fillWidth: true
            visible: control.timeRangeText.length > 0
            text: control.timeRangeText
            color: Theme.textSecondary
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }

        Text {
            Layout.fillWidth: true
            visible: control.subTitleText.length > 0
            text: control.subTitleText
            color: Theme.textSecondary
            font.pixelSize: 12
            font.italic: true
            wrapMode: Text.Wrap
        }

        Text {
            Layout.fillWidth: true
            visible: control.episodeText.length > 0
            text: "Episode: " + control.episodeText
            color: Theme.textSecondary
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }

        Text {
            Layout.fillWidth: true
            text: control.descriptionText
            color: Theme.textPrimary
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
    }
}
