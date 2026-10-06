pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtTest
import QtQuick.Window
import "../../qml/components"

TestCase {
    id: testCase
    name: "EpgSearchOverlay"
    width: 1280
    height: 720
    visible: true
    when: windowShown
    property int actions: 0
    property int beginnings: 0
    property int recordings: 0
    property int downloads: 0
    property int underlyingClicks: 0
    Rectangle {
        id: videoFixture
        anchors.fill: parent
        z: -1
        color: "#fff2d8"
        MouseArea {
            anchors.fill: parent
            onClicked: testCase.underlyingClicks++
        }
    }
    QtObject {
        id: controller
        property var model: rows
        property string query: ""
        property string timeFilter: "all"
        property string status: "idle"
        property bool busy: false
        property bool fetchingMore: false
        property bool resultsCurrent: true
        property string errorText: ""
        property bool hasMore: false
        property string selectedKey: "test-1"
        property int selectedIndex: 0
        property var selectedDetails: ({title: "Planet <Earth>", channelName: "Fixture", timeLabel: "Today, 12:00", actionKind: "live", primaryLabel: "Watch live", primaryEnabled: true, recordingVisible: true, recordingEnabled: true, recordingLabel: "Schedule recording", downloadEnabled: true, fromBeginningEnabled: true})
        property bool detailsBusy: false
        property string sourceName: "Fixture source"
        property string dataAgeText: ""
        property bool expanded: false
        property bool active: true
        signal stateChanged()
        signal showDetailsRequested()
        function setQuery(value) { query = value; status = value.trim().length >= 2 ? "ready" : "idle"; if (value.length >= 2) expanded = true; stateChanged() }
        function setTimeFilter(value) { timeFilter = value; stateChanged() }
        function selectIndex(value) { selectedIndex = value; selectedKey = rows.get(value).resultKey; stateChanged() }
        function moveSelection(delta) { selectIndex(Math.max(0, Math.min(rows.count - 1, selectedIndex + delta))) }
        function activateSelected() { if (resultsCurrent) testCase.actions++ }
        function activateSelectedFromBeginningOrDefault() {
            if (selectedDetails.fromBeginningEnabled) playSelectedFromBeginning()
            else activateSelected()
        }
        function toggleSelectedRecording() { if (resultsCurrent) testCase.recordings++ }
        function downloadSelected() { if (resultsCurrent) testCase.downloads++ }
        function playSelectedFromBeginning() { if (resultsCurrent) { testCase.actions++; testCase.beginnings++ } }
        function fetchNextPage() { }
        function retry() { }
    }
    ListModel {
        id: rows
        ListElement { resultKey: "test-1"; title: "Planet <Earth> & Ocean"; subTitle: "Oceans"; episodeNum: "S01E01"; channelName: "Fixture"; channelLogo: ""; sectionKey: "now"; timeLabel: "Today, 12:00–13:00"; statusLabel: "Now"; titleHighlights: []; subTitleHighlights: [] }
        ListElement { resultKey: "test-2"; title: "Planet Earth"; subTitle: "Mountains"; episodeNum: "S01E02"; channelName: "Fixture"; channelLogo: ""; sectionKey: "upcoming"; timeLabel: "Tomorrow, 12:00–13:00"; statusLabel: "Upcoming"; titleHighlights: []; subTitleHighlights: [] }
    }
    EpgSearchOverlay {
        id: overlay
        controller: controller
        onCloseRequested: overlay.close()
    }
    function init() {
        testCase.width = 1280
        testCase.height = 720
        controller.expanded = false
        controller.resultsCurrent = true
        controller.hasMore = false
        controller.detailsBusy = false
        controller.status = "idle"
        controller.errorText = ""
        controller.query = ""
        controller.timeFilter = "all"
        controller.selectedIndex = 0
        controller.selectedKey = "test-1"
        overlay.inputBlocked = false
        overlay.uiTransparency = 100
        failOnWarning(/.*/)
        actions = 0; beginnings = 0; recordings = 0; downloads = 0; underlyingClicks = 0
        overlay.open()
        tryCompare(overlay, "opened", true)
        tryCompare(overlay.queryField, "activeFocus", true)
    }
    function cleanup() { overlay.close(); tryCompare(overlay, "opened", false); if (rows.count > 2) rows.remove(2, rows.count - 2) }
    function test_noDimmerAndOutsideInputIsBlocked() {
        waitForRendering(overlay.contentItem)
        const frame = grabImage(testCase.Window.window.contentItem)
        compare(frame.pixel(20, 20), videoFixture.color)
        mouseClick(testCase, 20, 20)
        compare(underlyingClicks, 0)
        verify(overlay.opened)
        overlay.close()
        tryCompare(overlay, "opened", false)
        mouseClick(testCase, 20, 20)
        compare(underlyingClicks, 1)
    }
    function test_initialAndStableGeometry() {
        const field = overlay.queryField
        const origin = field.mapToItem(testCase, 0, 0)
        compare(field.font.pixelSize, 28)
        compare(field.height, 60)
        compare(overlay.dim, false)
        verify(!findChild(overlay.contentItem, "ui.epgSearch.body").visible)
        const close = findChild(overlay.contentItem, "ui.epgSearch.close")
        const closeOrigin = close.mapToItem(testCase, 0, 0)
        compare(closeOrigin.x, origin.x + field.width + 12)
        compare(closeOrigin.y + close.height, origin.y - 12)
        controller.setQuery("planet")
        waitForRendering(overlay.contentItem)
        compare(field.mapToItem(testCase, 0, 0), origin)
        verify(findChild(overlay.contentItem, "ui.epgSearch.filters").visible)
        controller.setQuery("")
        waitForRendering(overlay.contentItem)
        compare(field.mapToItem(testCase, 0, 0), origin)
        verify(!findChild(overlay.contentItem, "ui.epgSearch.body").visible)
    }
    function test_selectedCardAndLogo() {
        controller.setQuery("planet")
        waitForRendering(overlay.contentItem)
        const first = overlay.resultsView.itemAtIndex(0)
        const second = overlay.resultsView.itemAtIndex(1)
        verify(first.detailsPane)
        compare(second.detailsPane, null)
        const logo = findChild(first, first.objectName + ".logo")
        verify(!logo.visible)
        compare(first.headerHeight, 64)
        compare(logo.width, 40)
        const airingStatus = findChild(second, second.objectName + ".status")
        verify(!airingStatus.truncated)
        rows.setProperty(0, "channelLogo", Qt.resolvedUrl("../../resources/icons/play.svg").toString())
        tryCompare(logo, "status", Image.Ready)
        verify(logo.visible)
        compare(first.informationInset, 64)
        verify(first.height <= 156)
        controller.selectIndex(1)
        waitForRendering(overlay.contentItem)
        compare(first.detailsPane, null)
        verify(second.detailsPane)
        rows.setProperty(0, "channelLogo", "")
    }
    function test_transparency() {
        compare(overlay.queryField.background.color.a.toFixed(3), (150 / 255).toFixed(3))
        overlay.uiTransparency = 0
        compare(overlay.queryField.background.color.a, 1)
        controller.setQuery("planet")
        compare(findChild(overlay.contentItem, "ui.epgSearch.body").color.a, 1)
        compare(overlay.queryField.color.a, 1)
    }
    function test_editingAndSelection() {
        keyClick(Qt.Key_V); keyClick(Qt.Key_Space); keyClick(Qt.Key_5)
        compare(controller.query, "v 5")
        verify(controller.expanded)
        keyClick(Qt.Key_Down)
        compare(controller.selectedKey, "test-2")
        verify(overlay.queryField.activeFocus)
        keyClick(Qt.Key_Return)
        compare(actions, 1)
        keyClick(Qt.Key_F, Qt.ControlModifier)
        compare(overlay.queryField.selectedText, "v 5")
        keyClick(Qt.Key_Backspace)
        compare(controller.query, "")
        verify(controller.expanded) // Backend session state is independent of the compact UI.
        verify(!overlay.expanded)
        verify(!findChild(overlay.contentItem, "ui.epgSearch.body").visible)
    }
    function test_iconActions_data() {
        return [{tag: "live", kind: "live", label: "Watch live", icon: "play.svg"},
            {tag: "play", kind: "catchup", label: "Play", icon: "play.svg"},
            {tag: "resume", kind: "catchup", label: "Resume", icon: "play.svg"},
            {tag: "details", kind: "details", label: "Show details", icon: "movie-info.svg"}]
    }
    function test_iconActions(data) {
        controller.setQuery("planet")
        const saved = controller.selectedDetails
        controller.selectedDetails = Object.assign({}, saved, {actionKind: data.kind, primaryLabel: data.label})
        waitForRendering(overlay.contentItem)
        for (const entry of [{name: "primary", icon: data.icon, label: data.label},
                {name: "beginning", icon: "start-from-beginning.svg", label: "Play from beginning"},
                {name: "recording", icon: "dvr.svg", label: "Schedule recording"},
                {name: "download", icon: "download.svg", label: "Download"}]) {
            const button = findChild(overlay.contentItem, "ui.epgSearch." + entry.name)
            verify(button.visible)
            compare(button.width, 28)
            compare(button.height, 28)
            compare(button.ToolTip.text, entry.label)
            compare(button.Accessible.name, entry.label)
            compare(button.contentItem.source.toString(), "qrc:/resources/icons/" + entry.icon)
            tryCompare(button.contentItem, "status", Image.Ready)
            mouseMove(button, button.width / 2, button.height / 2)
            tryCompare(button, "hovered", true)
            tryCompare(button.ToolTip, "visible", true)
        }
        controller.selectedDetails = saved
    }
    function test_pastRecordingActionHidden() {
        controller.setQuery("planet")
        const saved = controller.selectedDetails
        controller.selectedDetails = Object.assign({}, saved, {recordingVisible: false, recordingEnabled: false})
        waitForRendering(overlay.contentItem)
        const button = findChild(overlay.contentItem, "ui.epgSearch.recording")
        verify(!button.visible)
        // An existing job retains its cancellation action and tooltip.
        controller.selectedDetails = Object.assign({}, saved, {recordingLabel: "Cancel recording"})
        waitForRendering(overlay.contentItem)
        verify(button.visible)
        compare(button.ToolTip.text, "Cancel recording")
        controller.selectedDetails = saved
    }
    function test_controlEnter_data() {
        const cases = []
        for (const key of [Qt.Key_Return, Qt.Key_Enter])
            for (const focus of ["query", "results", "recording"])
                for (const available of [true, false])
                    cases.push({tag: key + "-" + focus + "-" + available, key, focus, available})
        return cases
    }
    function test_controlEnter(data) {
        controller.setQuery("planet")
        const saved = controller.selectedDetails
        controller.selectedDetails = Object.assign({}, saved, {fromBeginningEnabled: data.available})
        waitForRendering(overlay.contentItem)
        const beginning = findChild(overlay.contentItem, "ui.epgSearch.beginning")
        compare(beginning.visible, data.available)
        const focus = data.focus === "query" ? overlay.queryField : data.focus === "results"
            ? overlay.resultsView : findChild(overlay.contentItem, "ui.epgSearch.recording")
        focus.forceActiveFocus()
        keyClick(data.key, Qt.ControlModifier)
        compare(actions, 1)
        compare(beginnings, data.available ? 1 : 0)
        compare(recordings, 0)
        keyClick(data.key, Qt.ControlModifier | Qt.ShiftModifier)
        compare(actions, 1)
        controller.selectedDetails = saved
    }
    function test_controlEnterBlocked_data() {
        return [{tag: "stale"}, {tag: "loading"}, {tag: "protected"}]
    }
    function test_controlEnterBlocked(data) {
        controller.setQuery("planet")
        if (data.tag === "stale") controller.resultsCurrent = false
        else if (data.tag === "loading") controller.detailsBusy = true
        else overlay.inputBlocked = true
        keyClick(Qt.Key_Return, Qt.ControlModifier)
        compare(actions, 0)
        compare(beginnings, 0)
    }
    function test_pointerSelectionAndSafeEmphasis() {
        controller.setQuery("planet")
        waitForRendering(overlay.contentItem)
        const first = overlay.resultsView.itemAtIndex(0)
        const second = overlay.resultsView.itemAtIndex(1)
        verify(first); verify(second)
        compare(first.emphasized("Planet <Earth> & Ocean", [{start: 0, length: 6}]), "<b>Planet</b> &lt;Earth&gt; &amp; Ocean")
        mouseMove(second, 20, 20)
        compare(controller.selectedKey, "test-1")
        mouseClick(second, 20, 20)
        compare(controller.selectedKey, "test-2")
        compare(actions, 0)
        mouseDoubleClickSequence(second, 20, 20)
        compare(actions, 1)
    }
    function test_doubleClickUnselectedResult() {
        controller.setQuery("planet")
        waitForRendering(overlay.contentItem)
        const second = overlay.resultsView.itemAtIndex(1)
        mouseDoubleClickSequence(second, 20, 20)
        compare(controller.selectedKey, "test-2")
        compare(actions, 1)
    }
    function test_selectedRowClickTogglesDetails() {
        controller.setQuery("planet")
        waitForRendering(overlay.contentItem)
        const first = overlay.resultsView.itemAtIndex(0)
        const origin = overlay.queryField.mapToItem(testCase, 0, 0)
        verify(first.detailsPane)
        mouseClick(first, 20, 20)
        verify(first.detailsPane) // Leave the double-click interval available.
        tryVerify(() => first.detailsPane === null)
        compare(first.height, first.headerHeight)
        compare(controller.selectedKey, "test-1")
        compare(actions, 0)
        controller.stateChanged() // Detail/action refresh must not reopen it.
        verify(!first.detailsPane)
        compare(overlay.queryField.mapToItem(testCase, 0, 0), origin)
        mouseClick(first, 20, 20)
        tryVerify(() => first.detailsPane !== null)
        wait(overlay.styleHints.mouseDoubleClickInterval + 20)
        verify(first.detailsPane)
        compare(actions, 0)
    }
    function test_selectedRowDoubleClick_data() {
        return [{tag: "expanded", collapse: false}, {tag: "collapsed", collapse: true}]
    }
    function test_selectedRowDoubleClick(data) {
        controller.setQuery("planet")
        waitForRendering(overlay.contentItem)
        const first = overlay.resultsView.itemAtIndex(0)
        if (data.collapse) {
            mouseClick(first, 20, 20)
            tryVerify(() => first.detailsPane === null)
        }
        mouseDoubleClickSequence(first, 20, 20)
        compare(actions, 1)
        wait(overlay.styleHints.mouseDoubleClickInterval + 20)
        verify(first.detailsPane)
        compare(actions, 1)
    }
    function test_collapsedDetailsReopenOnSelectionOrRequest() {
        controller.setQuery("planet")
        waitForRendering(overlay.contentItem)
        const first = overlay.resultsView.itemAtIndex(0)
        mouseClick(first, 20, 20)
        tryVerify(() => first.detailsPane === null)
        controller.showDetailsRequested()
        tryVerify(() => first.detailsPane !== null)
        mouseClick(first, 20, 20)
        tryVerify(() => first.detailsPane === null)
        controller.selectIndex(1)
        controller.selectIndex(0)
        tryVerify(() => first.detailsPane !== null)
    }
    function test_pendingCollapseCancelledByNewQuery() {
        controller.setQuery("planet")
        waitForRendering(overlay.contentItem)
        mouseClick(overlay.resultsView.itemAtIndex(0), 20, 20)
        controller.resultsCurrent = false
        controller.setQuery("earth")
        controller.resultsCurrent = true
        controller.stateChanged()
        wait(overlay.styleHints.mouseDoubleClickInterval + 20)
        verify(overlay.resultsView.itemAtIndex(0).detailsPane)
        compare(actions, 0)
    }
    function test_newRowPressCancelsPendingCollapse() {
        controller.setQuery("planet")
        waitForRendering(overlay.contentItem)
        const first = overlay.resultsView.itemAtIndex(0)
        const second = overlay.resultsView.itemAtIndex(1)
        mouseClick(first, 20, 20)
        mousePress(second, 20, 20)
        wait(overlay.styleHints.mouseDoubleClickInterval + 20)
        verify(first.detailsPane)
        mouseRelease(second, 20, 20)
        compare(controller.selectedKey, "test-2")
        verify(second.detailsPane)
        compare(actions, 0)
    }
    function test_doubleClickAtFixedWindowPosition() {
        controller.setQuery("planet")
        waitForRendering(overlay.contentItem)
        const second = overlay.resultsView.itemAtIndex(1)
        const point = second.mapToItem(testCase, 20, 20)
        mouseDoubleClickSequence(testCase, point.x, point.y, Qt.LeftButton, Qt.NoModifier, 50)
        compare(controller.selectedKey, "test-2")
        compare(actions, 1)
    }
    function test_doubleClickWaitsForCurrentDetails() {
        controller.setQuery("planet")
        controller.detailsBusy = true
        waitForRendering(overlay.contentItem)
        mouseDoubleClickSequence(overlay.resultsView.itemAtIndex(0), 20, 20)
        compare(actions, 0)
        controller.detailsBusy = false
        controller.stateChanged()
        tryCompare(testCase, "actions", 1)
    }
    function test_pendingDoubleClickCancellation_data() {
        return [{tag: "query"}, {tag: "filter"}, {tag: "selection"}, {tag: "context"}, {tag: "close"}, {tag: "protected"}]
    }
    function test_pendingDoubleClickCancellation(data) {
        controller.setQuery("planet")
        controller.detailsBusy = true
        waitForRendering(overlay.contentItem)
        mouseDoubleClickSequence(overlay.resultsView.itemAtIndex(0), 20, 20)
        compare(actions, 0)
        if (data.tag === "query") controller.setQuery("different")
        else if (data.tag === "filter") controller.setTimeFilter("upcoming")
        else if (data.tag === "selection") controller.selectIndex(1)
        else if (data.tag === "context") { controller.resultsCurrent = false; controller.stateChanged() }
        else if (data.tag === "close") overlay.close()
        else overlay.inputBlocked = true
        controller.detailsBusy = false
        controller.stateChanged()
        wait(0)
        compare(actions, 0)
    }
    function test_detailButtonDoesNotActivateRow() {
        controller.setQuery("planet")
        waitForRendering(overlay.contentItem)
        const button = findChild(overlay.contentItem, "ui.epgSearch.recording")
        mouseDoubleClickSequence(button)
        verify(recordings > 0)
        compare(actions, 0)
    }
    function test_longQueryIsNotSilentlyTruncated() {
        const text = "p".repeat(257)
        overlay.queryField.insert(0, text)
        compare(overlay.queryField.text, text)
        verify(overlay.queryField.maximumLength >= 257)
    }
    function test_longDescriptionKeepsActionsAccessible() {
        controller.setQuery("planet")
        const saved = controller.selectedDetails
        controller.selectedDetails = Object.assign({}, saved, {description: "Very long description. ".repeat(1000)})
        waitForRendering(overlay.contentItem)
        const primary = findChild(overlay.contentItem, "ui.epgSearch.primary")
        const description = findChild(overlay.contentItem, "ui.epgSearch.description")
        verify(primary)
        verify(primary.visible)
        verify(primary.mapToItem(overlay.contentItem, 0, 0).y + primary.height <= overlay.contentItem.height)
        compare(description.height, 48)
        controller.selectedDetails = saved
    }
    function test_descriptionWheelDoesNotScrollResults() {
        controller.setQuery("planet")
        const saved = controller.selectedDetails
        controller.selectedDetails = Object.assign({}, saved, {description: "Long description. ".repeat(1000)})
        waitForRendering(overlay.contentItem)
        const description = findChild(overlay.contentItem, "ui.epgSearch.description")
        const outerOffset = overlay.resultsView.contentY
        mouseWheel(description, description.width / 2, description.height / 2, 0, -120)
        tryVerify(() => description.contentItem.contentY > 0)
        compare(overlay.resultsView.contentY, outerOffset)
        controller.selectedDetails = saved
    }
    function test_pageAppendAndActionUpdatesPreserveScroll() {
        controller.setQuery("planet")
        for (let i = 0; i < 30; ++i) {
            rows.append({resultKey: "page-" + i, title: "Planet page " + i, subTitle: "", episodeNum: "", channelName: "Fixture", channelLogo: "", sectionKey: "upcoming", timeLabel: "Tomorrow", statusLabel: "Upcoming", titleHighlights: [], subTitleHighlights: []})
        }
        waitForRendering(overlay.contentItem)
        overlay.resultsView.contentY = 600
        const offset = overlay.resultsView.contentY
        verify(offset > 0)
        controller.hasMore = true
        const countLabel = findChild(overlay.contentItem, "ui.epgSearch.resultCount")
        compare(countLabel.text, "Shown 32 results · more available")
        controller.stateChanged()
        compare(overlay.resultsView.contentY, offset)
        rows.append({resultKey: "next-page", title: "Planet next page", subTitle: "", episodeNum: "", channelName: "Fixture", channelLogo: "", sectionKey: "upcoming", timeLabel: "Tomorrow", statusLabel: "Upcoming", titleHighlights: [], subTitleHighlights: []})
        controller.stateChanged()
        compare(overlay.resultsView.contentY, offset)
        compare(controller.selectedKey, "test-1")
        tryCompare(countLabel, "text", "Shown 33 results · more available")
        controller.hasMore = false
        compare(countLabel.text, "Shown 33 results")
    }
    function test_normalizedShortQueryHint() {
        controller.setQuery("a b")
        controller.status = "idle"
        controller.resultsCurrent = false
        compare(overlay.stateMessage(), "Enter at least 2 characters")
        controller.setQuery("!?")
        controller.status = "idle"
        compare(overlay.stateMessage(), "Enter at least 2 characters")
    }
    function test_noStaleActions() {
        controller.setQuery("planet")
        controller.resultsCurrent = false
        keyClick(Qt.Key_Return); keyClick(Qt.Key_Enter)
        keyClick(Qt.Key_R, Qt.ControlModifier); keyClick(Qt.Key_D, Qt.ControlModifier)
        compare(actions, 0); compare(recordings, 0); compare(downloads, 0)
    }
    function test_buttonEnterSingleAction() {
        controller.setQuery("planet")
        const button = findChild(overlay.contentItem, "ui.epgSearch.recording")
        verify(button)
        button.forceActiveFocus()
        keyClick(Qt.Key_Return)
        compare(recordings, 1)
        compare(actions, 0)
        keyClick(Qt.Key_Enter)
        compare(recordings, 2)
    }
    function test_tabCyclesTimeFilters_data() {
        return [{tag: "query"}, {tag: "list"}, {tag: "filter"}, {tag: "description"},
            {tag: "action"}, {tag: "close"}, {tag: "empty"}, {tag: "loading"}]
    }
    function test_tabCyclesTimeFilters(data) {
        controller.setQuery("planet")
        waitForRendering(overlay.contentItem)
        const targets = {query: overlay.queryField, list: overlay.resultsView,
            filter: findChild(overlay.contentItem, "ui.epgSearch.filter.upcoming"),
            description: findChild(overlay.contentItem, "ui.epgSearch.description"),
            action: findChild(overlay.contentItem, "ui.epgSearch.primary"),
            close: findChild(overlay.contentItem, "ui.epgSearch.close")}
        if (data.tag === "empty") controller.setQuery("")
        if (data.tag === "loading") { controller.resultsCurrent = false; controller.busy = true }
        const target = targets[data.tag] || overlay.queryField
        target.forceActiveFocus()
        overlay.queryField.cursorPosition = 3
        if (data.tag === "query") overlay.queryField.select(1, 4)
        const cursor = overlay.queryField.cursorPosition
        const selectedText = overlay.queryField.selectedText
        for (const expected of ["now", "upcoming", "past", "all", "now"]) {
            keyClick(Qt.Key_Tab)
            compare(controller.timeFilter, expected)
            verify(overlay.queryField.activeFocus)
            compare(controller.query, data.tag === "empty" ? "" : "planet")
            compare(overlay.queryField.cursorPosition, cursor)
            compare(overlay.queryField.selectedText, selectedText)
        }
        compare(actions, 0); compare(recordings, 0); compare(downloads, 0)
        controller.busy = false
    }
    function test_tabRespectsModifiersAndInputBlock() {
        controller.setQuery("planet")
        keyClick(Qt.Key_Tab, Qt.ControlModifier)
        compare(controller.timeFilter, "all")
        const repeat = {key: Qt.Key_Tab, modifiers: Qt.NoModifier, isAutoRepeat: true, accepted: false}
        overlay.tabKey(repeat)
        verify(repeat.accepted)
        compare(controller.timeFilter, "all")
        overlay.inputBlocked = true
        overlay.handleTab(false)
        compare(controller.timeFilter, "all")
        overlay.inputBlocked = false
        overlay.close()
        tryCompare(overlay, "opened", false)
        overlay.handleTab(false)
        compare(controller.timeFilter, "all")
    }
    function test_shiftTabFocusCycle() {
        controller.setQuery("planet")
        for (let i = 0; i < 24; ++i) {
            const before = testCase.Window.window.activeFocusItem
            keyClick(Qt.Key_Backtab, Qt.ShiftModifier)
            verify(overlay.activeFocus)
            verify(testCase.Window.window.activeFocusItem !== before,
                "Step " + i + " remained on " + (before ? before.objectName : "null"))
            compare(controller.timeFilter, "all")
        }
    }
    function test_smallGeometry() {
        testCase.width = 426; testCase.height = 240
        controller.setQuery("planet")
        waitForRendering(overlay.contentItem)
        verify(overlay.queryField.width > 180)
        const closeButton = findChild(overlay.contentItem, "ui.epgSearch.close")
        verify(closeButton.visible)
        verify(overlay.resultsView.height >= 56)
        controller.showDetailsRequested()
        tryVerify(() => overlay.activeDetailsPane !== null)
        wait(0)
        for (const button of overlay.activeDetailsPane.focusTargets) {
            if (!button.visible || !button.enabled) continue
            button.forceActiveFocus()
            wait(0)
            const bounds = button.mapToItem(overlay.resultsView, 0, 0)
            // The description viewport may exceed this tiny result viewport;
            // all action buttons must be fully reachable.
            if (button.objectName !== "ui.epgSearch.description") {
                verify(bounds.y >= -1)
                verify(bounds.y + button.height <= overlay.resultsView.height + 1)
            }
        }
        keyClick(Qt.Key_Escape)
        tryCompare(overlay, "opened", false)
    }
    function test_compactPageErrorKeepsRetry() {
        testCase.width = 426; testCase.height = 240
        controller.setQuery("planet")
        controller.errorText = "Next page failed"
        waitForRendering(overlay.contentItem)
        const retry = findChild(overlay.contentItem, "ui.epgSearch.retryCurrent")
        verify(retry.visible)
        verify(retry.enabled)
        const bounds = retry.mapToItem(testCase, 0, 0)
        verify(bounds.y >= 0)
        verify(bounds.y + retry.height <= testCase.height)
    }
    function test_errorAndDialogPriority() {
        controller.setQuery("planet")
        controller.errorText = "Fixture search error"
        controller.resultsCurrent = false
        overlay.inputBlocked = true
        keyClick(Qt.Key_R, Qt.ControlModifier)
        compare(recordings, 0)
        overlay.inputBlocked = false
        tryCompare(overlay.queryField, "activeFocus", true)
        keyClick(Qt.Key_Escape)
        tryCompare(overlay, "opened", false)
    }
}
