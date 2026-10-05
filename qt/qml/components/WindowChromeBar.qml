import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import OKILTV
import "../theme/Theme.js" as Theme

Item {
    id: root

    property var window
    property var livePage
    property bool targetVisible: false
    property int barHeight: Theme.titleBarHeight
    property int uiTransparency: 100
    property int edgeThickness: 8
    property int topResizeThickness: 3
    property int buttonSize: Theme.titleBarButtonSize
    property bool vodPalette: false
    property color barFillColor: root.vodPalette ? Theme.overlaySidebar : Theme.liveRailBackground

    implicitHeight: barHeight
    height: barHeight
    readonly property bool allowedInWindow: root.window !== undefined && root.window !== null
        && root.window.visibility !== Window.FullScreen
    readonly property bool animating: opacityAnimation.running || slideAnimation.running
    visible: root.allowedInWindow && (root.targetVisible || root.opacity > 0.01)
    enabled: root.allowedInWindow && root.targetVisible && root.opacity > 0.01 && !root.animating
    opacity: root.allowedInWindow && root.targetVisible ? 1.0 : 0.0
    readonly property int occupiedHeight: root.visible ? root.barHeight : 0

    readonly property bool canResize: root.window !== undefined && root.window !== null && root.window.visibility !== Window.FullScreen && root.window.visibility !== Window.Maximized
    readonly property bool interactionActive: enabled && (dragArea.containsMouse || dragArea.pressed || topLeftResizeMouseArea.containsMouse || topLeftResizeMouseArea.pressed || topCenterResizeMouseArea.containsMouse || topCenterResizeMouseArea.pressed || topRightResizeMouseArea.containsMouse || topRightResizeMouseArea.pressed || leftEdgeResizeMouseArea.containsMouse || leftEdgeResizeMouseArea.pressed || rightEdgeResizeMouseArea.containsMouse || rightEdgeResizeMouseArea.pressed || minimizeButton.hovered || maximizeButton.hovered || closeButton.hovered || minimizeButton.down || maximizeButton.down || closeButton.down)

    function revealChrome() {
        if (root.livePage && root.livePage.revealUi) {
            root.livePage.revealUi("pointer");
        }
    }

    function resizeWindow(edges) {
        if (!root.canResize || root.window === undefined || root.window === null) {
            return;
        }
        if (root.livePage && root.livePage.revealUi) {
            root.livePage.revealUi("pointer");
        }
        if (root.window.startSystemResize) {
            root.window.startSystemResize(edges);
        }
    }

    Behavior on opacity {
        NumberAnimation {
            id: opacityAnimation
            duration: Theme.transitionMs * 0.8
            easing.type: Easing.OutCubic
        }
    }

    transform: Translate {
        y: root.targetVisible ? 0 : -(root.height + Theme.spacingS)

        Behavior on y {
            NumberAnimation {
                id: slideAnimation
                duration: Theme.transitionMs
                easing.type: Easing.OutCubic
            }
        }
    }

    onInteractionActiveChanged: {
        if (root.interactionActive) {
            root.revealChrome();
        }
    }

    GlassPanel {
        anchors.fill: parent
        fillColor: root.barFillColor
        uiTransparency: root.uiTransparency
        strokeColor: "transparent"
        radiusSize: 0
    }

    Item {
        id: topLeftResizeArea
        visible: root.canResize
        enabled: root.canResize
        width: root.edgeThickness
        height: root.topResizeThickness
        z: 5

        MouseArea {
            id: topLeftResizeMouseArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeFDiagCursor
            onEntered: root.revealChrome()
            onPressed: root.resizeWindow(Qt.TopEdge | Qt.LeftEdge)
        }
    }

    Item {
        id: topCenterResizeArea
        visible: root.canResize
        enabled: root.canResize
        x: root.edgeThickness
        width: Math.max(0, root.width - root.edgeThickness * 2)
        height: root.topResizeThickness
        z: 5

        MouseArea {
            id: topCenterResizeMouseArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeVerCursor
            onEntered: root.revealChrome()
            onPressed: root.resizeWindow(Qt.TopEdge)
        }
    }

    Item {
        id: topRightResizeArea
        visible: root.canResize
        enabled: root.canResize
        x: Math.max(0, root.width - root.edgeThickness)
        width: root.edgeThickness
        height: root.topResizeThickness
        z: 5

        MouseArea {
            id: topRightResizeMouseArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeBDiagCursor
            onEntered: root.revealChrome()
            onPressed: root.resizeWindow(Qt.TopEdge | Qt.RightEdge)
        }
    }

    Item {
        id: leftEdgeResizeArea
        visible: root.canResize
        enabled: root.canResize
        x: 0
        y: root.topResizeThickness
        width: root.edgeThickness
        height: Math.max(0, root.height - root.topResizeThickness)
        z: 5

        MouseArea {
            id: leftEdgeResizeMouseArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeHorCursor
            onEntered: root.revealChrome()
            onPressed: root.resizeWindow(Qt.LeftEdge)
        }
    }

    Item {
        id: rightEdgeResizeArea
        visible: root.canResize
        enabled: root.canResize
        x: Math.max(0, root.width - root.edgeThickness)
        y: root.topResizeThickness
        width: root.edgeThickness
        height: Math.max(0, root.height - root.topResizeThickness)
        z: 5

        MouseArea {
            id: rightEdgeResizeMouseArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeHorCursor
            onEntered: root.revealChrome()
            onPressed: root.resizeWindow(Qt.RightEdge)
        }
    }

    Item {
        id: contentArea
        anchors.fill: parent
        anchors.leftMargin: 5
        anchors.rightMargin: 0
        z: 1

        MouseArea {
            id: dragArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            preventStealing: true
            cursorShape: Qt.ArrowCursor

            onEntered: root.revealChrome()
            onPressed: function (mouse) {
                if (mouse.button !== Qt.LeftButton) {
                    return;
                }
                root.revealChrome();
                if (!root.window) {
                    return;
                }
                // Let the OS own the drag so Windows can preview and apply edge snapping.
                if (root.window.startSystemMove) {
                    root.window.startSystemMove();
                }
            }
            onDoubleClicked: function (mouse) {
                if (mouse.button !== Qt.LeftButton || !root.window) {
                    return;
                }
                root.revealChrome();
                if (root.window.visibility === Window.Maximized) {
                    root.window.showNormal();
                } else {
                    root.window.showMaximized();
                }
            }
        }

        RowLayout {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 5
            width: Math.max(0, root.width - windowButtons.width - 20)
            height: root.barHeight
            spacing: Theme.spacingS

            Image {
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: Theme.titleBarIconSize
                Layout.preferredHeight: Theme.titleBarIconSize
                source: "qrc:/resources/icons/app.png"
                fillMode: Image.PreserveAspectFit
                smooth: true
                mipmap: true
            }

            Text {
                Layout.alignment: Qt.AlignVCenter
                Layout.fillWidth: true
                text: "OKILTV"
                color: Theme.textPrimary
                font.pixelSize: Theme.titleBarFontSize
                font.bold: true
                elide: Text.ElideRight
                renderType: Text.NativeRendering
            }

        }

        RowLayout {
            id: windowButtons
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 1

            IconActionButton {
                id: minimizeButton
                objectName: "ui.window.minimize"
                compact: true
                borderless: true
                barMode: true
                neutralBar: root.vodPalette
                implicitWidth: root.buttonSize
                implicitHeight: root.buttonSize
                iconName: "windowMinimize"
                iconInset: 3
                caption: "Minimize"
                onClicked: {
                    root.revealChrome();
                    if (root.window) {
                        root.window.showMinimized();
                    }
                }
            }

            IconActionButton {
                id: maximizeButton
                objectName: "ui.window.maximize"
                compact: true
                borderless: true
                barMode: true
                neutralBar: root.vodPalette
                implicitWidth: root.buttonSize
                implicitHeight: root.buttonSize
                iconName: root.window && root.window.visibility === Window.Maximized ? "windowRestore" : "windowMaximize"
                iconInset: 3
                caption: root.window && root.window.visibility === Window.Maximized ? "Restore down" : "Maximize"
                onClicked: {
                    root.revealChrome();
                    if (!root.window) {
                        return;
                    }
                    if (root.window.visibility === Window.Maximized) {
                        root.window.showNormal();
                    } else {
                        root.window.showMaximized();
                    }
                }
            }

            IconActionButton {
                id: closeButton
                objectName: "ui.window.close"
                compact: true
                borderless: true
                barMode: true
                neutralBar: root.vodPalette
                implicitWidth: root.buttonSize
                implicitHeight: root.buttonSize
                iconName: "windowClose"
                iconInset: 3
                iconColor: Theme.danger
                caption: "Close"
                onClicked: {
                    root.revealChrome();
                    if (root.window && root.window.requestAppClose) {
                        root.window.requestAppClose("window");
                    }
                }
            }
        }
    }

}
