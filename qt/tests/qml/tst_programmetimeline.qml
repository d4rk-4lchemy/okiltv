import QtQuick
import QtTest
import "../../qml/components"

TestCase {
    name: "ProgrammeTimeline"
    ProgrammeTimelineState { id: timeline }

    function init() {
        timeline.startMs = Date.UTC(2026, 8, 20, 20, 0)
        timeline.endMs = timeline.startMs + 3600000
        timeline.nowMs = timeline.startMs + 2400000
        timeline.archiveEdgeMs = timeline.nowMs - 180000
        timeline.positionMs = timeline.startMs + 900000
    }

    function test_running_programme_and_future_click() {
        fuzzyCompare(timeline.positionFraction, 0.25, 0.00001)
        fuzzyCompare(timeline.archiveFraction, 37 / 60, 0.00001)
        fuzzyCompare(timeline.nowFraction, 40 / 60, 0.00001)
        compare(timeline.timeAt(0.5), timeline.startMs + 1800000)
        verify(!timeline.isFuture(0.5, timeline.nowMs))
        verify(!timeline.isFuture(39 / 60, timeline.nowMs)) // unpublished, not future
        verify(!timeline.isFuture(40 / 60, timeline.nowMs))
        verify(timeline.isFuture(41 / 60, timeline.nowMs))
        verify(timeline.isFuture(1, timeline.nowMs))
    }

    function test_pause_advances_availability_without_moving_position() {
        timeline.nowMs += 60000
        timeline.archiveEdgeMs += 60000
        fuzzyCompare(timeline.nowFraction, 41 / 60, 0.00001)
        fuzzyCompare(timeline.archiveFraction, 38 / 60, 0.00001)
        compare(timeline.positionFraction, 0.25)
        verify(!timeline.isFuture(41 / 60, timeline.nowMs))
    }

    function test_ended_programme_and_publication_delay() {
        timeline.nowMs = timeline.endMs + 60000
        timeline.archiveEdgeMs = timeline.nowMs - 180000
        compare(timeline.nowFraction, 1)
        verify(!timeline.hasFuture)
        verify(!timeline.isFuture(1, timeline.nowMs))
        fuzzyCompare(timeline.archiveFraction, 58 / 60, 0.00001)
        timeline.archiveEdgeMs += 180000
        compare(timeline.archiveFraction, 1)
    }

    function test_epg_change_and_session_origin() {
        timeline.startMs += 600000
        compare(timeline.positionFraction, 0.1)
        compare(timeline.timeAt(0), timeline.startMs)
        compare(timeline.timeAt(1), timeline.endMs)
        timeline.endMs += 3600000
        fuzzyCompare(timeline.positionFraction, 5 / 110, 0.00001)
    }

    function test_invalid_epg() {
        timeline.endMs = timeline.startMs
        verify(!timeline.valid)
        compare(timeline.positionFraction, 0)
        verify(!timeline.isFuture(1, timeline.nowMs))
        timeline.endMs = NaN
        verify(!timeline.hasFuture)
    }
}
