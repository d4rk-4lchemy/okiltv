pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtTest
import "../../qml/components"

TestCase {
    id: testCase
    name: "VodPlaybackPanels"
    width: 900
    height: 700
    visible: true
    when: windowShown
    ListModel {
        id: backend
        property string searchText: ""
        property string errorText: ""
        property bool busy: false
        property bool hasMore: false
        property string playingMovieKey: "movie-0"
        property int playedRow: -1
        property int fetches: 0
        signal changed()
        function requestPoster(row) {}
        function playRow(row) { playedRow = row }
        function fetchMoreMovies() { ++fetches; hasMore = false }
    }
    VodPlaybackList { id: left; width: 340; height: parent.height; catalog: backend; shown: true }
    VodPlaybackInfo {
        id: right
        x: 560; width: 340; height: parent.height; shown: true
        movie: ({title: "Playing movie", description: "Long description ".repeat(100), cast: "Cast ".repeat(100), year: "2024", genres: "Drama", durationMinutes: 120})
    }
    SignalSpy { id: groups; target: left; signalName: "groupsRequested" }
    function init() {
        backend.clear()
        for (let i = 0; i < 20; ++i) backend.append({movieKey: "movie-" + i, title: "Movie " + i, poster: "", available: true})
        backend.playedRow = -1; backend.busy = false; backend.searchText = ""; backend.errorText = ""; backend.hasMore = false; backend.fetches = 0
        left.shown = true; right.shown = true; groups.clear()
        tryCompare(left, "enabled", true)
        findChild(left, "ui.vod.playback.list").positionViewAtBeginning()
        waitForRendering(left)
    }
    function test_selectionRequiresConfirmation() {
        const row = findChild(left, "ui.vod.playback.row.1")
        compare(row.height, 124)
        mouseClick(row)
        compare(backend.playedRow, -1)
        compare(findChild(right, "ui.vod.playback.title").text, "Playing movie")
        keyClick(Qt.Key_Return)
        compare(backend.playedRow, 1)
        backend.playedRow = -1
        mouseDoubleClickSequence(findChild(left, "ui.vod.playback.row.2"))
        compare(backend.playedRow, 2)
    }
    function test_searchFocusAndNavigation() {
        left.focusSearch()
        const search = findChild(left, "ui.vod.playback.search")
        tryCompare(search, "activeFocus", true)
        keyClick(Qt.Key_V); keyClick(Qt.Key_Space); keyClick(Qt.Key_5)
        compare(backend.searchText, "v 5")
        compare(backend.playedRow, -1)
        keyClick(Qt.Key_Down, Qt.ControlModifier)
        verify(left.searchActive)
        verify(!left.listActive)
        keyClick(Qt.Key_Down)
        verify(findChild(left, "ui.vod.playback.list").activeFocus)
        verify(left.listActive)
        verify(!left.searchActive)
        keyClick(Qt.Key_Down); keyClick(Qt.Key_Return)
        compare(backend.playedRow, 1)
    }
    function test_pendingQueryBlocksActivation() {
        left.focusList()
        backend.busy = true
        keyClick(Qt.Key_Return)
        compare(backend.playedRow, -1)
        backend.busy = false
        keyClick(Qt.Key_Return)
        compare(backend.playedRow, 0)
    }
    function test_hideReleasesSearchAndDeferredFocus() {
        left.focusSearch(); verify(left.searchActive)
        left.shown = false
        tryCompare(left, "opacity", 0)
        verify(!left.searchActive)
        verify(!left.listActive)
        left.shown = true; left.focusSearch()
        tryCompare(left, "searchActive", true)
    }
    function test_scrollRequestsNextPageAndGroups() {
        const list = findChild(left, "ui.vod.playback.list")
        backend.hasMore = true
        list.positionViewAtEnd()
        tryCompare(backend, "fetches", 1)
        const button = findChild(left, "ui.vod.playback.openGroups")
        mouseClick(button)
        compare(groups.count, 1)
        compare(backend.playedRow, -1)
    }
}
