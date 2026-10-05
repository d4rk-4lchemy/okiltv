pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: root
    property bool ready: true
    property var keyboardItem: null
    readonly property string keyboardTarget: keyboardItem ? keyboardItem.marqueeKey : ""
    property string pointerTarget: ""
    property string inputSource: "keyboard"
    readonly property string activeTarget: ready && visible && enabled
        ? inputSource === "pointer" ? pointerTarget : keyboardTarget : ""

    function useKeyboard() { inputSource = "keyboard" }
    function hoverTarget(key, hovered) {
        if (hovered) pointerTarget = key
        else if (pointerTarget === key) pointerTarget = ""
    }
    HoverHandler {
        parent: root.parent
        property point lastScenePosition: Qt.point(-1, -1)
        onPointChanged: {
            // Delegate movement during scrolling/resize is not mouse input.
            const position = point.scenePosition
            if (position.x === lastScenePosition.x && position.y === lastScenePosition.y) return
            lastScenePosition = position
            if (root.ready) root.inputSource = "pointer"
        }
    }
}
