import QtQuick
import QtQuick.Window

Item {
    id: root

    property var window
    property var livePage

    readonly property bool canResize: root.window !== undefined && root.window !== null && root.window.visibility !== Window.FullScreen && root.window.visibility !== Window.Maximized
    readonly property bool interactionActive: canResize && (topLeftResizeMouseArea.containsMouse || topLeftResizeMouseArea.pressed || topEdgeResizeMouseArea.containsMouse || topEdgeResizeMouseArea.pressed || topRightResizeMouseArea.containsMouse || topRightResizeMouseArea.pressed || leftEdgeResizeMouseArea.containsMouse || leftEdgeResizeMouseArea.pressed || rightEdgeResizeMouseArea.containsMouse || rightEdgeResizeMouseArea.pressed || bottomLeftResizeMouseArea.containsMouse || bottomLeftResizeMouseArea.pressed || bottomEdgeResizeMouseArea.containsMouse || bottomEdgeResizeMouseArea.pressed || bottomRightResizeMouseArea.containsMouse || bottomRightResizeMouseArea.pressed)

    implicitWidth: 0
    implicitHeight: 0
    visible: root.canResize
    enabled: root.canResize

    function revealChrome() {
        if (root.livePage && root.livePage.revealUi) {
            root.livePage.revealUi("pointer");
        }
    }

    function beginResize(edges) {
        if (!root.canResize || root.window === undefined || root.window === null) {
            return;
        }
        root.revealChrome();
        if (root.window.startSystemResize) {
            root.window.startSystemResize(edges);
        }
    }

    Item {
        id: topLeftResizeArea
        visible: root.canResize
        enabled: root.canResize
        x: 0
        y: 0
        width: 10
        height: 10
        z: 5

        MouseArea {
            id: topLeftResizeMouseArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeFDiagCursor
            onEntered: root.revealChrome()
            onPressed: root.beginResize(Qt.TopEdge | Qt.LeftEdge)
        }
    }

    Item {
        id: topEdgeResizeArea
        visible: root.canResize
        enabled: root.canResize
        x: 10
        y: 0
        width: Math.max(0, root.width - 20)
        height: 6
        z: 5

        MouseArea {
            id: topEdgeResizeMouseArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeVerCursor
            onEntered: root.revealChrome()
            onPressed: root.beginResize(Qt.TopEdge)
        }
    }

    Item {
        id: topRightResizeArea
        visible: root.canResize
        enabled: root.canResize
        x: Math.max(0, root.width - 10)
        y: 0
        width: 10
        height: 10
        z: 5

        MouseArea {
            id: topRightResizeMouseArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeBDiagCursor
            onEntered: root.revealChrome()
            onPressed: root.beginResize(Qt.TopEdge | Qt.RightEdge)
        }
    }

    Item {
        id: leftEdgeResizeArea
        visible: root.canResize
        enabled: root.canResize
        x: 0
        y: 10
        width: 8
        height: Math.max(0, root.height - 18)
        z: 5

        MouseArea {
            id: leftEdgeResizeMouseArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeHorCursor
            onEntered: root.revealChrome()
            onPressed: root.beginResize(Qt.LeftEdge)
        }
    }

    Item {
        id: rightEdgeResizeArea
        visible: root.canResize
        enabled: root.canResize
        x: Math.max(0, root.width - 8)
        y: 10
        width: 8
        height: Math.max(0, root.height - 18)
        z: 5

        MouseArea {
            id: rightEdgeResizeMouseArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeHorCursor
            onEntered: root.revealChrome()
            onPressed: root.beginResize(Qt.RightEdge)
        }
    }

    Item {
        id: bottomLeftResizeArea
        visible: root.canResize
        enabled: root.canResize
        x: 0
        y: Math.max(0, root.height - 10)
        width: 10
        height: 10
        z: 5

        MouseArea {
            id: bottomLeftResizeMouseArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeBDiagCursor
            onEntered: root.revealChrome()
            onPressed: root.beginResize(Qt.BottomEdge | Qt.LeftEdge)
        }
    }

    Item {
        id: bottomEdgeResizeArea
        visible: root.canResize
        enabled: root.canResize
        x: 10
        y: Math.max(0, root.height - 8)
        width: Math.max(0, root.width - 20)
        height: 8
        z: 5

        MouseArea {
            id: bottomEdgeResizeMouseArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeVerCursor
            onEntered: root.revealChrome()
            onPressed: root.beginResize(Qt.BottomEdge)
        }
    }

    Item {
        id: bottomRightResizeArea
        visible: root.canResize
        enabled: root.canResize
        x: Math.max(0, root.width - 10)
        y: Math.max(0, root.height - 10)
        width: 10
        height: 10
        z: 5

        MouseArea {
            id: bottomRightResizeMouseArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeFDiagCursor
            onEntered: root.revealChrome()
            onPressed: root.beginResize(Qt.BottomEdge | Qt.RightEdge)
        }
    }
}
