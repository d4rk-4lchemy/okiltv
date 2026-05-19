import QtQuick
import "../theme/Theme.js" as Theme

Rectangle {
    id: badge

    property string sourcePath: ""
    property string imageSource: ""
    property string label: ""
    property int badgeSize: 42
    property bool showFrame: true
    property int imageMargin: 4

    width: badgeSize
    height: badgeSize
    radius: showFrame ? Theme.radiusS : 0
    color: showFrame ? Theme.surfaceMuted : "transparent"
    border.width: showFrame ? 1 : 0
    border.color: showFrame ? Theme.border : "transparent"
    clip: showFrame

    Image {
        id: logoImage
        anchors.fill: parent
        anchors.margins: badge.imageMargin
        source: badge.imageSource.length > 0
            ? badge.imageSource
            : (badge.sourcePath ? "file:///" + badge.sourcePath : "")
        asynchronous: true
        fillMode: Image.PreserveAspectFit
        sourceSize.width: Math.max(1, Math.round((badge.badgeSize - badge.imageMargin * 2) * Screen.devicePixelRatio))
        sourceSize.height: Math.max(1, Math.round((badge.badgeSize - badge.imageMargin * 2) * Screen.devicePixelRatio))
        visible: source !== ""
    }

    Text {
        anchors.centerIn: parent
        visible: !logoImage.visible
        text: badge.label.length > 0 ? badge.label.slice(0, 2).toUpperCase() : "TV"
        color: Theme.textSecondary
        font.pixelSize: 12
        font.bold: true
    }
}
