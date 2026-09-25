import QtQuick
import QtQuick.Controls
import QtTest
import "../../qml/components"

TestCase {
    id: testCase
    name: "UpdateAvailable"
    width: 426
    height: 240
    visible: true
    when: windowShown

    QtObject {
        id: updates
        property string currentVersion: "0.5.4"
        property string latestVersion: "0.5.5"
        property string errorText: ""
        property bool pending: false
        property string action: ""
        property bool fail: false
        signal changed()
        function dismiss() { action = "later"; pending = false; changed() }
        function openRelease() { choose("yes") }
        function skipVersion() { choose("skip") }
        function choose(value) {
            action = value
            if (fail)
                errorText = "Could not complete this action. Try again or choose Not now."
            else
                pending = false
            changed()
        }
    }
    UpdateAvailableDialog {
        id: dialog
        controller: updates
    }
    function init() {
        dialog.close()
        updates.pending = false
        updates.fail = false
        updates.errorText = ""
        updates.action = ""
        dialog.allowedToOpen = true
        updates.changed()
    }
    function show() {
        updates.pending = true
        updates.changed()
        tryCompare(dialog, "opened", true)
    }
    function test_fits_minimum_window() {
        show()
        verify(dialog.width <= testCase.width, "Dialog width: " + dialog.width)
        verify(dialog.height <= testCase.height, "Dialog height: " + dialog.height)
        const message = findChild(dialog, "updateMessage")
        tryVerify(() => message.availableHeight >= message.contentHeight, 1000, "Version text must fit without scrolling: " + message.availableHeight + "/" + message.contentHeight + " compact=" + dialog.compactWindow)
    }
    function test_actions_data() {
        return [
            {tag: "Yes", button: "updateYes", action: "yes"},
            {tag: "Skip", button: "updateSkip", action: "skip"},
            {tag: "Not now", button: "updateNotNow", action: "later"}
        ]
    }
    function test_actions(data) {
        show()
        mouseClick(findChild(dialog, data.button))
        compare(updates.action, data.action)
        tryCompare(dialog, "visible", false)
    }
    function test_escape_and_default_focus() {
        show()
        verify(findChild(dialog, "updateNotNow").activeFocus)
        keyClick(Qt.Key_Escape)
        compare(updates.action, "later")
        tryCompare(dialog, "visible", false)
    }
    function test_keyboard_navigation() {
        show()
        keyClick(Qt.Key_Tab)
        verify(findChild(dialog, "updateYes").activeFocus)
        keyClick(Qt.Key_Tab)
        verify(findChild(dialog, "updateSkip").activeFocus)
        keyClick(Qt.Key_Return)
        compare(updates.action, "skip")
        tryCompare(dialog, "visible", false)
    }
    function test_waits_for_other_interaction() {
        dialog.allowedToOpen = false
        updates.pending = true
        updates.changed()
        verify(!dialog.visible)
        dialog.allowedToOpen = true
        tryCompare(dialog, "opened", true)
        dialog.allowedToOpen = false
        tryCompare(dialog, "visible", false)
        verify(updates.pending)
        compare(updates.action, "")
        dialog.allowedToOpen = true
        tryCompare(dialog, "opened", true)
        updates.pending = false // shutdown
        updates.changed()
        tryCompare(dialog, "visible", false)
    }
    function test_error_keeps_choices_available() {
        updates.fail = true
        show()
        mouseClick(findChild(dialog, "updateSkip"))
        verify(dialog.visible)
        verify(updates.pending)
        verify(updates.errorText.length > 0)
        verify(dialog.height <= testCase.height)
        mouseClick(findChild(dialog, "updateNotNow"))
        tryCompare(dialog, "visible", false)
    }
    function test_outside_click_does_not_dismiss() {
        show()
        mouseClick(testCase, 2, 2)
        verify(dialog.visible)
        verify(updates.pending)
    }
}
