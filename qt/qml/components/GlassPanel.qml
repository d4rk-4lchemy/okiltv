import QtQuick
import "../theme/Theme.js" as Theme

Rectangle {
    id: control

    property color fillColor: Theme.glass
    property color strokeColor: Theme.border
    property int radiusSize: Theme.radiusM

    color: fillColor
    radius: radiusSize
    border.width: 1
    border.color: strokeColor
}
