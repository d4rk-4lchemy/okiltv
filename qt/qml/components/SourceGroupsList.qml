pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../theme/Theme.js" as Theme

Item {
    id: root
    property var rows: []
    property string profileId: ""
    property bool reorderEnabled: true
    property string filterKey: ""
    readonly property bool dragActive: draggedId.length > 0
    readonly property int count: preview.count
    readonly property real contentHeight: list.contentHeight
    property alias contentY: list.contentY
    readonly property real scrollOffset: list.contentY - list.originY
    signal reorderRequested(var orderedIds)
    signal selectionRequested(string groupId, bool selected)

    property string pressedId: ""
    property string draggedId: ""
    property var draggedRow: ({})
    property real pressY: 0
    property real grabY: 0
    property real pointerX: 0
    property real pointerY: 0
    property var initialIds: []
    clip: true
    implicitHeight: Math.min(contentHeight, 340)

    function orderedIds() {
        const ids = []
        for (let i = 0; i < preview.count; ++i)
            ids.push(preview.get(i).groupId)
        return ids
    }

    function rowIndex(groupId) {
        for (let i = 0; i < preview.count; ++i)
            if (preview.get(i).groupId === groupId)
                return i
        return -1
    }

    function setScrollOffset(offset) {
        list.forceLayout()
        list.contentY = list.originY + Math.max(0, Math.min(offset,
                                    Math.max(0, list.contentHeight - list.height)))
    }

    function synchronize() {
        const offset = scrollOffset
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            const existing = rowIndex(row.id)
            const entry = { groupId: row.id, name: row.name,
                channelCount: row.count, selected: row.selected }
            if (existing < 0)
                preview.insert(i, entry)
            else {
                if (existing !== i)
                    preview.move(existing, i, 1)
                preview.set(i, entry)
            }
        }
        if (preview.count > rows.length)
            preview.remove(rows.length, preview.count - rows.length)
        setScrollOffset(offset)
    }

    function cancelDrag() {
        pressedId = ""
        draggedId = ""
        synchronize()
    }

    function updateTarget() {
        if (!dragActive)
            return
        const offset = scrollOffset
        const target = Math.max(0, Math.min(preview.count - 1,
            Math.floor((offset + pointerY - grabY + 27) / 60)))
        const source = rowIndex(draggedId)
        if (source >= 0 && source !== target) {
            preview.move(source, target, 1)
            setScrollOffset(offset)
        }
    }

    onRowsChanged: cancelDrag()
    onProfileIdChanged: { cancelDrag(); setScrollOffset(0) }
    onFilterKeyChanged: cancelDrag()
    onReorderEnabledChanged: { if (!reorderEnabled) cancelDrag() }
    onEnabledChanged: { if (!enabled) cancelDrag() }
    onVisibleChanged: { if (!visible) cancelDrag() }
    Component.onCompleted: synchronize()

    ListModel { id: preview }

    component GroupCard: Rectangle {
        id: card
        property var rowData: ({})
        radius: Theme.radiusM
        color: "#5a0c141b"
        height: 54
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            spacing: Theme.spacingM
            Rectangle {
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                radius: 4
                color: card.rowData.selected ? Theme.accent : "transparent"
                border.width: 1
                border.color: card.rowData.selected ? Theme.accent : "#7fa3b5"
                Text {
                    anchors.centerIn: parent
                    text: card.rowData.selected ? "\u2713" : ""
                    color: Theme.textPrimary
                    font.pixelSize: 11
                    font.bold: true
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    Layout.fillWidth: true
                    text: card.rowData.name || ""
                    color: Theme.textPrimary
                    font.pixelSize: 14
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: (card.rowData.channelCount || 0) + " channels"
                    color: Theme.textSecondary
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }
            Rectangle {
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                radius: 5
                color: "#2affffff"
                border.width: 1
                border.color: "#6ca0b8"
                visible: root.reorderEnabled
                Text {
                    anchors.centerIn: parent
                    text: ":::"
                    color: Theme.textSecondary
                    font.pixelSize: 11
                    font.bold: true
                }
            }
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        spacing: 6
        model: preview
        interactive: !root.reorderEnabled && !root.dragActive
        boundsBehavior: Flickable.StopAtBounds
        moveDisplaced: Transition {
            NumberAnimation { properties: "y"; duration: 120; easing.type: Easing.OutQuad }
        }
        delegate: GroupCard {
            id: cardDelegate
            required property var model
            rowData: model
            width: list.width
            opacity: root.draggedId === model.groupId ? 0 : 1
            MouseArea {
                anchors.fill: parent
                anchors.rightMargin: root.reorderEnabled ? 36 : 0
                onClicked: root.selectionRequested(cardDelegate.model.groupId,
                                                   !cardDelegate.model.selected)
            }
        }
    }

    // This mouse grab belongs to the viewport, never to a recycled row.
    MouseArea {
        id: handle
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        hoverEnabled: true
        cursorShape: root.pressedId.length > 0 ? Qt.ClosedHandCursor
            : root.reorderEnabled && containsMouse && mouseX >= width - 36 && mouseX <= width - 14
                && (root.scrollOffset + mouseY) % 60 >= 16
                && (root.scrollOffset + mouseY) % 60 <= 38
                && root.scrollOffset + mouseY < list.contentHeight ? Qt.OpenHandCursor : Qt.ArrowCursor
        onPressed: function(mouse) {
            const y = root.scrollOffset + mouse.y
            const index = Math.floor(y / 60)
            const withinRow = y - index * 60
            if (!root.reorderEnabled || mouse.x < width - 36 || mouse.x > width - 14
                    || withinRow < 16 || withinRow > 38 || index < 0 || index >= preview.count) {
                mouse.accepted = false
                return
            }
            root.pressedId = preview.get(index).groupId
            root.initialIds = root.orderedIds()
            root.pressY = mouse.y
            root.grabY = withinRow
            root.pointerX = mouse.x
            root.pointerY = mouse.y
            const row = preview.get(index)
            root.draggedRow = { name: row.name, channelCount: row.channelCount, selected: row.selected }
        }
        onPositionChanged: function(mouse) {
            if (!pressed || root.pressedId.length === 0)
                return
            root.pointerX = mouse.x
            root.pointerY = mouse.y
            if (!root.dragActive && Math.abs(mouse.y - root.pressY) >= handle.drag.threshold)
                root.draggedId = root.pressedId
            root.updateTarget()
        }
        onReleased: function(mouse) {
            const commit = root.dragActive && mouse.x >= 0 && mouse.x <= width
                && mouse.y >= 0 && mouse.y <= height
            if (commit) {
                root.pointerY = mouse.y
                root.updateTarget()
            }
            const ids = root.orderedIds()
            const changed = ids.length !== root.initialIds.length
                || ids.some(function(id, index) { return id !== root.initialIds[index] })
            root.pressedId = ""
            root.draggedId = ""
            if (commit && changed)
                root.reorderRequested(ids)
            else
                root.synchronize()
        }
        onCanceled: root.cancelDrag()
    }

    GroupCard {
        width: root.width
        y: root.pointerY - root.grabY
        rowData: root.draggedRow
        visible: root.dragActive
        opacity: 0.9
        border.width: 1
        border.color: "#7fa3b5"
    }

    WheelHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        blocking: true
        onWheel: function(event) {
            const delta = Math.abs(event.pixelDelta.y) > 0 ? event.pixelDelta.y : event.angleDelta.y * 0.5
            const previous = root.scrollOffset
            root.setScrollOffset(previous - delta)
            root.updateTarget()
            event.accepted = root.dragActive || Math.abs(root.scrollOffset - previous) > 0.01
        }
    }

    Timer {
        interval: 16
        repeat: true
        running: root.dragActive
        onTriggered: {
            if (root.pointerX < 0 || root.pointerX > root.width
                    || root.pointerY < 0 || root.pointerY > root.height)
                return
            const direction = root.pointerY < 36 ? -(36 - root.pointerY) / 36
                : root.pointerY > root.height - 36 ? (root.pointerY - root.height + 36) / 36 : 0
            if (direction !== 0) {
                root.setScrollOffset(root.scrollOffset + direction * 360 * interval / 1000)
                root.updateTarget()
            }
        }
    }
}
