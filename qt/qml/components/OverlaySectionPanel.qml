import QtQuick
import QtQuick.Layouts
import "../theme/Theme.js" as Theme

Rectangle {
    id: control

    property color panelColor: "#86101924"
    property int horizontalPadding: Theme.spacingL
    property int verticalPadding: Theme.spacingL
    property int panelSpacing: Theme.spacingM
    default property alias contentData: content.data

    color: panelColor
    radius: Theme.radiusM
    border.width: 0
    implicitHeight: content.implicitHeight + verticalPadding * 2

    ColumnLayout {
        id: content

        x: control.horizontalPadding
        y: control.verticalPadding
        width: Math.max(0, control.width - control.horizontalPadding * 2)
        spacing: control.panelSpacing
    }
}
