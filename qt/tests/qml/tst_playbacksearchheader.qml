pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import "../../qml/components"

TestCase {
    id: testCase
    name: "PlaybackSearchHeader"
    width: 340
    height: 100
    visible: true
    when: windowShown

    PlaybackSearchHeader {
        id: header
        width: parent.width - 12
        x: 4
        y: 16
        searchObjectName: "search"
        switchActionObjectName: "switch"
        onSearchKeyPressed: function(event) {
            if (event.key === Qt.Key_Down) {
                event.accepted = true
                testCase.listFocusRequests++
            }
        }
    }
    property int listFocusRequests: 0
    SignalSpy { id: edits; target: header; signalName: "textEdited" }
    SignalSpy { id: switches; target: header; signalName: "switchRequested" }

    function init() {
        header.enabled = true
        header.text = ""
        header.field.focus = false
        edits.clear()
        switches.clear()
        listFocusRequests = 0
    }

    function test_modes_data() {
        return [
            {tag: "channels", placeholder: "Search channels", label: "← Groups", neutral: false},
            {tag: "movies", placeholder: "Search movies", label: "← Groups", neutral: true},
            {tag: "live groups", placeholder: "Search groups", label: "Channels →", neutral: false},
            {tag: "movie groups", placeholder: "Search groups", label: "Movies →", neutral: true}
        ]
    }

    function test_modes(data) {
        header.placeholderText = data.placeholder
        header.switchText = data.label
        header.neutralPalette = data.neutral
        waitForRendering(header)
        header.field.forceActiveFocus()
        tryCompare(header.field, "activeFocus", true)
        keyClick(Qt.Key_V); keyClick(Qt.Key_Space); keyClick(Qt.Key_5)
        compare(header.text, "v 5")
        compare(edits.count, 3)
        keyClick(Qt.Key_Left)
        keyClick(Qt.Key_Backspace)
        compare(header.text, "v5")
        compare(listFocusRequests, 0)
        keyClick(Qt.Key_Down)
        compare(listFocusRequests, 1)
        mouseClick(findChild(header, "switch"))
        compare(switches.count, 1)
        compare(header.text, "v5")
    }

    function test_disabledHeaderDoesNotSwitch() {
        header.enabled = false
        mouseClick(findChild(header, "switch"))
        compare(switches.count, 0)
    }
}
