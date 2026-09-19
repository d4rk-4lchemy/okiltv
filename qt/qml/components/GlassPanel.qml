import QtQuick
import "../theme/Theme.js" as Theme

Rectangle {
    id: control

    // qmllint disable unqualified
    property int uiTransparency: (typeof settingsController !== "undefined") ? settingsController.uiTransparency : 100
    // qmllint enable unqualified

    property color fillColor: Theme.glass
    property real fillOpacity: 1
    property color strokeColor: Theme.border
    property int radiusSize: Theme.radiusM

    color: Theme.uiBackground(control.fillColor, control.uiTransparency, control.fillOpacity)
    radius: radiusSize
    border.width: 1
    border.color: strokeColor
}
