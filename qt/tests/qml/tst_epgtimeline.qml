import QtQuick
import QtTest
import "../../qml/components"

TestCase {
    id: testCase
    name: "EpgTimeline"
    width: 360
    height: 600
    when: windowShown
    visible: true

    property double origin: Date.now()
    QtObject {
        id: epg
        property var channel: ({ id: 1, profileId: "profile", catchupSupported: true, catchupWindowHours: 168 })
        property var pastPrograms: []
        property var currentProgram: ({})
        property var nextProgram: ({})
        property var upcomingPrograms: []
        property bool loading: false
        signal dataChanged()
    }
    QtObject {
        id: app
        signal catchupProgressChanged()
        function catchupActionState(channel, program) { return { enabled: true } }
    }
    QtObject {
        id: dvr
        property int scheduledCount: 0
        function isProgramScheduled(channel, program) { return false }
    }
    EpgTimeline {
        id: panel
        anchors.fill: parent
        catchupIconSource: Qt.resolvedUrl("../../resources/icons/catch-up-indicator.svg")
        continueCatchupIconSource: Qt.resolvedUrl("../../resources/icons/continue-catch-up.svg")
        epgModel: epg
        appController: app
        dvrController: dvr
        onProgramHovered: function(program, anchor) {
            if (catchupActive && keyboardSelectionActive)
                selectedKey = programKey(program)
        }
    }
    SignalSpy { id: selected; target: panel; signalName: "selectionRequested" }
    SignalSpy { id: activated; target: panel; signalName: "activationRequested" }

    function program(index) {
        return { channelId: "channel", title: "Programme " + index,
            start: new Date(origin + index * 3600000).toISOString(),
            stop: new Date(origin + (index + 1) * 3600000).toISOString(),
            startTimeLabel: "12:00", timeRange: "12:00 - 13:00", progressPercent: 50 }
    }
    function init() {
        mouseMove(testCase, -10, -10)
        panel.keyboardSelectionActive = false
        panel.selectedKey = ""
        panel.catchupActive = false
        panel.playbackProgram = ({})
        panel.playbackChannel = ({})
        panel.visible = true
        origin = Date.now() - 1800000
        epg.channel = { id: 1, profileId: "profile", catchupSupported: true, catchupWindowHours: 168 }
        const past = []
        for (let i = -48; i < 0; ++i)
            past.push(program(i))
        epg.pastPrograms = past
        epg.currentProgram = program(0)
        epg.nextProgram = program(1)
        const future = []
        for (let i = 2; i < 24; ++i)
            future.push(program(i))
        epg.upcomingPrograms = future
        panel.followingNow = true
        epg.dataChanged()
        tryCompare(panel, "updating", false)
        compare(panel.entries.length, 72)
        panel.goToNow()
        selected.clear()
        activated.clear()
    }
    function watch(index) {
        panel.playbackChannel = epg.channel
        panel.playbackProgram = program(index)
        panel.catchupActive = true
        tryVerify(function() { return panel.centeredPlaybackKey === panel.playbackKey })
    }
    function verifyPlaybackCentered() {
        const item = panel.itemAt(panel.playbackIndex)
        verify(item !== null)
        verify(item.playing)
        verify(item.card)
        compare(findChild(item, "ui.epg.cardBadge").text, "CATCH-UP")
        verify(Math.abs(item.mapToItem(panel, 0, item.height / 2).y - panel.height / 2) < 2)
    }
    function test_catchup_centers_and_marks_watched_programme() {
        watch(-20)
        verifyPlaybackCentered()
        compare(panel.preferredIndex, panel.indexForProgram(program(-20)))
        compare(panel.followingNow, false)
        panel.playbackProgram = program(-19)
        tryVerify(function() { return panel.centeredPlaybackKey === panel.playbackKey })
        verifyPlaybackCentered()
        panel.playbackProgram = program(-21)
        tryVerify(function() { return panel.centeredPlaybackKey === panel.playbackKey })
        verifyPlaybackCentered()
    }
    function test_catchup_has_one_movable_selection() {
        watch(-20)
        const watched = panel.itemAt(panel.playbackIndex)
        verify(watched.active)
        const otherIndex = panel.playbackIndex + 1
        const other = panel.itemAt(otherIndex)
        verify(other !== null)
        mouseMove(other, other.width / 2, other.height - 15)
        tryVerify(function() { return other.active && !watched.active })
        verify(watched.card)
        verify(!other.card)
        compare(panel.preferredIndex, otherIndex)
        panel.selectedKey = panel.entries[otherIndex + 1].key
        panel.keyboardSelectionActive = true
        const keyboardRow = panel.itemAt(otherIndex + 1)
        verify(keyboardRow.active)
        verify(!other.active)
        verify(!watched.active)
        // A passive progress/EPG refresh must not restore the playback highlight.
        const updated = program(-20)
        updated.progressPercent = 75
        panel.playbackProgram = updated
        epg.dataChanged()
        tryCompare(panel, "updating", false)
        compare(panel.catchupSelectionKey, panel.selectedKey)
        verify(!panel.itemAt(panel.playbackIndex).active)
        verify(panel.itemAt(panel.playbackIndex).card)
        mouseMove(panel.itemAt(otherIndex), 100, panel.itemAt(otherIndex).height - 10)
        tryCompare(panel, "catchupSelectionKey", panel.entries[otherIndex].key)
        verify(!panel.itemAt(otherIndex + 1).active)
        compare(panel.selectedKey, panel.catchupSelectionKey)
        verify(panel.pointerSelectionActive)
        const browsedKey = panel.catchupSelectionKey
        panel.playbackProgram = program(-22)
        tryVerify(function() { return panel.centeredPlaybackKey === panel.playbackKey })
        compare(panel.catchupSelectionKey, browsedKey)
        verify(!panel.itemAt(panel.playbackIndex).active)
    }
    function test_catchup_refresh_and_progress_preserve_manual_scroll() {
        watch(-20)
        panel.showIndex(10)
        const before = panel.itemAt(10).mapToItem(panel, 0, 0).y
        const updated = program(-20)
        updated.progressPercent = 60
        panel.playbackProgram = updated
        epg.dataChanged()
        tryCompare(panel, "updating", false)
        verify(Math.abs(panel.itemAt(10).mapToItem(panel, 0, 0).y - before) < 2)
        panel.visible = false
        panel.visible = true
        tryVerify(function() {
            const item = panel.itemAt(panel.playbackIndex)
            return item !== null && Math.abs(item.mapToItem(panel, 0, item.height / 2).y - panel.height / 2) < 2
        })
    }
    function test_catchup_card_uses_media_progress_including_zero() {
        watch(-20)
        const watched = panel.itemAt(panel.playbackIndex)
        const track = findChild(watched, "ui.epg.progressTrack")
        const fill = findChild(watched, "ui.epg.progressFill")
        const height = watched.height
        for (const progress of [0, 23, 100]) {
            const updated = program(-20)
            updated.progressPercent = progress
            panel.playbackProgram = updated
            verify(track.visible)
            tryVerify(function() { return Math.abs(fill.width - track.width * progress / 100) < 0.01 })
            compare(watched.height, height)
            verifyPlaybackCentered()
        }
    }
    function test_catchup_demotes_live_cards_and_restores_them_on_exit() {
        watch(-2)
        panel.showIndex(panel.defaultIndex + 1)
        const now = panel.itemAt(panel.defaultIndex)
        const next = panel.itemAt(panel.defaultIndex + 1)
        verify(!now.card)
        verify(!next.card)
        verify(findChild(now, "ui.epg.rowNow").visible)
        verify(!findChild(next, "ui.epg.rowNow").visible)
        verify(!findChild(next, "ui.epg.cardBadge").visible)
        panel.catchupActive = false
        tryCompare(panel, "followingNow", true)
        verify(now.card)
        verify(next.card)
        compare(findChild(now, "ui.epg.cardBadge").text, "NOW")
        compare(findChild(next, "ui.epg.cardBadge").text, "NEXT")
        verify(!findChild(now, "ui.epg.rowNow").visible)
    }
    function test_live_end_keeps_watched_card_and_moves_only_now_marker() {
        watch(0)
        const key = panel.playbackKey
        const before = panel.itemAt(panel.playbackIndex).mapToItem(panel, 0, 0).y
        verify(findChild(panel.itemAt(panel.playbackIndex), "ui.epg.cardNow").visible)
        epg.pastPrograms = epg.pastPrograms.concat([epg.currentProgram])
        epg.currentProgram = epg.nextProgram
        epg.nextProgram = epg.upcomingPrograms[0]
        epg.upcomingPrograms = epg.upcomingPrograms.slice(1)
        epg.dataChanged()
        tryCompare(panel, "updating", false)
        compare(panel.playbackKey, key)
        const watched = panel.itemAt(panel.playbackIndex)
        verify(watched.card)
        verify(watched.past)
        compare(findChild(watched, "ui.epg.cardBadge").text, "CATCH-UP")
        verify(!findChild(watched, "ui.epg.cardNow").visible)
        verify(Math.abs(watched.mapToItem(panel, 0, 0).y - before) < 2)
        const now = panel.itemAt(panel.defaultIndex)
        verify(!now.card)
        verify(findChild(now, "ui.epg.rowNow").visible)
        panel.playbackProgram = program(1)
        tryVerify(function() { return panel.centeredPlaybackKey === panel.playbackKey })
        verifyPlaybackCentered()
        verify(findChild(panel.itemAt(panel.playbackIndex), "ui.epg.cardNow").visible)
        verify(!panel.itemAt(panel.indexForProgram(program(0))).card)
    }
    function test_catchup_restart_recenters_during_refresh() {
        watch(-20)
        panel.showIndex(10)
        epg.dataChanged()
        panel.centerPlayback(true)
        tryCompare(panel, "updating", false)
        verifyPlaybackCentered()
    }
    function test_catchup_waits_for_async_epg_and_handles_missing_programme() {
        epg.pastPrograms = []
        epg.dataChanged()
        tryCompare(panel, "updating", false)
        panel.playbackChannel = epg.channel
        panel.playbackProgram = program(-20)
        panel.catchupActive = true
        compare(panel.playbackIndex, -1)
        const past = []
        for (let i = -48; i < 0; ++i)
            past.push(program(i))
        epg.pastPrograms = past
        epg.dataChanged()
        tryCompare(panel, "updating", false)
        verifyPlaybackCentered()
        panel.playbackProgram = ({})
        compare(panel.playbackIndex, -1)
        verify(!panel.itemAt(panel.indexForProgram(program(-20))).playing)
    }
    function test_catchup_channel_identity_and_return_to_live() {
        watch(-20)
        panel.playbackChannel = { id: 1, profileId: "other" }
        compare(panel.playbackIndex, -1)
        panel.playbackChannel = { id: 2, profileId: "profile" }
        compare(panel.playbackIndex, -1)
        panel.playbackChannel = epg.channel
        tryVerify(function() { return panel.centeredPlaybackKey === panel.playbackKey })
        verifyPlaybackCentered()
        panel.catchupActive = false
        tryCompare(panel, "followingNow", true)
        compare(panel.playbackIndex, -1)
        compare(panel.preferredIndex, panel.defaultIndex)
        verify(Math.abs(panel.itemAt(panel.defaultIndex).mapToItem(panel, 0, 0).y) < 2)
    }

    function test_default_and_history() {
        compare(panel.entries[panel.defaultIndex].kind, "now")
        compare(panel.defaultIndex, 48)
        const now = panel.itemAt(panel.defaultIndex)
        verify(now !== null)
        verify(Math.abs(now.mapToItem(panel, 0, 0).y) < 2)
        panel.showIndex(42)
        tryCompare(panel, "followingNow", false)
        verify(panel.itemAt(42) !== null)
        compare(panel.entries[42].kind, "past")
        panel.goToNow()
        compare(panel.followingNow, true)
    }
    function test_refresh_preserves_history_position() {
        panel.showIndex(30)
        tryCompare(panel, "followingNow", false)
        const before = panel.itemAt(30).mapToItem(panel, 0, 0).y
        const retained = panel.entries[30].program
        epg.pastPrograms = epg.pastPrograms.slice(2)
        epg.dataChanged()
        tryCompare(panel, "updating", false)
        const index = panel.indexForProgram(retained)
        compare(index, 28)
        verify(Math.abs(panel.itemAt(index).mapToItem(panel, 0, 0).y - before) < 2)
        compare(panel.followingNow, false)
    }
    function test_repeated_refresh_preserves_anchor() {
        panel.showIndex(30)
        tryCompare(panel, "followingNow", false)
        const before = panel.itemAt(30).mapToItem(panel, 0, 0).y
        const retained = panel.entries[30].program
        epg.pastPrograms = epg.pastPrograms.slice(2)
        epg.dataChanged()
        epg.dataChanged()
        tryCompare(panel, "updating", false)
        const item = panel.itemAt(panel.indexForProgram(retained))
        verify(item !== null)
        verify(Math.abs(item.mapToItem(panel, 0, 0).y - before) < 2)
    }
    function test_now_becomes_history() {
        const current = epg.currentProgram
        epg.pastPrograms = epg.pastPrograms.concat([current])
        epg.currentProgram = epg.nextProgram
        epg.nextProgram = epg.upcomingPrograms[0]
        epg.upcomingPrograms = epg.upcomingPrograms.slice(1)
        epg.dataChanged()
        tryCompare(panel, "updating", false)
        compare(panel.defaultIndex, 49)
        compare(panel.entries[48].kind, "past")
        compare(panel.followingNow, true)
        verify(Math.abs(panel.itemAt(49).mapToItem(panel, 0, 0).y) < 2)
    }
    function test_channel_change_resets_to_now() {
        panel.showIndex(5)
        tryCompare(panel, "followingNow", false)
        epg.channel = { id: 2, profileId: "other", catchupSupported: true, catchupWindowHours: 168 }
        epg.channelChanged()
        // Cached channel data must also be positioned without another event-loop turn.
        compare(panel.updating, false)
        compare(panel.followingNow, true)
        verify(panel.itemAt(panel.defaultIndex) !== null)
        verify(Math.abs(panel.itemAt(panel.defaultIndex).mapToItem(panel, 0, 0).y) < 2)
    }
    function test_async_channel_history_is_positioned_before_next_frame() {
        // Mirror NowNextModel: clear the old channel, change identity, then deliver EPG.
        const past = epg.pastPrograms
        const current = epg.currentProgram
        const next = epg.nextProgram
        const upcoming = epg.upcomingPrograms
        panel.showIndex(5)
        epg.pastPrograms = []
        epg.currentProgram = ({})
        epg.nextProgram = ({})
        epg.upcomingPrograms = []
        epg.dataChanged()
        epg.channel = { id: 2, profileId: "profile", catchupSupported: true, catchupWindowHours: 168 }
        tryCompare(panel, "updating", false)

        epg.pastPrograms = past
        epg.currentProgram = current
        epg.nextProgram = next
        epg.upcomingPrograms = upcoming
        epg.dataChanged()
        // No wait/tryCompare here: the first renderable layout must already show NOW.
        const now = panel.itemAt(panel.defaultIndex)
        verify(now !== null, "NOW must exist before yielding to rendering")
        verify(Math.abs(now.mapToItem(panel, 0, 0).y) < 2, "First layout must start at NOW")
        compare(panel.followingNow, true)
    }

    function test_fallbacks_and_empty() {
        epg.currentProgram = ({})
        epg.dataChanged()
        tryCompare(panel, "updating", false)
        compare(panel.entries[panel.defaultIndex].kind, "next")
        epg.nextProgram = ({})
        epg.upcomingPrograms = []
        epg.dataChanged()
        tryCompare(panel, "updating", false)
        compare(panel.entries[panel.defaultIndex].kind, "past")
        compare(panel.defaultIndex, 47)
        epg.pastPrograms = []
        epg.dataChanged()
        tryCompare(panel, "updating", false)
        compare(panel.entries.length, 0)
        compare(panel.defaultIndex, -1)
    }
    function test_click_and_double_click() {
        panel.showIndex(40)
        const row = panel.itemAt(40)
        verify(row !== null)
        mouseClick(row, 100, row.height - 20)
        compare(selected.count, 1)
        compare(activated.count, 0)
        mouseDoubleClickSequence(row, 100, row.height - 20)
        compare(activated.count, 1)
        compare(activated.signalArguments[0][0], 40)
    }
    function test_duplicates_and_dates() {
        epg.pastPrograms = epg.pastPrograms.concat([epg.pastPrograms[47]])
        epg.dataChanged()
        tryCompare(panel, "updating", false)
        compare(panel.entries.length, 72)
        let dates = 0
        for (let i = 0; i < panel.entries.length; ++i) {
            if (panel.entries[i].dateLabel.length > 0) {
                verify(/^(Monday|Tuesday|Wednesday|Thursday|Friday|Saturday|Sunday) \d{2}\.\d{2}$/.test(panel.entries[i].dateLabel))
                ++dates
            }
        }
        verify(dates >= 3)
    }
}
