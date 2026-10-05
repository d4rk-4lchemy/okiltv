pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import "../theme/Theme.js" as Theme

Button {
    id: control
    property int uiTransparency: 100
    property bool marked: false
    property bool favouriteAction: false
    property bool posterMode: false
    property real sizeScale: 1
    property bool retainEnabledAppearance: false
    readonly property url iconSource: favouriteAction ? "qrc:/resources/icons/favourites.svg" : "qrc:/resources/icons/bookmark.svg"
    readonly property color activeIconColor: Theme.accent
    readonly property string caption: favouriteAction
        ? (marked ? "Remove from favourites" : "Add to favourites")
        : (marked ? "Remove from plan to watch" : "Add to plan to watch")
    implicitWidth: 42 * sizeScale
    implicitHeight: 42 * sizeScale
    padding: 9 * sizeScale
    hoverEnabled: true
    Accessible.name: caption
    ToolTip.visible: hovered
    ToolTip.delay: 400
    ToolTip.text: caption
    contentItem: Image {
        source: control.iconSource
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        opacity: control.enabled || control.retainEnabledAppearance ? 1 : 0.36
        layer.enabled: control.marked
        layer.effect: MultiEffect {
            colorization: 1
            colorizationColor: control.activeIconColor
        }
    }
    background: Rectangle {
        radius: Theme.radiusS * control.sizeScale
        color: Theme.uiBackground(control.down || control.hovered ? Theme.overlaySurfaceInteractive
            : control.posterMode ? Theme.overlaySurface : Theme.overlaySurfaceRaised,
            control.uiTransparency, control.posterMode ? 0.8 : 1)
        border.width: control.visualFocus || !control.posterMode ? 1 : 0
        border.color: control.visualFocus ? Theme.overlayTextSecondary : Theme.overlayBorder
    }
}
