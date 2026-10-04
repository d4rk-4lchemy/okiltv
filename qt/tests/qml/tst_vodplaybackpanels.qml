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
        property bool series: false
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
    QtObject {
        id: episodes
        property var seasons: [{id:"1",name:"Season 1"}]
        property string seasonId: "1"
        property bool busy: false
        property bool statusReady: true
        property bool statusBusy: false
        property string markedKey: ""
        property string errorText: ""
        property int plays: 0
        property int currentIndex: 0
        property var episodes: [{episodeKey:"first",label:"S01E01",title:"First",description:"",poster:"",available:true,selected:true,durationMinutes:20,progressFraction:0.3,resumeSeconds:360,watched:false}]
        signal changed()
        function selectSeason(id) { seasonId=id }
        function selectEpisode(index) {
            currentIndex = index
            episodes = episodes.map((item, i) => Object.assign({}, item, {selected: i === index}))
        }
        function playEpisode(index) { ++plays }
        function toggleEpisodeWatched(key) { markedKey = key }
    }
    function test_seriesRightPanelDirectPlayAndSettings() {
        right.episodeModel=episodes;episodes.plays=0
        wait(50)
        const row=findChild(right,"ui.vod.episode.0");verify(row)
        episodes.markedKey = ""
        mouseClick(findChild(row, "ui.vod.episodeWatched.first"))
        compare(episodes.markedKey, "first"); compare(episodes.plays, 0)
        mouseClick(row, 10, row.height / 2);compare(episodes.plays,1)
        verify(findChild(right,"ui.vod.playback.settingsButton").enabled)
        const list=findChild(right,"ui.vod.episodes");list.forceActiveFocus();keyClick(Qt.Key_Return)
        compare(episodes.plays,2)
        right.episodeModel=null
    }
    function test_episodeMarqueeUsesOnlyIndicatedRow() {
        const longTitle = "A deliberately long episode title that does not fit in the right panel"
        episodes.episodes = [Object.assign({}, episodes.episodes[0], {title: longTitle}),
            Object.assign({}, episodes.episodes[0], {episodeKey: "second", label: "S01E02", title: longTitle + " two", selected: false})]
        right.episodeModel = episodes
        waitForRendering(right)
        let first = findChild(right, "ui.vod.episodeTitle.first")
        let second = findChild(right, "ui.vod.episodeTitle.second")
        verify(first.overflowing && second.overflowing)
        mouseMove(testCase, 450, 650)
        wait(1100)
        verify(!first.scrolling && !second.scrolling, "Visible/playing episodes alone must not scroll")
        right.focusEpisodes()
        wait(600); verify(!first.scrolling)
        tryCompare(first, "scrolling", true, 1000)
        verify(!second.scrolling)
        mouseMove(second, 20, 10)
        tryCompare(first, "indicated", false)
        tryCompare(second, "scrolling", true, 1500)
        // Keyboard ownership survives the row replacement in this fake model.
        keyClick(Qt.Key_Down); keyClick(Qt.Key_Up)
        first = findChild(right, "ui.vod.episodeTitle.first")
        second = findChild(right, "ui.vod.episodeTitle.second")
        verify(first.indicated); verify(!second.indicated)
        const list = findChild(right, "ui.vod.episodes")
        list.contentY = 20; list.contentY = 0
        wait(100); verify(first.indicated); verify(!second.indicated)
        const row = findChild(right, "ui.vod.episode.0")
        const button = findChild(row, "ui.vod.episodeWatched.first")
        verify(first.mapToItem(row, first.width, 0).x <= button.x + 1)
        compare(row.height, 72)
        const plays = episodes.plays
        compare(plays, 0)
        right.shown = false
        verify(!first.scrolling); compare(first.offset, 0)
    }
    function test_seriesEpisodeStatusGeometry_data() {
        return [
            {tag: "wide-unwatched", panelWidth: 340, watched: false},
            {tag: "wide-watched", panelWidth: 340, watched: true},
            {tag: "narrow-unwatched", panelWidth: 240, watched: false},
            {tag: "narrow-watched", panelWidth: 240, watched: true}
        ]
    }
    function test_seriesEpisodeStatusGeometry(data) {
        right.width = data.panelWidth
        episodes.episodes = [Object.assign({}, episodes.episodes[0], {watched: data.watched})]
        right.episodeModel = episodes
        waitForRendering(right)
        const row = findChild(right, "ui.vod.episode.0")
        verify(row)
        const button = findChild(row, "ui.vod.episodeWatched.first")
        const progress = findChild(row, "ui.vod.episodeProgress.first")
        verify(button && progress)
        fuzzyCompare(button.width, 25.2, 0.01)
        fuzzyCompare(button.height, 25.2, 0.01)
        fuzzyCompare(button.contentItem.children[0].width, 14.4, 0.01)
        fuzzyCompare(button.contentItem.children[0].height, 14.4, 0.01)
        const progressEnd = progress.mapToItem(row, progress.width, 0).x
        fuzzyCompare(progressEnd, row.width - 12, 1)
        verify(progressEnd > button.x + button.width / 2)
        compare(button.caption, data.watched ? "Mark episode as unwatched" : "Mark episode as watched")
        episodes.markedKey = ""; episodes.plays = 0
        mouseClick(button)
        compare(episodes.markedKey, "first")
        compare(episodes.plays, 0)
        episodes.statusBusy = true
        verify(!button.enabled)
        compare(button.contentItem.children[0].opacity, 1)
    }
    SignalSpy { id: groups; target: left; signalName: "groupsRequested" }
    function init() {
        right.episodeModel=null; right.width = 340
        episodes.currentIndex = 0
        episodes.statusBusy = false
        episodes.episodes = [{episodeKey:"first",label:"S01E01",title:"First",description:"",poster:"",available:true,selected:true,durationMinutes:20,progressFraction:0.3,resumeSeconds:360,watched:false}]
        backend.clear()
        for (let i = 0; i < 20; ++i) backend.append({movieKey: "movie-" + i, title: "Movie " + i, poster: "", available: true})
        episodes.plays = 0
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
