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
    property int barHeight: 33
    property int edgeThickness: 8
    property int topResizeThickness: 3
    property int buttonSize: 30
    property color barFillColor: "#82070d12"

    implicitHeight: barHeight
    height: barHeight
    visible: root.window !== undefined && root.window !== null && root.window.visibility !== Window.FullScreen && (root.targetVisible || root.opacity > 0.01)
    enabled: root.window !== undefined && root.window !== null && root.window.visibility !== Window.FullScreen && root.opacity > 0.01
    opacity: root.window !== undefined && root.window !== null && root.window.visibility !== Window.FullScreen && root.targetVisible ? 1.0 : 0.0
    readonly property int occupiedHeight: root.visible ? root.barHeight : 0

    readonly property bool canResize: root.window !== undefined && root.window !== null && root.window.visibility !== Window.FullScreen && root.window.visibility !== Window.Maximized
    readonly property bool useManualWindowDrag: Qt.platform.os === "windows"
    property bool manualDragActive: false
    property bool manualDragThresholdPassed: false
    property real manualDragPressGlobalX: 0
    property real manualDragPressGlobalY: 0
    property real manualDragStartWindowX: 0
    property real manualDragStartWindowY: 0
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

    function beginManualWindowDrag(mouse) {
        if (!root.window) {
            return;
        }
        const globalPoint = dragArea.mapToGlobal(mouse.x, mouse.y);
        root.manualDragActive = true;
        root.manualDragThresholdPassed = false;
        root.manualDragPressGlobalX = globalPoint.x;
        root.manualDragPressGlobalY = globalPoint.y;
        root.manualDragStartWindowX = root.window.x;
        root.manualDragStartWindowY = root.window.y;
    }

    function updateManualWindowDrag(mouse) {
        if (!root.manualDragActive || !root.window) {
            return;
        }

        const globalPoint = dragArea.mapToGlobal(mouse.x, mouse.y);
        const deltaX = globalPoint.x - root.manualDragPressGlobalX;
        const deltaY = globalPoint.y - root.manualDragPressGlobalY;
        const threshold = Math.max(1, Application.styleHints.startDragDistance);

        if (!root.manualDragThresholdPassed) {
            if (Math.abs(deltaX) < threshold && Math.abs(deltaY) < threshold) {
                return;
            }
            root.manualDragThresholdPassed = true;
        }

        root.window.x = Math.round(root.manualDragStartWindowX + deltaX);
        root.window.y = Math.round(root.manualDragStartWindowY + deltaY);
    }

    function endManualWindowDrag() {
        root.manualDragActive = false;
        root.manualDragThresholdPassed = false;
    }

    Behavior on opacity {
        NumberAnimation {
            duration: Theme.transitionMs * 0.8
            easing.type: Easing.OutCubic
        }
    }

    transform: Translate {
        y: root.targetVisible ? 0 : -(root.height + Theme.spacingS)

        Behavior on y {
            NumberAnimation {
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
                if (root.useManualWindowDrag && root.window.visibility === Window.Windowed) {
                    root.beginManualWindowDrag(mouse);
                } else if (root.window.startSystemMove) {
                    root.window.startSystemMove();
                }
            }
            onPositionChanged: function (mouse) {
                root.updateManualWindowDrag(mouse);
            }
            onReleased: root.endManualWindowDrag()
            onCanceled: root.endManualWindowDrag()
            onDoubleClicked: function (mouse) {
                if (mouse.button !== Qt.LeftButton || !root.window) {
                    return;
                }
                root.endManualWindowDrag();
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
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 5
            anchors.rightMargin: 0
            height: root.barHeight
            spacing: Theme.spacingS

            Image {
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
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
                font.pixelSize: 13
                font.bold: true
                elide: Text.ElideRight
                renderType: Text.NativeRendering
            }

            Item {
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.alignment: Qt.AlignVCenter
                spacing: 1

                IconActionButton {
                    id: minimizeButton
                    compact: true
                    borderless: true
                    barMode: true
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
                    compact: true
                    borderless: true
                    barMode: true
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
                    compact: true
                    borderless: true
                    barMode: true
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
}
