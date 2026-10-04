import QtQuick
import QtQuick.Controls
import QtTest
import "../../qml/screens"
import "../../qml/components"
import "../../qml/theme/Theme.js" as Theme

TestCase {
    id: testCase
    name: "VodMovies"
    width: 1200
    height: 800
    visible: true
    when: windowShown
    ListModel {
        id: backend
        property bool series: false
        property var episodesModel: episodeBackend
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
        function toggleWatched() {
            if (series) { ++episodeBackend.seriesMarks; episodeBackend.allWatched = !episodeBackend.allWatched }
            else movie = Object.assign({}, movie, {watched: !movie.watched, resumeSeconds: 0})
        }
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
    QtObject {
        id: episodeBackend
        property bool busy: false
        property bool statusReady: true
        property bool statusBusy: false
        property bool allWatched: false
        property int seriesMarks: 0
        property int episodeMarks: 0
        property var rows: null
        property int currentIndex: selectedIndex
        property string errorText: ""
        property string seasonId: "1"
        property var seasons: [{id:"0",name:"Specials"},{id:"1",name:"Season 1"},{id:"3",name:"Season 3"}]
        property var episodes: [{episodeKey:"first",label:"S01E01",title:"First",number:1,description:"Episode plot",poster:"",durationMinutes:20,progressFraction:0.5,resumeSeconds:600,selected:true,available:true,watched:false}]
        property var selectedEpisode: episodes[0]
        property int selectedIndex: 0
        signal changed()
        signal rowsChanging()
        function toggleEpisodeWatched(key) {
            ++episodeMarks
            for (let i = 0; rows && i < rows.count; ++i) {
                const item = rows.get(i).modelData
                if (item.episodeKey === key) rows.setProperty(i, "modelData", Object.assign({}, item, {watched: !item.watched}))
            }
        }
        function selectSeason(id) { seasonId=id; selectedIndex=0; changed() }
        function selectEpisode(index) { selectedIndex=index; changed() }
        function playEpisode(index,fromBeginning) { selectedIndex=index;backend.play(Boolean(fromBeginning)) }
    }
    ListModel { id: stableEpisodes; dynamicRoles: true }
    VodMoviesPage { id: page; anchors.fill: parent; catalog: backend; onSidebarWidthCommitted: newWidth => preferredSidebarWidth = newWidth }
    SignalSpy { id: closed; target: page; signalName: "closeRequested" }
    SignalSpy { id: widthCommitted; target: page; signalName: "sidebarWidthCommitted" }
    Component {
        id: scrollFixture
        Flickable {
            width: 100; height: 100
            contentWidth: 100; contentHeight: 100
            ScrollBar.vertical: VodScrollBar { objectName: "verticalBar" }
            ScrollBar.horizontal: VodScrollBar { objectName: "horizontalBar" }
        }
    }
    function test_scrollbarsOnlyAppearForOverflow_data() {
        return [{tag: "empty", extent: 0, shown: false},
                {tag: "fits", extent: 100, shown: false},
                {tag: "overflows", extent: 101, shown: true}]
    }
    function test_scrollbarsOnlyAppearForOverflow(data) {
        const view = createTemporaryObject(scrollFixture, testCase, {
            contentWidth: data.extent, contentHeight: data.extent
        })
        const vertical = findChild(view, "verticalBar")
        const horizontal = findChild(view, "horizontalBar")
        tryCompare(vertical, "visible", data.shown)
        tryCompare(horizontal, "visible", data.shown)
        const reservedWidth = vertical.width
        const reservedHeight = horizontal.height
        verify(reservedWidth > 0); verify(reservedHeight > 0)
        view.width = 120; view.height = 120
        tryCompare(vertical, "visible", false)
        tryCompare(horizontal, "visible", false)
        compare(vertical.width, reservedWidth); compare(horizontal.height, reservedHeight)
    }
    function test_seriesDetailsKeyboardUsesSelectedEpisodeWithoutTab() {
        backend.series = true
        for (let i = 0; i < 40; ++i)
            stableEpisodes.append({modelData: Object.assign({}, episodeBackend.episodes[0], {
                episodeKey: "episode-" + i, title: "Episode " + i, selected: i === 0
            })})
        episodeBackend.rows = stableEpisodes
        backend.selectMovie(0)
        const list = findChild(page, "ui.vod.episodes")
        tryCompare(list, "activeFocus", true)
        for (let i = 1; i < 15; ++i) {
            keyClick(Qt.Key_Down)
            compare(episodeBackend.selectedIndex, i)
            compare(list.currentIndex, i)
        }
        verify(list.contentY > 0)
        keyClick(Qt.Key_Up)
        compare(episodeBackend.selectedIndex, 13)
        keyClick(Qt.Key_Return)
        compare(backend.playCount, 1)
        compare(episodeBackend.selectedIndex, 13)
        keyClick(Qt.Key_Enter)
        compare(backend.playCount, 2)
        episodeBackend.changed() // Passive history updates retain keyboard focus.
        verify(list.activeFocus)
        for (let i = 0; i < 20; ++i) keyClick(Qt.Key_Up)
        compare(episodeBackend.selectedIndex, 0)
        keyClick(Qt.Key_Return)
        compare(episodeBackend.selectedIndex, 0)
        compare(backend.playCount, 3)
        for (let i = 0; i < 45; ++i) keyClick(Qt.Key_Down)
        compare(episodeBackend.selectedIndex, 39)
        compare(list.currentIndex, 39)
        keyClick(Qt.Key_Return)
        compare(episodeBackend.selectedIndex, 39)
        compare(backend.playCount, 4)
    }
    function test_seriesReturnFocusWaitsForOverlayReadiness() {
        backend.series = true
        page.enabled = false
        page.prepareForOpen()
        backend.selectMovie(0)
        testCase.forceActiveFocus()
        const list = findChild(page, "ui.vod.episodes")
        wait(30)
        verify(!list.activeFocus)
        page.enabled = true
        tryCompare(list, "activeFocus", true)
        keyClick(Qt.Key_Return)
        compare(backend.playCount, 1)
    }
    function test_seriesUsesSharedLibraryAndIndependentEpisodeSelection() {
        backend.series=true
        backend.selectMovie(0)
        wait(50)
        const list=findChild(page,"ui.vod.episodes")
        verify(list);compare(list.model.length,1)
        compare(findChild(page,"ui.vod.season").currentText,"Season 1")
        const play=findChild(page,"ui.vod.play")
        compare(play.text,"Resume · 00:10")
        list.forceActiveFocus();keyClick(Qt.Key_Return)
        compare(backend.playCount,1)
        backend.playCount=0
        mouseClick(findChild(page,"ui.vod.episode.0"))
        compare(backend.playCount,0)
        page.handleEscape();verify(!page.detailsOpen)
    }
    function test_seriesTracksBeforeFirstPlayback() {
        backend.series = true
        backend.selectMovie(0)
        backend.movie = Object.assign({}, backend.movie, {
            mediaProbeReady: true, trackOptionsEditable: true, progressLoaded: true,
            audioTrackOptions: [{label: "Default"}, {label: "Polish"}, {label: "English"}],
            subtitleTrackOptions: [{label: "Default"}, {label: "Off"}, {label: "Polish"}]
        })
        const audio = findChild(page, "ui.vod.audioBeforePlay")
        const subtitles = findChild(page, "ui.vod.subtitlesBeforePlay")
        verify(audio.enabled); verify(subtitles.enabled)
        audio.activated(2); subtitles.activated(2)
        compare(backend.movie.audioTrackIndex, 2)
        compare(backend.movie.subtitleTrackIndex, 2)
        compare(backend.playCount, 0)
        backend.movie = Object.assign({}, backend.movie, {mediaProbeReady: false, trackOptionsEditable: false, mediaProbeLoading: true})
        backend.probePlayBlocked = true
        verify(!audio.enabled); verify(!subtitles.enabled)
        verify(!findChild(page, "ui.vod.play").enabled)
    }
    function test_seriesSeasonKeysClampAndKeepEpisodeFocus() {
        backend.series = true
        backend.selectMovie(0)
        const list = findChild(page, "ui.vod.episodes")
        tryCompare(list, "activeFocus", true)
        episodeBackend.selectedIndex = 4
        keyClick(Qt.Key_Right)
        compare(episodeBackend.seasonId, "3")
        compare(episodeBackend.selectedIndex, 0)
        keyClick(Qt.Key_Right)
        compare(episodeBackend.seasonId, "3")
        keyClick(Qt.Key_Left)
        compare(episodeBackend.seasonId, "1")
        keyClick(Qt.Key_Left)
        compare(episodeBackend.seasonId, "0")
        keyClick(Qt.Key_Left)
        compare(episodeBackend.seasonId, "0")
        verify(list.activeFocus)
        compare(backend.playCount, 0)
        const seasons = episodeBackend.seasons
        episodeBackend.seasons = [{id: "0", name: "Specials"}]
        keyClick(Qt.Key_Right); keyClick(Qt.Key_Left)
        compare(episodeBackend.seasonId, "0")
        episodeBackend.seasons = seasons
    }
    function test_inheritedEpisodeTracksCannotOpenSelectors() {
        backend.series = true
        backend.selectMovie(0)
        backend.movie = Object.assign({}, backend.movie, {
            mediaProbeReady: false, trackOptionsEditable: false, progressLoaded: true,
            audioTrackOptions: [{label: "Default"}, {label: "English"}],
            subtitleTrackOptions: [{label: "Default"}, {label: "Off"}, {label: "Polish"}]
        })
        const audio = findChild(page, "ui.vod.audioBeforePlay")
        const subtitles = findChild(page, "ui.vod.subtitlesBeforePlay")
        compare(audio.count, 2); compare(subtitles.count, 3)
        verify(!audio.enabled); verify(!subtitles.enabled)
        mouseClick(audio); mouseClick(subtitles)
        verify(!audio.popup.visible); verify(!subtitles.popup.visible)
        verify(findChild(page, "ui.vod.play").enabled)
    }
    function test_libraryStorageLoading_data() {
        return [{tag: "movies", series: false}, {tag: "series", series: true}]
    }
    function test_libraryStorageLoading(data) {
        backend.series = data.series
        backend.clear(); backend.busy = true
        const spinner = findChild(page, "ui.vod.libraryLoading")
        tryCompare(spinner, "visible", true); verify(spinner.running)
        compare(backend.errorText, "")
        backend.busy = false; backend.errorText = "VOD storage unavailable after 15 seconds. Try again."
        tryCompare(spinner, "visible", false); verify(!spinner.running)
    }
    function test_seriesDetailsBoundedAndStatusPreservesScroll() {
        backend.series = true
        for (let i = 0; i < 80; ++i) stableEpisodes.append({modelData: {
            episodeKey: "episode-" + i, label: "S01E" + i, title: "Episode " + i,
            description: "Plot", poster: "", durationMinutes: 20, progressFraction: 0.3,
            resumeSeconds: 360, selected: i === 0, available: true, watched: false
        }})
        episodeBackend.rows = stableEpisodes
        backend.selectMovie(0)
        backend.movie = Object.assign({}, backend.movie, {description: "Long description ".repeat(300), cast: "Long cast ".repeat(100)})
        wait(50)
        const viewport = findChild(page, "ui.vod.detailsViewport")
        const info = findChild(page, "ui.vod.detailsInfo")
        const container = findChild(page, "ui.vod.seriesDetails")
        const list = findChild(page, "ui.vod.episodes")
        compare(viewport.interactive, false)
        compare(viewport.contentHeight, viewport.height)
        verify(info.contentHeight > info.height)
        compare(container.mapToItem(viewport, 0, container.height).y, viewport.height)
        list.positionViewAtIndex(35, ListView.Beginning); wait(30)
        const before = list.contentY
        const selection = episodeBackend.selectedIndex
        const row = list.itemAtIndex(35)
        verify(row)
        const button = findChild(row, "ui.vod.episodeWatched.episode-35")
        verify(button && button.enabled)
        list.forceActiveFocus()
        mouseClick(button)
        compare(episodeBackend.episodeMarks, 1)
        compare(episodeBackend.selectedIndex, selection)
        compare(backend.playCount, 0)
        compare(list.activeFocus, true)
        compare(list.contentY, before)
        stableEpisodes.setProperty(35, "modelData", Object.assign({}, stableEpisodes.get(35).modelData, {progressFraction: 0.8, poster: ""}))
        compare(list.contentY, before)
        backend.toggleWatched(); compare(episodeBackend.seriesMarks, 1)
        compare(findChild(page, "ui.vod.watched").caption, "Mark series as unwatched")
        compare(list.contentY, before)
        const seriesButton = findChild(page, "ui.vod.watched")
        info.contentY = Math.max(0, seriesButton.mapToItem(info.contentItem, 0, 0).y - 10); wait(30)
        mouseClick(seriesButton); compare(episodeBackend.seriesMarks, 2)
        compare(list.contentY, before)
        // A catalogue insertion before the viewport preserves the visible identity/offset.
        episodeBackend.rowsChanging()
        stableEpisodes.insert(0, {modelData: Object.assign({}, stableEpisodes.get(0).modelData, {episodeKey: "new-episode", selected: false})})
        episodeBackend.rowsChanged(); wait(30)
        const anchor = list.indexAt(1, list.contentY + 1)
        compare(stableEpisodes.get(anchor).modelData.episodeKey, "episode-35")
        compare(list.itemAtIndex(anchor).y, list.contentY)
        page.width = 700; testCase.width = 700; testCase.height = 520; wait(50)
        verify(info.contentHeight > info.height)
        compare(container.mapToItem(viewport, 0, container.height).y, viewport.height)
        verify(list.height > 0)
        const resizedScroll = list.contentY
        episodeBackend.toggleEpisodeWatched("episode-35")
        compare(list.contentY, resizedScroll)
    }
    function test_movieStatusWriteKeepsIconsAndPlayAppearance() {
        backend.selectMovie(0)
        wait(30)
        mouseMove(page, 0, 0)
        const play = findChild(page, "ui.vod.play")
        const watched = findChild(page, "ui.vod.watched")
        const restart = findChild(page, "ui.vod.playFromBeginning")
        const playColor = play.background.color
        const watchedColor = watched.background.color
        backend.busy = true // The catalogue blocks actions during the status write.
        verify(!play.enabled && !watched.enabled && !restart.enabled)
        compare(play.contentItem.children[0].opacity, 1)
        compare(watched.contentItem.children[0].opacity, 1)
        compare(restart.contentItem.children[0].opacity, 1)
        compare(play.background.color, playColor)
        compare(watched.background.color, watchedColor)
        mouseClick(play)
        mouseClick(watched)
        compare(backend.playCount, 0)
        compare(backend.movie.watched, false)
        backend.movie = Object.assign({}, backend.movie, {watched: true})
        backend.busy = false
        compare(watched.caption, "Mark as unwatched")
        verify(play.enabled && watched.enabled)
        // Actual playback/probe unavailability still has a disabled appearance.
        backend.probePlayBlocked = true
        compare(play.contentItem.children[0].opacity, 0.36)
    }
    function test_seriesStatusWriteKeepsEveryEpisodeIconAppearance() {
        backend.series = true
        episodeBackend.episodes = [episodeBackend.episodes[0], Object.assign({}, episodeBackend.episodes[0], {episodeKey: "second", selected: false})]
        backend.selectMovie(0)
        wait(30)
        mouseMove(page, 0, 0)
        const first = findChild(page, "ui.vod.episodeWatched.first")
        const second = findChild(page, "ui.vod.episodeWatched.second")
        const watched = findChild(page, "ui.vod.watched")
        const play = findChild(page, "ui.vod.play")
        const playColor = play.background.color
        const originalIcon = first.iconSource
        episodeBackend.statusBusy = true
        for (const button of [first, second, watched]) {
            verify(!button.enabled)
            compare(button.contentItem.children[0].opacity, 1)
        }
        mouseClick(first)
        compare(episodeBackend.episodeMarks, 0)
        episodeBackend.statusReady = false // Follow-up history refresh stays blocked.
        episodeBackend.statusBusy = false
        compare(first.contentItem.children[0].opacity, 1)
        compare(second.contentItem.children[0].opacity, 1)
        compare(play.contentItem.children[0].opacity, 1)
        compare(play.background.color, playColor)
        episodeBackend.episodes = [Object.assign({}, episodeBackend.episodes[0], {watched: true}), episodeBackend.episodes[1]]
        episodeBackend.statusReady = true
        wait(30)
        const updatedFirst = findChild(page, "ui.vod.episodeWatched.first")
        const updatedSecond = findChild(page, "ui.vod.episodeWatched.second")
        verify(updatedFirst.iconSource !== originalIcon)
        compare(updatedSecond.iconSource, originalIcon)
        verify(updatedFirst.enabled && updatedSecond.enabled)
    }
    function init() {
        page.resizingSidebar = false; page.preferredSidebarWidth = 0; widthCommitted.clear()
        testCase.width = 1200; testCase.height = 800
        page.enabled = true
        page.visible = true; page.width = 1200
        backend.series=false; episodeBackend.seasonId="1"; episodeBackend.selectedIndex=0
        episodeBackend.rows = null; episodeBackend.allWatched = false; episodeBackend.seriesMarks = 0; episodeBackend.episodeMarks = 0
        episodeBackend.busy = false; episodeBackend.statusReady = true; episodeBackend.statusBusy = false
        episodeBackend.episodes = [{episodeKey:"first",label:"S01E01",title:"First",number:1,description:"Episode plot",poster:"",durationMinutes:20,progressFraction:0.5,resumeSeconds:600,selected:true,available:true,watched:false}]
        stableEpisodes.clear()
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
        verifyPosterLayout()
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
        tryVerify(() => findChild(page, "ui.vod.toWatch.started") !== null)
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
        grabImage(page).save("continue-watching.png")
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
        fuzzyCompare(shelf.contentX, firstOffset, 0.5)
        page.visible = false; page.visible = true; page.prepareForOpen(); wait(30)
        fuzzyCompare(shelf.contentX, firstOffset, 0.5); compare(shelf.currentIndex, selection)
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
        backend.movie = Object.assign({}, backend.movie, {mediaProbeReady: true, trackOptionsEditable: true, audioTrackOptions: options,
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
        backend.movie = Object.assign({}, backend.movie, {mediaProbeLoading: true, mediaProbeReady: false, trackOptionsEditable: false, resumeSeconds: 120})
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
    function test_marqueeGridKeyboardAndRealPointer_data() {
        return [{tag: "movies", series: false}, {tag: "series", series: true}]
    }
    function test_marqueeGridKeyboardAndRealPointer(data) {
        backend.series = data.series
        backend.selectCategory("drama")
        for (let i = 0; i < 2; ++i)
            backend.setProperty(i, "title", "A deliberately long title that cannot fit below this poster " + i)
        mouseMove(page, 30, 60)
        page.gridIndex = 0; page.focusGrid()
        const grid = findChild(page, "ui.vod.grid")
        const first = findChild(page, "ui.vod.gridTitle.0")
        const second = findChild(page, "ui.vod.gridTitle.1")
        verify(first && second)
        verify(first.overflowing)
        wait(600); verify(!first.scrolling)
        tryCompare(first, "scrolling", true, 1000)
        verify(!second.scrolling)
        // A real move takes ownership without opening details or changing selection.
        mouseMove(second, 30, 10)
        tryCompare(first, "indicated", false)
        tryCompare(second, "indicated", true)
        verify(!first.scrolling); compare(first.offset, 0)
        wait(600); verify(!second.scrolling)
        tryCompare(second, "scrolling", true, 1000)
        keyClick(Qt.Key_Right)
        keyClick(Qt.Key_Left)
        tryCompare(first, "indicated", true)
        verify(!second.indicated); verify(!second.scrolling)
        // Force real keyboard scrolling/reflow below the stationary pointer.
        const selection = findChild(page, "ui.vod.titleSelection")
        keyClick(Qt.Key_Down); keyClick(Qt.Key_Down)
        tryVerify(() => grid.contentY > 0)
        waitForRendering(grid)
        compare(selection.inputSource, "keyboard")
        compare(selection.activeTarget, "grid|movie-" + grid.currentIndex)
        keyClick(Qt.Key_Up); keyClick(Qt.Key_Up)
        tryCompare(grid, "currentIndex", 0)
        tryVerify(() => findChild(page, "ui.vod.gridTitle.0") !== null && findChild(page, "ui.vod.gridTitle.1") !== null)
        const restoredFirst = findChild(page, "ui.vod.gridTitle.0")
        const restoredSecond = findChild(page, "ui.vod.gridTitle.1")
        tryCompare(restoredFirst, "indicated", true)
        verify(!restoredSecond.indicated)
        mouseMove(restoredSecond, 32, 10)
        tryCompare(restoredSecond, "indicated", true)
        verify(!restoredFirst.indicated)
        const playCount = backend.playCount
        compare(backend.selectedIndex, -1); compare(playCount, 0)
        page.enabled = false
        verify(!restoredSecond.scrolling); compare(restoredSecond.offset, 0)
    }
    function test_marqueeShelfAndGridShareOneTarget() {
        const title = "A deliberately long continuing title that will not fit below a poster"
        startWithHistory([{movieKey: "movie-0", title: title, poster: "", progressFraction: 0.5}])
        backend.setProperty(0, "title", title)
        const shelf = findChild(page, "ui.vod.continue")
        tryCompare(shelf, "activeFocus", true)
        const caption = findChild(page, "ui.vod.continueTitle.0")
        verify(caption.indicated)
        tryCompare(caption, "scrolling", true, 1500)
        backend.continueMovies = [Object.assign({}, backend.continueMovies[0], {progressFraction: 0.6})]
        wait(50)
        compare(findChild(page, "ui.vod.continueTitle.0"), caption)
        verify(caption.scrolling)
        keyClick(Qt.Key_Down)
        const gridTitle = findChild(page, "ui.vod.gridTitle.0")
        verify(gridTitle.indicated); verify(!caption.indicated)
        verify(!caption.scrolling)
        tryCompare(gridTitle, "scrolling", true, 1500)
        keyClick(Qt.Key_Return)
        verify(page.detailsOpen); verify(!gridTitle.scrolling)
    }
    function test_fractionalColumnWidth() {
        page.width = 1600
        const grid = findChild(page, "ui.vod.grid")
        grid.contentY = 0
        wait(30)
        compare(grid.indexAt(grid.cellWidth * (grid.columns - 0.5), 20), grid.columns - 1)
    }
    function verifyPosterLayout() {
        const grid = findChild(page, "ui.vod.grid")
        const viewport = findChild(page, "ui.vod.posterViewport")
        verify(page.posterWidth >= 289.5 * 11 / 15 - 0.001)
        verify(page.posterWidth <= 289.5 * Math.sqrt(0.75) + 0.001)
        verify(page.posterHeight >= 303.6 - 0.001)
        verify(page.posterHeight <= 414 * Math.sqrt(0.75) + 0.001)
        fuzzyCompare(page.posterHeight / page.posterWidth, 414 / 289.5, 0.00001)
        fuzzyCompare(grid.cellHeight, page.posterHeight + 54, 0.001)
        const occupied = grid.columns * page.posterWidth + (grid.columns - 1) * 16
        if (viewport.width >= 289.5 * 11 / 15)
            verify(occupied <= viewport.width + 0.01, "Last poster must fit before the scrollbar")
        verify((grid.columns + 1) * (289.5 * 11 / 15) + grid.columns * 16 > viewport.width,
               "Another minimum-size poster must not fit")
        fuzzyCompare(grid.cellWidth - page.posterWidth, 16, 0.001)
    }
    function test_responsivePosterThresholds() {
        backend.selectCategory("drama")
        page.preferredSidebarWidth = 208
        wait(30)
        const grid = findChild(page, "ui.vod.grid")
        const viewport = findChild(page, "ui.vod.posterViewport")
        const reserved = testCase.width - viewport.width
        const threshold = 5 * (289.5 * 11 / 15) + 4 * 16
        testCase.width = reserved + threshold - 0.25; wait(30)
        compare(grid.columns, 4); verifyPosterLayout()
        const before = page.posterWidth
        testCase.width = reserved + threshold + 0.25; wait(30)
        compare(grid.columns, 5); verifyPosterLayout()
        // Check actual packing while both rows have instantiated delegates.
        compare(grid.indexAt(4 * grid.cellWidth + page.posterWidth / 2, 20), 4)
        compare(grid.indexAt(page.posterWidth / 2, grid.cellHeight + 20), 5)
        verify(page.posterWidth < before)
        const after = page.posterWidth
        testCase.width += 20; wait(30)
        compare(grid.columns, 5); verifyPosterLayout()
        verify(page.posterWidth > after)
        testCase.width = reserved + threshold - 0.25; wait(30)
        compare(grid.columns, 4); fuzzyCompare(page.posterWidth, before, 0.01)
    }
    function test_responsivePosterLimits() {
        backend.selectCategory("drama")
        const viewport = findChild(page, "ui.vod.posterViewport")
        const scrollbar = findChild(page, "ui.vod.movieScrollBar")
        // Exercise extreme viewports independently of the toolbar's minimum width.
        try {
            viewport.width = 289.5 * Math.sqrt(0.75); wait(30)
            fuzzyCompare(page.posterWidth, 289.5 * Math.sqrt(0.75), 0.01)
            fuzzyCompare(page.posterHeight, 414 * Math.sqrt(0.75), 0.01)
            verifyPosterLayout()
            viewport.width = 289.5 * 11 / 15; wait(30)
            fuzzyCompare(page.posterWidth, 289.5 * 11 / 15, 0.01)
            fuzzyCompare(page.posterHeight, 303.6, 0.01)
            verifyPosterLayout()
            viewport.width = 100; wait(30)
            verifyPosterLayout()
            compare(findChild(page, "ui.vod.grid").columns, 1)
            verify(viewport.clip)
        } finally {
            viewport.width = Qt.binding(() => Math.max(0, viewport.parent.width - scrollbar.width - Theme.vodLibraryScrollBarGap))
        }
    }
    function test_responsivePostersPreserveBrowsing() {
        backend.selectCategory("drama")
        const grid = findChild(page, "ui.vod.grid")
        page.gridIndex = 30; page.focusGrid(); wait(30)
        const widths = [900, 1600, 759, 760, 1200]
        for (const windowWidth of widths) {
            testCase.width = windowWidth; wait(30)
            verifyPosterLayout()
            compare(grid.currentIndex, 30); compare(page.gridIndex, 30)
            verify(grid.activeFocus)
            verify(grid.contentY > 0, "Resizing must not reset browsing to the beginning")
        }
        const posterWidth = page.posterWidth
        const posterHeight = page.posterHeight
        testCase.height = 600; wait(30)
        compare(page.posterWidth, posterWidth); compare(page.posterHeight, posterHeight)
        testCase.width = 1250
        page.preferredSidebarWidth = 208; wait(30)
        const wideViewportPoster = page.posterWidth
        dragSidebarTo(390)
        verifyPosterLayout()
        verify(page.posterWidth !== wideViewportPoster)
        compare(grid.currentIndex, 30); verify(grid.activeFocus)
        keyClick(Qt.Key_Down); compare(grid.currentIndex, 30 + grid.columns)
        keyClick(Qt.Key_Return)
        compare(backend.selectedIndex, grid.currentIndex)
        verify(page.detailsOpen); compare(backend.playCount, 0)
    }
    function test_scrollbarKeepsPosterGeometryStable() {
        const grid = findChild(page, "ui.vod.grid")
        const viewport = findChild(page, "ui.vod.posterViewport")
        const scrollbar = findChild(page, "ui.vod.movieScrollBar")
        verify(scrollbar.size < 1)
        verify(scrollbar.visible)
        const columns = grid.columns
        const viewportWidth = viewport.width
        const posterWidth = page.posterWidth
        backend.clear()
        backend.append({movieKey: "one", title: "One movie", year: "2024", poster: "", available: true,
                        progressFraction: 0, resolutionLabel: "", toWatch: false, favourite: false, listsBusy: false})
        tryCompare(scrollbar, "size", 1)
        tryCompare(scrollbar, "visible", false)
        compare(grid.columns, columns)
        compare(viewport.width, viewportWidth)
        compare(page.posterWidth, posterWidth)
    }
    function test_responsiveContinueShelf() {
        const history = []
        for (let i = 0; i < 12; ++i)
            history.push({movieKey: "started-" + i, title: "A long movie title for responsive posters " + i,
                          year: "2024", poster: "", progressFraction: 0.5, toWatch: false, favourite: false})
        startWithHistory(history)
        const shelf = findChild(page, "ui.vod.continue")
        tryCompare(shelf, "activeFocus", true)
        keyClick(Qt.Key_Right)
        for (const windowWidth of [1600, 900, 759, 426]) {
            testCase.width = windowWidth; wait(30)
            const item = shelf.itemAtIndex(1)
            verify(item !== null)
            fuzzyCompare(item.width, page.posterWidth, 0.001)
            fuzzyCompare(item.height, page.posterHeight + 54, 0.001)
            compare(shelf.currentIndex, 1); verify(shelf.activeFocus)
            compare(page.shelfKey, "started-1")
            const watch = findChild(item, "ui.vod.toWatch.started-1")
            const favourite = findChild(item, "ui.vod.favourite.started-1")
            verify(watch !== null); verify(favourite !== null)
            verify(watch.x + watch.width <= favourite.x)
            verify(watch.parent.y + watch.height <= page.posterHeight)
            grabImage(page).save("continue-responsive-" + windowWidth + ".png")
        }
        keyClick(Qt.Key_Down)
        const grid = findChild(page, "ui.vod.grid")
        verify(grid.activeFocus)
        keyClick(Qt.Key_Up); verify(shelf.activeFocus)
        keyClick(Qt.Key_Return)
        compare(backend.selectedIndex, -2); verify(page.detailsOpen)
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
        grabImage(page).save("resolution-badges.png")
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
            verifyPosterLayout()
            grabImage(page).save("library-" + size[0] + "x" + size[1] + ".png")
            backend.selectMovie(0); wait(30)
            grabImage(page).save("details-" + size[0] + "x" + size[1] + ".png")
            backend.back()
        }
    }
}
