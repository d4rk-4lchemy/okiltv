import QtQuick
import QtTest
import "../../qml/components"

TestCase {
    id: testCase
    name: "SourceGroupsList"
    width: 500
    height: 450
    visible: true
    when: windowShown
    property var backendRows: []
    SourceGroupsList {
        id: panel
        width: 400
        height: 340
        rows: testCase.backendRows
        profileId: "source-a"
        onReorderRequested: function(ids) {
            const byId = {}
            for (const row of testCase.backendRows)
                byId[row.id] = row
            testCase.backendRows = ids.map(function(id) { return byId[id] })
        }
        onSelectionRequested: function(id, selected) {
            testCase.backendRows = testCase.backendRows.map(function(row) {
                return { id: row.id, name: row.name, count: row.count,
                    selected: row.id === id ? selected : row.selected }
            })
        }
    }
    SignalSpy { id: reordered; target: panel; signalName: "reorderRequested" }
    SignalSpy { id: selection; target: panel; signalName: "selectionRequested" }

    function init() {
        panel.cancelDrag()
        panel.visible = true
        panel.enabled = true
        panel.reorderEnabled = true
        panel.profileId = "source-a"
        panel.filterKey = ""
        const rows = []
        for (let i = 0; i < 100; ++i)
            rows.push({ id: "group-" + i, name: "Group " + i, count: i + 1, selected: true })
        backendRows = rows
        panel.setScrollOffset(1200)
        compare(panel.count, 100)
        compare(panel.scrollOffset, 1200)
        reordered.clear()
        selection.clear()
    }

    function cleanup() {
        mouseRelease(panel, 375, 170, Qt.LeftButton)
        panel.cancelDrag()
    }

    function test_four_rows_data() {
        return [{ tag: "down", start: 27, end: 267, id: "group-20", index: 24 },
                { tag: "up", start: 267, end: 27, id: "group-24", index: 20 }]
    }
    function test_four_rows(data) {
        mousePress(panel, 375, data.start, Qt.LeftButton)
        mouseMove(panel, 375, (data.start + data.end) / 2)
        verify(panel.dragActive)
        mouseMove(panel, 375, data.end)
        compare(reordered.count, 0)
        compare(panel.orderedIds()[data.index], data.id)
        compare(panel.scrollOffset, 1200)
        mouseRelease(panel, 375, data.end, Qt.LeftButton)
        compare(reordered.count, 1)
        compare(backendRows[data.index].id, data.id)
        compare(panel.scrollOffset, 1200)
        verify(!panel.dragActive)
        compare(selection.count, 0)
    }

    function test_edge_scroll_data() {
        return [{ tag: "down", edge: 338, sign: 1 }, { tag: "up", edge: 2, sign: -1 }]
    }
    function test_edge_scroll(data) {
        mousePress(panel, 375, 147, Qt.LeftButton)
        mouseMove(panel, 375, data.edge)
        verify(panel.dragActive)
        tryVerify(function() { return (panel.scrollOffset - 1200) * data.sign > 400 }, 3500)
        verify(panel.dragActive)
        compare(reordered.count, 0)
        const offset = panel.scrollOffset
        mouseRelease(panel, 375, data.edge, Qt.LeftButton)
        compare(reordered.count, 1)
        compare(panel.scrollOffset, offset)
        verify(!panel.dragActive)
        wait(60)
        compare(panel.scrollOffset, offset)
    }

    function test_click_handle_and_noop() {
        mouseClick(panel, 375, 147)
        compare(reordered.count, 0)
        compare(selection.count, 0)
        mousePress(panel, 375, 147)
        mouseMove(panel, 375, 267)
        mouseMove(panel, 375, 147)
        mouseRelease(panel, 375, 147)
        compare(reordered.count, 0)
        compare(panel.orderedIds()[22], "group-22")
    }

    function test_cancel_data() {
        return [{ tag: "outside" }, { tag: "disabled" }, { tag: "hidden" },
                { tag: "profile" }, { tag: "filter" }, { tag: "rows" }, { tag: "grab" }]
    }
    function test_cancel(data) {
        mousePress(panel, 375, 147)
        mouseMove(panel, 375, 267)
        verify(panel.dragActive)
        switch (data.tag) {
        case "outside": mouseRelease(panel, 430, 267); break
        case "disabled": panel.enabled = false; break
        case "hidden": panel.visible = false; break
        case "profile": panel.profileId = "source-b"; break
        case "filter": panel.filterKey = "hide-unchecked"; break
        case "rows": backendRows = backendRows.slice(); break
        case "grab": panel.cancelDrag(); break
        }
        verify(!panel.dragActive)
        compare(reordered.count, 0)
        compare(panel.orderedIds()[22], "group-22")
    }

    function test_selection_and_search() {
        mouseClick(panel, 100, 147)
        compare(selection.count, 1)
        compare(backendRows[22].selected, false)
        compare(panel.scrollOffset, 1200)
        panel.reorderEnabled = false
        mousePress(panel, 375, 147)
        mouseMove(panel, 375, 267)
        mouseRelease(panel, 375, 267)
        verify(!panel.dragActive)
        compare(reordered.count, 0)
    }

    function test_first_last_and_gaps_data() {
        return [{ tag: "first", offset: 0, start: 87, end: 2, from: 1, to: 0 },
                { tag: "last", offset: 5654, start: 133, end: 338, from: 96, to: 99 },
                { tag: "gap", offset: 1207, start: 20, end: 230, from: 20, to: 23 }]
    }
    function test_first_last_and_gaps(data) {
        panel.setScrollOffset(data.offset)
        mousePress(panel, 375, data.start)
        mouseMove(panel, 375, data.end)
        verify(panel.dragActive)
        compare(panel.orderedIds()[data.to], "group-" + data.from)
        mouseRelease(panel, 375, data.end)
        compare(reordered.count, 1)
        compare(panel.scrollOffset, data.offset)
    }

    function test_small_list_and_empty_update() {
        backendRows = backendRows.slice(0, 3)
        compare(panel.scrollOffset, 0)
        mousePress(panel, 375, 27)
        mouseMove(panel, 375, 147)
        verify(panel.dragActive)
        mouseRelease(panel, 375, 147)
        compare(backendRows[2].id, "group-0")
        mousePress(panel, 375, 87)
        mouseMove(panel, 375, 147)
        verify(panel.dragActive)
        backendRows = []
        compare(panel.count, 0)
        verify(!panel.dragActive)
        compare(reordered.count, 1)
    }

    function test_wheel_during_drag() {
        mousePress(panel, 375, 147)
        mouseMove(panel, 375, 177)
        verify(panel.dragActive)
        mouseWheel(panel, 375, 177, 0, -120)
        verify(panel.scrollOffset > 1200)
        verify(panel.dragActive)
        compare(reordered.count, 0)
        mouseRelease(panel, 375, 177)
        compare(reordered.count, 1)
    }
}
