pragma ComponentBehavior: Bound

import QtQuick
import QtTest
import "../../qml/components"

TestCase {
    id: testCase
    name: "MediaModeSwitch"
    width: 426
    height: 100
    visible: true
    when: windowShown

    MediaModeSwitch {
        id: control
        anchors.centerIn: parent
    }
    SignalSpy { id: requests; target: control; signalName: "modeRequested" }
    SignalSpy { id: dismissals; target: control; signalName: "dismissed" }
    Component {
        id: chromeFixture
        MediaModeChrome { width: 426; targetVisible: true }
    }

    function init() {
        control.enabled = true;
        control.selectedMode = "live";
        control.width = 300;
        control.focus = false;
        requests.clear();
        dismissals.clear();
        wait(200);
    }
    function test_clicks_are_requests_and_repeat_is_not_a_toggle() {
        const movies = findChild(control, "ui.navigation.movies");
        const live = findChild(control, "ui.navigation.live");
        const series = findChild(control, "ui.navigation.series");
        compare(movies.text, "Movies"); compare(live.text, "Live TV"); compare(series.text, "Series");
        verify(movies.x < live.x && live.x < series.x);
        mouseClick(movies);
        compare(requests.signalArguments[0][0], "movies");
        compare(control.selectedMode, "live"); // The owner confirms actual navigation.
        control.selectedMode = "movies";
        mouseClick(movies);
        compare(requests.signalArguments[1][0], "movies");
        compare(control.selectedMode, "movies");
        mouseClick(series);
        compare(requests.signalArguments[2][0], "series");
    }
    function test_indicator_moves_and_fits_minimum_width() {
        const indicator = findChild(control, "ui.navigation.indicator");
        const before = indicator.x;
        control.selectedMode = "series";
        wait(50);
        verify(indicator.x > before && indicator.x < 3 + 2 * indicator.width);
        tryCompare(indicator, "x", 3 + 2 * indicator.width);
        control.width = 216;
        tryCompare(indicator, "x", 143);
        compare(indicator.width, 70);
        const series = findChild(control, "ui.navigation.series");
        verify(series.x + series.width <= control.width);
        mouseClick(series);
        compare(requests.signalArguments[0][0], "series");
    }
    function test_keyboard_does_not_change_selection_until_confirmed() {
        control.forceActiveFocus();
        verify(control.interactionActive);
        keyClick(Qt.Key_Right);
        compare(control.focusedIndex, 2);
        compare(control.selectedMode, "live");
        keyClick(Qt.Key_Right);
        compare(control.focusedIndex, 2);
        keyClick(Qt.Key_Return);
        compare(requests.signalArguments[0][0], "series");
        keyClick(Qt.Key_Left); keyClick(Qt.Key_Left); keyClick(Qt.Key_Left);
        compare(control.focusedIndex, 0);
        keyClick(Qt.Key_Space);
        compare(requests.signalArguments[1][0], "movies");
        keyClick(Qt.Key_Escape);
        compare(dismissals.count, 1);
    }
    function test_disabled_control_rejects_pointer_and_keyboard() {
        control.enabled = false;
        mouseClick(findChild(control, "ui.navigation.movies"));
        control.choose(2);
        compare(requests.count, 0);
    }
    function test_navigation_gating_keeps_chrome_visible() {
        const chrome = createTemporaryObject(chromeFixture, testCase);
        tryCompare(chrome, "animating", false);
        compare(chrome.opacity, 1);
        const live = findChild(chrome, "ui.navigation.live");
        const position = live.mapToItem(testCase, 0, 0);
        chrome.navigationEnabled = false;
        chrome.selectedMode = "movies";
        wait(50);
        verify(chrome.visible);
        compare(chrome.opacity, 1);
        verify(!live.enabled);
        compare(live.mapToItem(testCase, 0, 0), position);
        chrome.selectedMode = "live";
        wait(50);
        compare(chrome.opacity, 1);
        compare(live.mapToItem(testCase, 0, 0), position);
        chrome.navigationEnabled = true;
        verify(live.enabled);
    }
}
