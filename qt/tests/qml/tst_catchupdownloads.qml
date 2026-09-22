import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtTest
import "../../qml/components"

TestCase {
    id: testCase
    name: "CatchupDownloads"
    width: 640
    height: 480
    when: windowShown
    visible: true

    ListModel {
        id: downloads
        property bool paused: false
        property bool activeProgressKnown: true
        property bool hasPending: false
        property bool unread: false
        property bool shuttingDown: false
        property int queuedCount: 0
        property real activeProgress: 0
        property string cancelled: ""
        property string restarted: ""
        signal summaryChanged()
        signal notification(string message)
        signal destinationChosen(url destination)
        signal destinationCancelled()
        property url suggested: ""
        function chooseDestination(window, destination) { suggested = destination }
        function cancelDestination() { destinationCancelled() }
        function suggestedDestination(title) { return "file:///tmp/" + encodeURIComponent(title) + ".mkv" }
        function pauseAll() { paused = true }
        function resumeAll() { paused = false }
        function markRead() { unread = false }
        function cancel(jobId) {
            cancelled = jobId
            for (let i = 0; i < count; ++i) {
                if (get(i).jobId === jobId && get(i).jobState === "queued") {
                    setProperty(i, "jobState", "cancelled")
                    break
                }
            }
        }
        function dismiss(jobId) { clear() }
        function restart(jobId) { restarted = jobId }
    }
    QtObject {
        id: app
        property bool eligible: true
        property var submitted: ({})
        property int requests: 0
        property string enqueueError: ""
        function catchupDownloadActionState(channel, program) {
            return { enabled: eligible, reason: eligible ? "" : "Archive unavailable" }
        }
        function enqueueCatchupDownload(channel, program, destination) {
            ++requests
            submitted = { channel: channel, program: program, destination: destination }
            return enqueueError
        }
    }
    Item {
        x: 75
        y: 20
        Item {
            id: transportBar
            y: 350
            width: 540
            height: 110
            Item {
                id: transportSlot
                x: 180
                y: 60
                width: 42
                height: 42
            }
        }
    }
    CatchupDownloads {
        id: ui
        anchors.fill: parent
        buttonHost: transportSlot
        transportBar: transportBar
        app: app
        controller: downloads
        mainWindow: testCase.Window.window
    }
    function init() {
        ui.handleEscape()
        ui.handleEscape()
        ui.notice = ""
        ui.shellChromeVisible = true
        downloads.clear()
        downloads.hasPending = false
        downloads.paused = false
        downloads.unread = false
        downloads.cancelled = ""
        downloads.restarted = ""
        app.eligible = true
        app.requests = 0
        app.enqueueError = ""
    }
    function test_pause_button_toggles_and_survives_panel_close() {
        downloads.hasPending = true
        const indicator = findChild(ui, "ui.downloads.indicator")
        mouseClick(indicator)
        const button = findChild(ui, "ui.downloads.pause")
        const close = findChild(ui, "ui.downloads.close")
        verify(button.visible)
        compare(button.width, 28)
        verify(button.mapToItem(close.parent, 0, 0).x < close.x)
        verify(String(button.iconSource).endsWith("pause.svg"))
        mouseClick(button)
        compare(downloads.paused, true)
        verify(String(button.iconSource).endsWith("play.svg"))
        compare(button.caption, "Resume downloads")
        compare(indicator.pulsing, false)
        mouseClick(close)
        mouseClick(indicator)
        compare(downloads.paused, true)
        mouseClick(button)
        compare(downloads.paused, false)
        downloads.shuttingDown = true
        compare(button.enabled, false)
        downloads.shuttingDown = false
        downloads.hasPending = false
        compare(button.visible, false)
    }

    function test_ineligible_shows_reason_without_dialog() {
        app.eligible = false
        ui.requestDownload({id: 1}, {start: "2026-09-01", title: "Old programme"})
        compare(ui.choosingFile, false)
        compare(ui.notice, "Archive unavailable")
        compare(app.requests, 0)
    }
    function test_missing_selection() {
        ui.requestDownload({}, {})
        verify(ui.notice.indexOf("Select a programme") >= 0)
        compare(ui.choosingFile, false)
    }
    function test_dialog_snapshots_selection_and_only_submits_after_accept() {
        const channel = {id: 1, name: "Channel"}
        const program = {start: "2026-09-01", title: "Selected programme"}
        ui.requestDownload(channel, program)
        compare(ui.choosingFile, true)
        compare(app.requests, 0)
        compare(decodeURIComponent(String(downloads.suggested)), "file:///tmp/Selected programme.mkv")
        program.title = "Different programme"
        channel.id = 2
        // Simulate the file dialog's accepted signal with its selected result.
        downloads.destinationChosen("file:///tmp/selected.mkv")
        compare(app.requests, 1)
        compare(app.submitted.program.title, "Selected programme")
        compare(app.submitted.channel.id, 1)
        compare(String(app.submitted.destination), "file:///tmp/selected.mkv")
    }
    function test_dialog_cancel_creates_no_job() {
        ui.requestDownload({id: 1}, {start: "2026-09-01", title: "Programme"})
        verify(ui.handleEscape())
        compare(ui.choosingFile, false)
        compare(app.requests, 0)
    }
    function test_results_align_with_transport_and_mark_read() {
        downloads.unread = true
        const indicator = findChild(ui, "ui.downloads.indicator")
        verify(indicator !== null)
        tryCompare(indicator, "visible", true)
        mouseClick(indicator)
        tryCompare(ui, "interactionActive", true)
        compare(downloads.unread, false)
        const panel = findChild(ui, "ui.downloads.panel")
        const barPosition = transportBar.mapToItem(panel.parent, 0, 0)
        const panelPosition = panel.background.mapToItem(panel.parent, 0, 0)
        fuzzyCompare(panelPosition.y + panel.height, barPosition.y - 10, 1)
        fuzzyCompare(panelPosition.x, barPosition.x, 1)
        compare(indicator.strongPulse, false)
        verify(ui.handleEscape())
        tryCompare(ui, "interactionActive", false)
        tryCompare(indicator, "visible", false)
    }
    function test_pending_and_unread_pulse_states() {
        const indicator = findChild(ui, "ui.downloads.indicator")
        compare(indicator.pulsing, false)
        compare(indicator.visible, false)
        downloads.hasPending = true
        compare(indicator.visible, true)
        compare(indicator.pulsing, true)
        compare(indicator.strongPulse, false)
        downloads.hasPending = false
        downloads.paused = false
        downloads.unread = true
        compare(indicator.pulsing, true)
        compare(indicator.strongPulse, true)
        mouseClick(indicator)
        tryCompare(ui, "interactionActive", true)
        compare(indicator.pulsing, false)
        downloads.unread = true
        downloads.summaryChanged()
        compare(downloads.unread, false)
    }
    function test_indicator_toggles_panel_without_reopening() {
        downloads.hasPending = true
        const indicator = findChild(ui, "ui.downloads.indicator")
        mouseClick(indicator)
        tryCompare(ui, "interactionActive", true)
        mouseClick(indicator)
        tryCompare(ui, "interactionActive", false)
        mouseClick(indicator)
        tryCompare(ui, "interactionActive", true)
        mouseClick(testCase, testCase.width - 10, 10)
        tryCompare(ui, "interactionActive", false)
    }
    function test_background_notification() {
        downloads.notification("Download complete: Programme")
        compare(ui.notice, "Download complete: Programme")
    }

    function openJob(state) {
        downloads.append({jobId: "job", title: "Programme", channelName: "Channel", programStart: "2026-09-22T12:00:00",
            destination: "/tmp/programme.mkv", jobState: state, progress: 25, progressKnown: true,
            bytes: 1024, errorText: ""})
        downloads.unread = true
        mouseClick(findChild(ui, "ui.downloads.indicator"))
        const list = findChild(ui, "ui.downloads.list")
        tryCompare(list, "count", 1)
        list.forceLayout()
        tryVerify(function() { return list.itemAtIndex(0) !== null })
        tryCompare(list.itemAtIndex(0), "jobState", state)
        return findChild(list.itemAtIndex(0), "ui.downloads.action.job")
    }
    function test_queued_cancel_keeps_restart_and_dismiss_without_confirmation() {
        const action = openJob("queued")
        verify(action !== null)
        mouseClick(action)
        compare(downloads.cancelled, "job")
        compare(downloads.count, 1)
        compare(downloads.get(0).jobState, "cancelled")
        compare(findChild(ui, "ui.downloads.cancelDialog").visible, false)
        const list = findChild(ui, "ui.downloads.list")
        const restart = findChild(list.itemAtIndex(0), "ui.downloads.restart.job")
        tryCompare(restart, "visible", true)
        tryCompare(action, "caption", "Dismiss")
        mouseClick(restart)
        compare(downloads.restarted, "job")
        mouseClick(action)
        compare(downloads.count, 0)
    }
    function test_progress_fill_tracks_saved_media_across_pause_and_resume() {
        openJob("downloading")
        const row = findChild(ui, "ui.downloads.list").itemAtIndex(0)
        const bar = findChild(row, "ui.downloads.progress.job")
        const fill = findChild(row, "ui.downloads.fill.job")
        verify(bar !== null && fill !== null)
        compare(bar.indeterminate, false)
        fuzzyCompare(fill.width / bar.availableWidth, 0.25, 0.01)
        downloads.setProperty(0, "progress", 45)
        tryCompare(bar, "value", 45)
        fuzzyCompare(fill.width / bar.availableWidth, 0.45, 0.01)
        for (const state of ["paused", "resuming", "downloading"]) {
            downloads.setProperty(0, "jobState", state)
            compare(bar.indeterminate, false)
            fuzzyCompare(fill.width / bar.availableWidth, 0.45, 0.01)
        }
        downloads.setProperty(0, "jobState", "finalizing")
        downloads.setProperty(0, "progress", 90)
        tryCompare(bar, "value", 90)
        fuzzyCompare(fill.width / bar.availableWidth, 0.90, 0.01)
    }
    function test_unknown_progress_does_not_look_complete() {
        openJob("downloading")
        downloads.setProperty(0, "progressKnown", false)
        const row = findChild(ui, "ui.downloads.list").itemAtIndex(0)
        const bar = findChild(row, "ui.downloads.progress.job")
        const fill = findChild(row, "ui.downloads.fill.job")
        tryCompare(bar, "indeterminate", true)
        verify(fill.width > 0 && fill.width < bar.availableWidth / 2)
    }
    function test_active_cancel_requires_confirmation_data() {
        return [{tag: "downloading", state: "downloading"},
            {tag: "resuming", state: "resuming"},
            {tag: "paused", state: "paused"},
            {tag: "finalizing", state: "finalizing"},
            {tag: "verifying", state: "verifying"}]
    }
    function test_active_cancel_requires_confirmation(data) {
        const action = openJob(data.state)
        mouseClick(action)
        const dialog = findChild(ui, "ui.downloads.cancelDialog")
        tryCompare(dialog, "visible", true)
        compare(dialog.jobId, "job")
        compare(downloads.cancelled, "")
        verify(ui.handleEscape())
        tryCompare(dialog, "visible", false)
        compare(downloads.cancelled, "")
        compare(findChild(ui, "ui.downloads.panel").visible, true)
        mouseClick(action)
        tryCompare(dialog, "visible", true)
        dialog.accept()
        compare(downloads.cancelled, "job")
    }
    function test_completed_dismiss_and_panel_close_icons() {
        const action = openJob("completed")
        compare(action.caption, "Dismiss")
        mouseClick(action)
        compare(downloads.count, 0)
        compare(downloads.cancelled, "")
        const close = findChild(ui, "ui.downloads.close")
        mouseClick(close)
        tryCompare(ui, "interactionActive", false)
    }
    function test_restart_for_cancelled_and_failed_data() {
        return ["queued", "downloading", "verifying", "stopping", "completed", "failed", "cancelled"]
            .map(function(state) { return {tag: state, state: state} })
    }
    function test_restart_for_cancelled_and_failed(data) {
        const close = openJob(data.state)
        const list = findChild(ui, "ui.downloads.list")
        const restart = findChild(list.itemAtIndex(0), "ui.downloads.restart.job")
        const restartable = data.state === "cancelled" || data.state === "failed"
        compare(restart.visible, restartable)
        if (restartable) {
            verify(restart.x < close.x)
            mouseClick(restart)
            compare(downloads.restarted, "job")
            compare(downloads.cancelled, "")
        }
    }
    function test_shell_chrome_hide_closes_downloads_data() {
        return [{tag: "panel", confirmation: false},
            {tag: "confirmation", confirmation: true}]
    }
    function test_shell_chrome_hide_closes_downloads(data) {
        const action = openJob("downloading")
        const dialog = findChild(ui, "ui.downloads.cancelDialog")
        if (data.confirmation) {
            mouseClick(action)
            tryCompare(dialog, "visible", true)
        }
        ui.shellChromeVisible = false
        tryCompare(findChild(ui, "ui.downloads.panel"), "visible", false)
        tryCompare(dialog, "visible", false)
        compare(ui.interactionActive, false)
        compare(downloads.cancelled, "")
        compare(downloads.count, 1)
        ui.shellChromeVisible = true
        compare(findChild(ui, "ui.downloads.panel").visible, false)
    }
}
