pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import "../theme/Theme.js" as Theme

ScrollBar {
    id: control
    policy: size < 1 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
    contentItem: Rectangle {
        implicitWidth: 6
        implicitHeight: 6
        radius: width / 2
        color: Theme.vodScrollBarColor(control.pressed, control.hovered)
    }
}
