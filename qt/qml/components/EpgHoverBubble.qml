import QtQuick
import QtQuick.Layouts
import "../theme/Theme.js" as Theme

Item {
    id: control

    property string timePattern: "HH:mm"
    property var programData: ({})
    property string noticeText: ""
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
    readonly property string timeRangeText: {
        const start = new Date(textValue("start"))
        const stop = new Date(textValue("stop"))
        if (!Number.isFinite(start.getTime()) || !Number.isFinite(stop.getTime()))
            return textValue("timeRange").trim()
        return Qt.locale("en_US").toString(start, control.timePattern)
            + " - " + Qt.locale("en_US").toString(stop, control.timePattern)
    }
    readonly property string episodeText: textValue("episodeNum").trim()
    readonly property string descriptionText: {
        if (programData.detailsPending) return "Loading programme details…"
        if (programData.detailsError) return programData.detailsError
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
            visible: control.noticeText.length > 0
            text: control.noticeText
            color: Theme.textSecondary
            font.pixelSize: 11
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
