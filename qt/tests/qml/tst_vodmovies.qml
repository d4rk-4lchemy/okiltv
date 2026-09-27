import QtQuick
import QtTest
import "../../qml/screens"

TestCase {
    id: testCase
    name: "VodMovies"
    width: 1200
    height: 800
    visible: true
    when: windowShown
    ListModel {
        id: backend
        property var continueMovies: []
        function toggleWatched() { movie = Object.assign({}, movie, {watched: !movie.watched, resumeSeconds: 0}) }
        property var sources: [{id: "one", name: "Local cinema"}, {id: "two", name: "Second library"}]
        property var categories: [{id: "__continue_watching__", name: "Continue watching"}, {id: "action", name: "Action"}, {id: "drama", name: "Drama"}]
        property string sourceId: "one"
        property string categoryId: ""
        property string searchText: ""
        property bool descending: false
        property bool busy: false
        property bool startingPlayback: false
        property bool hasMore: false
        property string errorText: ""
        property var movie: ({})
        property int selectedIndex: -1
        property int playCount: 0
        property bool playedFromBeginning: false
        signal changed()
        function open() {}
        function close() {}
        function refresh() {}
        function fetchMoreMovies() {}
        function requestPoster(row) {}
        function requestResolution(row) {}
        function requestProgress(row) {}
        function selectSource(id) { sourceId = id }
        function selectCategory(id) { categoryId = id }
        function selectMovie(row) {
            selectedIndex = row
            const item = row < 0 ? continueMovies[-row - 1] : get(row)
            movie = {title: item.title, year: "2024", available: true, description: "A film from the local test library.", genres: "Drama", durationMinutes: 120, resumeSeconds: 65, progressLoaded: true, watched: false}
        }
        function back() { movie = ({}) }
        function play(fromBeginning) { ++playCount; playedFromBeginning = fromBeginning }
    }
    VodMoviesPage { id: page; anchors.fill: parent; catalog: backend }
    SignalSpy { id: closed; target: page; signalName: "closeRequested" }
    readonly property string artifactDirectory: decodeURIComponent(Qt.resolvedUrl("../../../.tmp/00_vod_ui_tests/").toString().replace("file://", ""))
    function init() {
        testCase.width = 1200; testCase.height = 800
        page.visible = true; page.width = 1200
        backend.clear()
        for (let i = 0; i < 80; ++i)
            backend.append({movieKey: "movie-" + i, title: "Movie " + i, year: "2024", poster: "", available: true, progressFraction: 0, resolutionLabel: ""})
        backend.continueMovies = []
        backend.movie = ({}); backend.searchText = ""; backend.busy = false
        backend.errorText = ""; backend.sourceId = "one"; backend.categoryId = ""; backend.selectedIndex = -1; backend.playCount = 0
        closed.clear(); page.prepareForOpen(); wait(50)
    }
    function test_watchedActionAndContinueShelf() {
        findChild(page, "ui.vod.grid").positionViewAtBeginning()
        wait(20)
        backend.continueMovies = [{title: "Started movie", poster: "", progressFraction: 0.5}]
        const shelf = findChild(page, "ui.vod.continue")
        verify(shelf !== null); tryCompare(shelf, "count", 1)
        tryVerify(function() { const y = shelf.mapToItem(page, 0, 0).y; return y >= 0 && y + shelf.height < page.height })
        const heading = findChild(page, "ui.vod.allHeading")
        tryVerify(function() { return heading.mapToItem(page, 0, 0).y >= shelf.mapToItem(page, 0, shelf.height).y })
        const gridPoster = findChild(page, "ui.vod.progress.0").parent
        compare(gridPoster.width, shelf.itemAtIndex(0).width)
        compare(gridPoster.height, page.posterHeight)
        grabImage(page).save(artifactDirectory + "continue-watching.png")
        mouseClick(shelf.itemAtIndex(0))
        compare(backend.selectedIndex, -1); compare(backend.playCount, 0)
        const button = findChild(page, "ui.vod.watched")
        verify(button !== null); compare(button.text, "Mark as watched")
        mouseClick(button); compare(button.text, "Mark as unwatched")
        mouseClick(button); compare(button.text, "Mark as watched")
        backend.continueMovies = []
    }
    function test_resumeTimestampAndMinimum() {
        backend.selectMovie(0)
        const play = findChild(page, "ui.vod.play")
        for (const entry of [[59, "Play movie", true], [60, "Resume · 00:01", false], [1440, "Resume · 00:24", false], [5040, "Resume · 01:24", false]]) {
            backend.movie = Object.assign({}, backend.movie, {resumeSeconds: entry[0]})
            compare(play.text, entry[1])
            mouseClick(play)
            compare(backend.playedFromBeginning, entry[2])
        }
    }
    function test_navigationAndDetails() {
        const grid = findChild(page, "ui.vod.grid")
        verify(grid !== null)
        grid.currentIndex = 0; grid.forceActiveFocus()
        keyClick(Qt.Key_Right)
        compare(grid.currentIndex, 1)
        keyClick(Qt.Key_Return)
        compare(backend.selectedIndex, 1)
        verify(page.detailsOpen)
        const play = findChild(page, "ui.vod.play")
        mouseClick(play)
        compare(backend.playCount, 1)
        page.handleEscape()
        verify(!page.detailsOpen)
        compare(grid.currentIndex, 1)
        compare(closed.count, 0)
        page.handleEscape()
        compare(closed.count, 1)
    }
    function test_searchAcceptsVAndSpace() {
        const search = findChild(page, "ui.vod.search")
        mouseClick(search)
        verify(search.activeFocus)
        keyClick(Qt.Key_V); keyClick(Qt.Key_Space); keyClick(Qt.Key_F)
        compare(backend.searchText, "v f")
        compare(backend.playCount, 0)
        keyClick(Qt.Key_Down)
        verify(findChild(page, "ui.vod.grid").activeFocus)
    }
    function test_popupEscapeAndSourceChoice() {
        const source = findChild(page, "ui.vod.source")
        source.popup.open(); tryCompare(source.popup, "visible", true)
        page.handleEscape(); tryCompare(source.popup, "visible", false)
        compare(closed.count, 0)
        source.forceActiveFocus(); keyClick(Qt.Key_Down)
        compare(backend.sourceId, "two")
    }
    function test_scrollSurvivesDetails() {
        const grid = findChild(page, "ui.vod.grid")
        grid.contentY = grid.cellHeight * 3
        const offset = grid.contentY
        backend.selectMovie(20); wait(20); page.handleEscape(); wait(20)
        compare(grid.contentY, offset)
    }
    function test_appendingPreservesKeyboardSelection() {
        const grid = findChild(page, "ui.vod.grid")
        grid.forceActiveFocus()
        keyClick(Qt.Key_Right)
        keyClick(Qt.Key_Right)
        const selected = grid.currentIndex
        backend.append({movieKey: "more", title: "Another movie", year: "2024", poster: "", available: true, progressFraction: 0, resolutionLabel: ""})
        wait(20)
        compare(grid.currentIndex, selected)
    }
    function test_fractionalColumnWidth() {
        page.width = 1600
        const grid = findChild(page, "ui.vod.grid")
        grid.contentY = 0
        wait(30)
        compare(grid.indexAt(grid.cellWidth * (grid.columns - 0.5), 20), grid.columns - 1)
    }
    function test_resolutionBadges() {
        const labels = ["480p", "720p", "1080p", "1440p", "4K"]
        for (let i = 0; i < labels.length; ++i) backend.setProperty(i, "resolutionLabel", labels[i])
        const colors = []
        for (let i = 0; i < labels.length; ++i) {
            let badge = null
            tryVerify(function() { badge = findChild(page, "ui.vod.resolution." + i); return badge !== null && badge.visible })
            compare(badge.label, labels[i])
            verify(badge.x > badge.parent.width / 2)
            compare(badge.y, 8)
            verify(colors.indexOf(badge.color.toString()) < 0)
            colors.push(badge.color.toString())
        }
        backend.setProperty(0, "resolutionLabel", "")
        tryCompare(findChild(page, "ui.vod.resolution.0"), "visible", false)
        backend.continueMovies = [{title: "Started movie", poster: "", progressFraction: 0.5, resolutionLabel: "4K"}]
        let shelfBadge = null
        tryVerify(function() { shelfBadge = findChild(page, "ui.vod.continueResolution.0"); return shelfBadge !== null && shelfBadge.visible })
        compare(shelfBadge.label, "4K")
        backend.setProperty(0, "resolutionLabel", "480p")
        waitForRendering(page)
        grabImage(page).save(artifactDirectory + "resolution-badges.png")
    }
    function test_posterProgress() {
        const grid = findChild(page, "ui.vod.grid")
        grid.contentY = 0
        const bar = findChild(page, "ui.vod.progress.0")
        verify(bar !== null)
        verify(!bar.visible)
        backend.setProperty(0, "progressFraction", 0.4)
        tryCompare(bar, "visible", true)
        compare(bar.children[0].width, bar.width * 0.4)
        compare(bar.height, 4)
        backend.setProperty(0, "progressFraction", 0)
        tryCompare(bar, "visible", false)
    }
    function test_layout() {
        verify(findChild(page, "ui.vod.grid").width > 500)
        const sizes = [[1600, 900], [1672, 889], [1200, 800], [800, 600], [426, 240]]
        for (const size of sizes) {
            testCase.width = size[0]; testCase.height = size[1]
            page.width = size[0]
            wait(40)
            if (size[0] === 1600) compare(findChild(page, "ui.vod.grid").columns, 6)
            grabImage(page).save(artifactDirectory + "library-" + size[0] + "x" + size[1] + ".png")
            backend.selectMovie(0); wait(30)
            grabImage(page).save(artifactDirectory + "details-" + size[0] + "x" + size[1] + ".png")
            backend.back()
        }
    }
}
