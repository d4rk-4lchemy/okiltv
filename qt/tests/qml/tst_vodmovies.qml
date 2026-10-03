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
        property bool continueMoviesLoaded: true
        function toggleToWatch(key) { toggleList(key, "toWatch") }
        function toggleFavourite(key) { toggleList(key, "favourite") }
        function toggleList(key, flag) {
            for (let row = 0; row < count; ++row)
                if (get(row).movieKey === key) setProperty(row, flag, !get(row)[flag])
            continueMovies = continueMovies.map(item => item.movieKey === key ? Object.assign({}, item, {[flag]: !item[flag]}) : item)
            if (movie.movieKey === key) movie = Object.assign({}, movie, {[flag]: !movie[flag]})
            changed()
        }
        function toggleWatched() { movie = Object.assign({}, movie, {watched: !movie.watched, resumeSeconds: 0}) }
        property var sources: [{id: "one", name: "Local cinema"}, {id: "two", name: "Second library"}]
        property var categories: [{id: "__continue_watching__", name: "Continue watching", iconSource: "qrc:/resources/icons/play.svg"}, {id: "__to_watch__", name: "To Watch", iconSource: "qrc:/resources/icons/bookmark.svg"}, {id: "__favourites__", name: "Favourites", iconSource: "qrc:/resources/icons/favourites.svg"}, {id: "action", name: "Action"}, {id: "drama", name: "Drama"}]
        property string sourceId: "one"
        property string categoryId: ""
        property string searchText: ""
        property bool descending: false
        property bool busy: false
        property bool startingPlayback: false
        property bool probePlayBlocked: false
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
        function selectAudioOption(index) { movie = Object.assign({}, movie, {audioTrackIndex: index}) }
        function selectSubtitleOption(index) { movie = Object.assign({}, movie, {subtitleTrackIndex: index}) }
        function selectMovie(row) {
            selectedIndex = row
            const item = row < 0 ? continueMovies[-row - 1] : get(row)
            movie = {movieKey: item.movieKey, toWatch: Boolean(item.toWatch), favourite: Boolean(item.favourite), listsLoaded: true, listsBusy: false, title: item.title, year: "2024", available: true, description: "A film from the local test library.", genres: "Drama", durationMinutes: 120, resumeSeconds: 65, progressLoaded: true, watched: false}
        }
        function back() { movie = ({}) }
        function play(fromBeginning) { ++playCount; playedFromBeginning = fromBeginning }
    }
    VodMoviesPage { id: page; anchors.fill: parent; catalog: backend; onSidebarWidthCommitted: newWidth => preferredSidebarWidth = newWidth }
    SignalSpy { id: closed; target: page; signalName: "closeRequested" }
    readonly property string artifactDirectory: decodeURIComponent(Qt.resolvedUrl("../../../.tmp/00_vod_ui_tests/").toString().replace("file://", ""))
    SignalSpy { id: widthCommitted; target: page; signalName: "sidebarWidthCommitted" }
    function init() {
        page.resizingSidebar = false; page.preferredSidebarWidth = 0; widthCommitted.clear()
        testCase.width = 1200; testCase.height = 800
        page.enabled = true
        page.visible = true; page.width = 1200
        backend.clear()
        for (let i = 0; i < 80; ++i)
            backend.append({movieKey: "movie-" + i, title: "Movie " + i, year: "2024", poster: "", available: true, progressFraction: 0, resolutionLabel: "", toWatch: false, favourite: false, listsBusy: false})
        backend.continueMovies = []; backend.continueMoviesLoaded = true
        backend.movie = ({}); backend.searchText = ""; backend.busy = false
        backend.probePlayBlocked = false; backend.startingPlayback = false
        backend.errorText = ""; backend.sourceId = "one"; backend.categoryId = ""; backend.selectedIndex = -1; backend.playCount = 0
        page.initialSelectionDone = false; page.initialSelectionPending = false
        page.browseArea = "grid"; page.gridIndex = 0; page.shelfIndex = 0; page.shelfKey = ""; page.shelfOffset = 0
        closed.clear(); page.prepareForOpen(); wait(50)
    }
    function dragSidebarTo(targetX) {
        const handle = findChild(page, "ui.vod.sidebarResize")
        mousePress(handle, handle.width / 2, 100)
        mouseMove(page, targetX, 100, 30)
        mouseRelease(page, targetX, 100)
        wait(30)
    }
    function test_sidebarResizeAndLimits() {
        const sidebar = findChild(page, "ui.vod.groupSidebar")
        compare(sidebar.width, 268)
        dragSidebarTo(340)
        compare(page.preferredSidebarWidth, 340); compare(sidebar.width, 340)
        compare(widthCommitted.count, 1)
        dragSidebarTo(700)
        compare(sidebar.width, 400); compare(page.preferredSidebarWidth, 400)
        dragSidebarTo(50)
        compare(sidebar.width, 208); compare(page.preferredSidebarWidth, 208)
        compare(backend.categoryId, ""); compare(backend.playCount, 0)
        compare(page.posterWidth, 199); compare(page.posterHeight, 282)
    }
    function test_sidebarWindowResizeAndReopen() {
        page.preferredSidebarWidth = 390
        testCase.width = 900; wait(30)
        compare(page.sidebarWidth, 300); compare(page.preferredSidebarWidth, 390)
        testCase.width = 759; wait(30)
        verify(!findChild(page, "ui.vod.sidebarResize").visible)
        verify(findChild(page, "ui.vod.category").visible)
        testCase.width = 1200; wait(30)
        compare(page.sidebarWidth, 390)
        page.visible = false; page.visible = true; wait(30)
        compare(page.sidebarWidth, 390); compare(widthCommitted.count, 0)
    }
    function test_sidebarMaximumKeepsControlsInsideWindow() {
        page.preferredSidebarWidth = 1000
        backend.selectCategory("drama")
        backend.selectMovie(0)
        for (const windowWidth of [760, 800, 1200, 1600]) {
            testCase.width = windowWidth; wait(30)
            const sidebar = findChild(page, "ui.vod.groupSidebar")
            compare(sidebar.width, Math.floor(windowWidth / 3))
            for (const name of ["ui.vod.source", "ui.vod.search", "ui.vod.close"]) {
                const control = findChild(page, name)
                const position = control.mapToItem(page, 0, 0)
                verify(position.x >= sidebar.width, name)
                verify(position.x + control.width <= page.width + 1, name)
            }
        }
        compare(backend.categoryId, "drama"); compare(backend.movie.title, "Movie 0")
        compare(widthCommitted.count, 0)
    }
    function test_sidebarCanceledAndDisabled() {
        const handle = findChild(page, "ui.vod.sidebarResize")
        mousePress(handle, handle.width / 2, 100)
        mouseMove(page, 350, 100, 30)
        page.enabled = false
        mouseRelease(page, 350, 100)
        compare(page.preferredSidebarWidth, 0); compare(widthCommitted.count, 0)
        compare(page.sidebarWidth, 268)
        dragSidebarTo(380)
        compare(widthCommitted.count, 0)
        page.enabled = true
        mousePress(handle, handle.width / 2, 100)
        mouseMove(page, 350, 100, 30)
        testCase.width = 700; wait(30)
        mouseRelease(page, 350, 100)
        compare(widthCommitted.count, 0); compare(page.preferredSidebarWidth, 0)
    }
    function test_movieListButtonsOnPostersAndDetails() {
        const grid = findChild(page, "ui.vod.grid")
        const watch = findChild(page, "ui.vod.toWatch.movie-0")
        const favourite = findChild(page, "ui.vod.favourite.movie-0")
        verify(watch !== null); verify(favourite !== null)
        mouseMove(grid, grid.width - 20, grid.height - 20)
        tryCompare(watch, "visible", false)
        mouseMove(watch.parent.parent, 20, 30)
        tryCompare(watch, "visible", true)
        verify(watch.x < favourite.x)
        verify(watch.y + watch.height <= page.posterHeight)
        compare(watch.caption, "Add to plan to watch")
        compare(favourite.caption, "Add to favourites")
        mouseClick(watch)
        tryCompare(watch, "marked", true)
        compare(watch.caption, "Remove from plan to watch")
        verify(!page.detailsOpen); compare(backend.playCount, 0)
        mouseClick(favourite)
        tryCompare(favourite, "marked", true)
        compare(favourite.caption, "Remove from favourites")
        verify(!page.detailsOpen); compare(backend.playCount, 0)
        backend.selectMovie(0)
        const detailsWatch = findChild(page, "ui.vod.detailsToWatch")
        const detailsFavourite = findChild(page, "ui.vod.detailsFavourite")
        verify(detailsWatch.marked); verify(detailsFavourite.marked)
        compare(detailsWatch.width, 42); compare(detailsWatch.height, 42)
        mouseClick(detailsWatch); verify(!detailsWatch.marked)
        verify(detailsFavourite.marked)
        mouseClick(detailsFavourite); verify(!detailsFavourite.marked)
        backend.movie = Object.assign({}, backend.movie, {listsBusy: true})
        verify(!detailsWatch.enabled); verify(!detailsFavourite.enabled)
    }
    function test_movieListButtonsOnContinueShelf() {
        startWithHistory([{movieKey: "started", title: "Started movie", poster: "", progressFraction: 0.5, toWatch: false, favourite: false}])
        const watch = findChild(page, "ui.vod.toWatch.started")
        const favourite = findChild(page, "ui.vod.favourite.started")
        verify(watch !== null); verify(favourite !== null)
        const shelf = findChild(page, "ui.vod.continue")
        tryCompare(shelf, "count", 1); wait(50)
        mouseMove(watch.parent.parent, 20, 30)
        tryCompare(watch, "visible", true)
        mouseClick(watch)
        tryVerify(() => findChild(page, "ui.vod.toWatch.started").marked)
        verify(!page.detailsOpen); compare(backend.playCount, 0)
        const updatedFavourite = findChild(page, "ui.vod.favourite.started")
        mouseMove(updatedFavourite.parent.parent, 20, 30)
        tryCompare(updatedFavourite, "visible", true)
        mouseClick(updatedFavourite)
        tryVerify(() => findChild(page, "ui.vod.favourite.started").marked)
        verify(!page.detailsOpen); compare(backend.playCount, 0)
    }
    function test_focusWaitsForOverlayReadiness() {
        const grid = findChild(page, "ui.vod.grid")
        page.visible = false
        page.enabled = false
        page.visible = true
        page.initialSelectionDone = false
        backend.continueMoviesLoaded = false
        page.prepareForOpen()
        testCase.forceActiveFocus()
        backend.continueMovies = [{movieKey: "recent", title: "Recent", poster: "", progressFraction: 0.5}]
        backend.continueMoviesLoaded = true
        backend.changed()
        wait(30)
        verify(testCase.activeFocus)
        verify(!grid.activeFocus)
        const shelf = findChild(page, "ui.vod.continue")
        verify(shelf !== null)
        verify(!shelf.activeFocus)
        page.enabled = true
        tryCompare(shelf, "activeFocus", true)
        // Closing keeps the page rendered but rejects delayed model focus changes.
        page.enabled = false
        testCase.forceActiveFocus()
        backend.continueMovies = []
        backend.changed()
        wait(30)
        verify(testCase.activeFocus)
        verify(!grid.activeFocus)
        page.visible = false
        verify(!page.openingFocusPending)
    }
    function test_watchedActionAndContinueShelf() {
        startWithHistory([{movieKey: "started", title: "Started movie", poster: "", progressFraction: 0.5}])
        const shelf = findChild(page, "ui.vod.continue")
        verify(shelf !== null); tryCompare(shelf, "count", 1)
        tryCompare(shelf, "activeFocus", true); wait(30)
        tryVerify(function() { const y = shelf.mapToItem(page, 0, 0).y; return y >= 0 && y + shelf.height < page.height })
        const heading = findChild(page, "ui.vod.allHeading")
        tryVerify(function() { return heading.mapToItem(page, 0, 0).y >= shelf.mapToItem(page, 0, shelf.height).y })
        const gridPoster = findChild(page, "ui.vod.progress.0").parent
        compare(gridPoster.width, shelf.itemAtIndex(0).width)
        compare(gridPoster.height, page.posterHeight)
        grabImage(page).save(artifactDirectory + "continue-watching.png")
        mouseClick(shelf.itemAtIndex(0))
        wait(30)
        verify(page.detailsOpen)
        compare(backend.selectedIndex, -1); compare(backend.playCount, 0)
        const button = findChild(page, "ui.vod.watched")
        verify(button !== null); compare(button.caption, "Mark as watched")
        verify(button.iconSource.toString().endsWith("mark-watched.svg"))
        mouseClick(button); compare(button.caption, "Mark as unwatched")
        verify(button.iconSource.toString().endsWith("mark-unwatched.svg"))
        mouseClick(button); compare(button.caption, "Mark as watched")
        compare(button.width, 42); compare(button.height, 42)
        backend.continueMovies = []
    }
    function test_resumeTimestampAndMinimum() {
        backend.selectMovie(0)
        const play = findChild(page, "ui.vod.play")
        verify(play.iconSource.toString().endsWith("play.svg"))
        verify(play.accent)
        verify(play.width > 42); compare(play.height, 42)
        const beginning = findChild(page, "ui.vod.playFromBeginning")
        compare(beginning.caption, "Play from beginning")
        verify(beginning.iconSource.toString().endsWith("start-from-beginning.svg"))
        for (const entry of [[59, "Play", true], [60, "Resume · 00:01", false], [1440, "Resume · 00:24", false], [5040, "Resume · 01:24", false]]) {
            backend.movie = Object.assign({}, backend.movie, {resumeSeconds: entry[0]})
            compare(play.text, entry[1])
            compare(beginning.visible, entry[0] >= 60)
            mouseClick(play)
            compare(backend.playedFromBeginning, entry[2])
        }
    }
    function shelfMovies(count) {
        const rows = []
        for (let i = 0; i < count; ++i)
            rows.push({movieKey: "started-" + i, title: "Started movie " + i, poster: "", progressFraction: 0.5})
        return rows
    }
    function startWithHistory(rows, loaded = true) {
        backend.continueMoviesLoaded = loaded
        backend.continueMovies = rows
        page.initialSelectionDone = false
        page.prepareForOpen()
        backend.changed()
    }
    function test_firstOpenWaitsForHistory() {
        backend.busy = true
        startWithHistory([], false)
        verify(page.initialSelectionPending)
        backend.continueMovies = shelfMovies(8)
        backend.busy = false
        backend.continueMoviesLoaded = true
        backend.changed()
        const shelf = page.continueShelf
        tryCompare(shelf, "activeFocus", true)
        compare(shelf.currentIndex, 0)
        verify(!page.detailsOpen)
        compare(backend.playCount, 0)
        keyClick(Qt.Key_Right); keyClick(Qt.Key_Right)
        compare(shelf.currentIndex, 2)
        page.visible = false; page.visible = true; page.prepareForOpen()
        tryCompare(shelf, "activeFocus", true)
        compare(shelf.currentIndex, 2)
    }
    function test_historyFallbackAndInteraction() {
        startWithHistory([], false)
        const search = findChild(page, "ui.vod.search")
        search.forceActiveFocus()
        backend.continueMovies = shelfMovies(3)
        backend.continueMoviesLoaded = true; backend.changed()
        wait(30)
        verify(search.activeFocus)
        verify(!page.initialSelectionPending)
        backend.continueMovies = []
        backend.errorText = "History unavailable"
        startWithHistory([])
        tryCompare(findChild(page, "ui.vod.grid"), "activeFocus", true)
        compare(findChild(page, "ui.vod.grid").currentIndex, 0)
        backend.clear(); startWithHistory([])
        tryVerify(function() { return !page.initialSelectionPending })
        compare(findChild(page, "ui.vod.grid").currentIndex, -1)
    }
    function test_shelfKeyboardAndDetailsReturn() {
        startWithHistory(shelfMovies(8))
        const shelf = page.continueShelf
        const grid = findChild(page, "ui.vod.grid")
        tryCompare(shelf, "activeFocus", true)
        for (let i = 0; i < 6; ++i) keyClick(Qt.Key_Right)
        compare(shelf.currentIndex, 6)
        verify(shelf.contentX > 0)
        keyClick(Qt.Key_Down)
        verify(grid.activeFocus); verify(!shelf.activeFocus); compare(grid.currentIndex, 0)
        keyClick(Qt.Key_Right); wait(10)
        keyClick(Qt.Key_Up)
        verify(shelf.activeFocus); compare(shelf.currentIndex, 6)
        keyClick(Qt.Key_Down)
        verify(grid.activeFocus); compare(grid.currentIndex, 1)
        keyClick(Qt.Key_Up); keyClick(Qt.Key_Return)
        compare(backend.selectedIndex, -7); verify(page.detailsOpen)
        compare(backend.playCount, 0)
        page.handleEscape()
        verify(shelf.activeFocus); compare(shelf.currentIndex, 6)
        keyClick(Qt.Key_Right); keyClick(Qt.Key_Right)
        compare(shelf.currentIndex, 7)
        backend.continueMovies = []
        tryCompare(grid, "activeFocus", true)
        keyClick(Qt.Key_Up)
        verify(grid.activeFocus)
    }
    function test_shelfSelectionFollowsIdentity() {
        const rows = shelfMovies(5)
        startWithHistory(rows)
        const shelf = page.continueShelf
        tryCompare(shelf, "activeFocus", true)
        keyClick(Qt.Key_Right); keyClick(Qt.Key_Right)
        backend.continueMovies = [rows[2], rows[0], rows[1], rows[3], rows[4]]
        tryCompare(shelf, "currentIndex", 0)
        compare(page.shelfKey, "started-2")
        backend.continueMovies = [rows[0], rows[1]]
        tryCompare(page, "shelfKey", "started-0")
        compare(shelf.currentIndex, 0)
    }
    function test_shelfWheelStaysHorizontal() {
        startWithHistory(shelfMovies(12))
        const shelf = page.continueShelf
        const grid = findChild(page, "ui.vod.grid")
        tryCompare(shelf, "activeFocus", true)
        wait(30)
        const verticalOffset = grid.contentY
        const selection = shelf.currentIndex
        mouseWheel(shelf, 100, 100, 0, -120)
        tryVerify(function() { return shelf.contentX > 0 })
        compare(grid.contentY, verticalOffset); compare(shelf.currentIndex, selection)
        const firstOffset = shelf.contentX
        backend.continueMovies = backend.continueMovies.map(item => Object.assign({}, item, {resolutionLabel: "720p"}))
        wait(30)
        compare(shelf.contentX, firstOffset)
        page.visible = false; page.visible = true; page.prepareForOpen(); wait(30)
        compare(shelf.contentX, firstOffset); compare(shelf.currentIndex, selection)
        mouseWheel(shelf, 100, 100, -120, 0)
        tryVerify(function() { return shelf.contentX > firstOffset })
        mouseWheel(shelf, 100, 100, 0, 1200)
        tryCompare(shelf, "contentX", 0)
        compare(grid.contentY, verticalOffset); compare(shelf.currentIndex, selection)
    }
    function checkComboPopup(combo) {
        combo.popup.open()
        tryCompare(combo.popup, "visible", true)
        compare(combo.popup.width, combo.width)
        const list = combo.popup.contentItem
        tryVerify(function() { return list.itemAtIndex(0) !== null })
        const option = list.itemAtIndex(0)
        compare(option.height, combo.height)
        compare(option.width, combo.popup.availableWidth)
        compare(option.contentItem.font.pixelSize, combo.font.pixelSize)
        combo.popup.close()
    }
    function test_dropdownGeometryAndOptions() {
        checkComboPopup(findChild(page, "ui.vod.source"))
        checkComboPopup(findChild(page, "ui.vod.sort"))
        testCase.width = 700; page.width = 700; wait(30)
        checkComboPopup(findChild(page, "ui.vod.category"))
        backend.selectMovie(0)
        const options = [{label: "Default"}, {label: "Track with a very long description that must never resize a dropdown"}, {label: "Off"}]
        backend.movie = Object.assign({}, backend.movie, {mediaProbeReady: true, audioTrackOptions: options,
            subtitleTrackOptions: options, audioTrackIndex: 0, subtitleTrackIndex: 0})
        for (const size of [1200, 700]) {
            testCase.width = size; page.width = size; wait(30)
            const audio = findChild(page, "ui.vod.audioBeforePlay")
            const subtitles = findChild(page, "ui.vod.subtitlesBeforePlay")
            compare(audio.width, subtitles.width)
            const widths = [audio.width, subtitles.width]
            const positions = [audio.x, subtitles.x]
            checkComboPopup(audio); checkComboPopup(subtitles)
            audio.popup.open()
            tryCompare(audio.popup, "visible", true)
            tryVerify(function() { return audio.popup.contentItem.itemAtIndex(1) !== null })
            mouseClick(audio.popup.contentItem.itemAtIndex(1))
            compare(backend.movie.audioTrackIndex, 1)
            tryCompare(audio.popup, "visible", false)
            subtitles.forceActiveFocus(); keyClick(Qt.Key_Down)
            compare(backend.movie.subtitleTrackIndex, 1)
            wait(20)
            compare(audio.width, widths[0]); compare(subtitles.width, widths[1])
            compare(audio.x, positions[0]); compare(subtitles.x, positions[1])
            backend.movie = Object.assign({}, backend.movie, {audioTrackIndex: 0, subtitleTrackIndex: 0})
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
    function test_playWhileReadingTracks() {
        backend.selectMovie(0)
        backend.movie = Object.assign({}, backend.movie, {mediaProbeLoading: true, mediaProbeReady: false, resumeSeconds: 120})
        const play = findChild(page, "ui.vod.play")
        const beginning = findChild(page, "ui.vod.playFromBeginning")
        backend.probePlayBlocked = true
        verify(!play.enabled)
        verify(!beginning.enabled)
        mouseClick(play)
        compare(backend.playCount, 0)
        backend.probePlayBlocked = false
        verify(play.enabled)
        verify(beginning.enabled)
        mouseClick(play)
        compare(backend.playCount, 1)
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
        backend.append({movieKey: "more", title: "Another movie", year: "2024", poster: "", available: true, progressFraction: 0, resolutionLabel: "", toWatch: false, favourite: false, listsBusy: false})
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
