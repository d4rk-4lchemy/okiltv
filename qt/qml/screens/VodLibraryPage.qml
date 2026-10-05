pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme/Theme.js" as Theme

FocusScope {
    id: root
    required property var catalog
    readonly property bool seriesLibrary: Boolean(catalog.series)
    readonly property var episodes: seriesLibrary ? catalog.episodesModel : null
    readonly property var playbackChoice: seriesLibrary && episodes ? episodes.selectedEpisode : catalog.movie
    readonly property string allTitle: seriesLibrary ? "All series" : "All movies"
    property int uiTransparency: 100
    property int preferredSidebarWidth: 0
    property real windowWidth: width
    property real navigationTopInset: 0
    property bool resizingSidebar: false
    property real draggedSidebarWidth: 0
    readonly property int maximumSidebarWidth: Math.floor(windowWidth / 3)
    readonly property real sidebarWidth: Math.min(maximumSidebarWidth,
        Math.max(Theme.vodSidebarMinimumWidth, resizingSidebar ? draggedSidebarWidth
            : preferredSidebarWidth > 0 ? preferredSidebarWidth
            : width < 1200 ? Theme.vodSidebarMinimumWidth : Theme.vodSidebarDefaultWidth))
    signal sidebarWidthCommitted(int newWidth)
    onCompactLayoutChanged: { if (compactLayout) resizingSidebar = false }

    readonly property bool detailsOpen: Boolean(catalog.movie.title)
    onDetailsOpenChanged: {
        if (detailsOpen && seriesLibrary) Qt.callLater(root.focusDetailEpisodes)
    }
    function focusDetailEpisodes() {
        if (root.visible && root.enabled && root.detailsOpen && root.seriesLibrary)
            seriesEpisodes.focusEpisodes()
    }
    readonly property bool searchActive: search.activeFocus
    readonly property bool compactLayout: width < 760
    readonly property real maximumPosterWidth: 289.5 * Math.sqrt(0.75)
    readonly property real maximumPosterHeight: 414 * Math.sqrt(0.75)
    readonly property real minimumPosterWidth: 289.5 * 11 / 15
    readonly property real posterWidth: Math.max(minimumPosterWidth, Math.min(maximumPosterWidth,
        (posterViewport.width - (movies.columns - 1) * cardSpacing) / movies.columns))
    readonly property real posterHeight: posterWidth * maximumPosterHeight / maximumPosterWidth
    readonly property int cardSpacing: 16
    readonly property real movieCardHeight: posterHeight + 54
    readonly property bool showContinue: catalog.categoryId === "" && catalog.searchText.length === 0 && catalog.continueMovies.length > 0
    property ListView continueShelf: null
    readonly property var continueItems: catalog.continueMovies
    property bool initialSelectionDone: false
    property bool initialSelectionPending: false
    property bool openingFocusPending: false
    property string browseArea: "grid"
    property int gridIndex: 0
    property int shelfIndex: 0
    property string shelfKey: ""
    property real shelfOffset: 0

    VodMarqueeController {
        id: titleSelection
        objectName: "ui.vod.titleSelection"
        anchors.fill: parent
        ready: root.visible && root.enabled && !root.detailsOpen && !root.resizingSidebar
            && !sourcePicker.popup.visible && !sortPicker.popup.visible && !categoryPicker.popup.visible
        keyboardItem: root.browseArea === "continue" && root.continueShelf && root.continueShelf.activeFocus
            && root.continueShelf.currentItem ? root.continueShelf.currentItem
            : root.browseArea === "grid" && movies.activeFocus && movies.currentItem
                ? movies.currentItem : null
    }

    function cancelInitialSelection() {
        initialSelectionPending = false
        initialSelectionDone = true
    }
    function focusGrid(reveal = true) {
        titleSelection.useKeyboard()
        browseArea = "grid"
        if (continueShelf) continueShelf.focus = false
        movies.currentIndex = movies.count > 0 ? Math.min(gridIndex, movies.count - 1) : -1
        movies.forceActiveFocus()
        if (reveal && movies.currentIndex >= 0) movies.positionViewAtIndex(movies.currentIndex, GridView.Contain)
    }
    function selectShelf(index, reveal = true) {
        if (!continueShelf || !root.showContinue) return
        shelfIndex = Math.max(0, Math.min(index, catalog.continueMovies.length - 1))
        shelfKey = catalog.continueMovies[shelfIndex].movieKey || ""
        continueShelf.currentIndex = shelfIndex
        if (reveal) {
            continueShelf.positionViewAtIndex(shelfIndex, ListView.Contain)
            shelfOffset = continueShelf.contentX
        }
    }
    function focusShelf(reveal = true) {
        titleSelection.useKeyboard()
        if (!continueShelf || !root.showContinue) { focusGrid(); return }
        browseArea = "continue"
        // GridView is a focus scope: a focused header also activates the grid.
        // Clear its current item to prevent it scrolling the header out of view.
        movies.currentIndex = -1
        selectShelf(shelfIndex, reveal)
        continueShelf.forceActiveFocus()
        movies.positionViewAtBeginning()
    }
    function restoreBrowseFocus() {
        if (browseArea === "continue" && root.showContinue) focusShelf(false)
        else focusGrid(false)
    }
    function revealResizedSelection() {
        if (!root.visible || !root.enabled || root.detailsOpen) return
        movies.forceLayout()
        // Reflow can move the selected delegate outside a zero-cache viewport.
        // Keep it instantiated so native keyboard navigation continues to work.
        if (root.browseArea === "grid" && movies.activeFocus && movies.currentIndex >= 0)
            movies.positionViewAtIndex(movies.currentIndex, GridView.Contain)
        else if (root.browseArea === "continue" && continueShelf && continueShelf.activeFocus)
            selectShelf(shelfIndex)
    }
    function openMovie(area, index) {
        cancelInitialSelection()
        browseArea = area
        if (area === "continue") selectShelf(index)
        else { gridIndex = index; movies.currentIndex = index }
        catalog.selectMovie(area === "continue" ? -index - 1 : index)
    }
    function reconcileSelection() {
        if (!root.visible || !root.enabled || root.detailsOpen) return
        if (root.showContinue && continueShelf) {
            let index = catalog.continueMovies.findIndex(item => item.movieKey === shelfKey && shelfKey.length > 0)
            selectShelf(index >= 0 ? index : shelfIndex, false)
            continueShelf.contentX = Math.max(0, Math.min(shelfOffset, continueShelf.contentWidth - continueShelf.width))
        } else if (browseArea === "continue") {
            browseArea = "grid"
            if (!search.activeFocus && !sourcePicker.activeFocus && !categoryPicker.activeFocus && !sortPicker.activeFocus)
                focusGrid(false)
        }
        if (!catalog.busy && browseArea === "grid") {
            gridIndex = movies.count > 0 ? Math.min(gridIndex, movies.count - 1) : 0
            movies.currentIndex = movies.count > 0 ? gridIndex : -1
        }
        if (!initialSelectionPending || !catalog.continueMoviesLoaded) return
        if (root.showContinue && continueShelf && continueShelf.count > 0) {
            cancelInitialSelection()
            shelfIndex = 0
            focusShelf()
        } else if (!catalog.busy) {
            cancelInitialSelection()
            gridIndex = 0
            focusGrid()
        }
    }
    ListModel { id: continueRows; dynamicRoles: true }
    function reconcileContinueRows() {
        if (!continueRows) return
        // Keep delegates alive when progress/artwork or ordering changes.
        for (let i = 0; i < continueItems.length; ++i) {
            const item = continueItems[i]
            let existing = -1
            for (let j = i; j < continueRows.count; ++j) {
                if (continueRows.get(j).modelData.movieKey === item.movieKey) { existing = j; break }
            }
            if (existing < 0) continueRows.insert(i, {modelData: item})
            else {
                if (existing !== i) continueRows.move(existing, i, 1)
                continueRows.setProperty(i, "modelData", item)
            }
        }
        if (continueRows.count > continueItems.length)
            continueRows.remove(continueItems.length, continueRows.count - continueItems.length)
    }
    Component.onCompleted: reconcileContinueRows()
    function rememberShelfIdentity() {
        if (catalog.continueMovies.length === 0) return
        const index = catalog.continueMovies.findIndex(item => item.movieKey === shelfKey && shelfKey.length > 0)
        shelfIndex = index >= 0 ? index : Math.min(shelfIndex, catalog.continueMovies.length - 1)
        shelfKey = catalog.continueMovies[shelfIndex].movieKey || ""
    }
    function chooseCategory(id) {
        cancelInitialSelection()
        browseArea = "grid"
        gridIndex = 0
        catalog.back()
        if (catalog.categoryId !== id) catalog.searchText = ""
        catalog.selectCategory(id)
        movies.positionViewAtBeginning()
    }
    function resumeTimestamp(seconds) {
        const minutes = Math.floor(seconds / 60)
        return String(Math.floor(minutes / 60)).padStart(2, "0") + ":" + String(minutes % 60).padStart(2, "0")
    }
    readonly property string categoryTitle: {
        for (const category of catalog.categories)
            if (category.id === catalog.categoryId) return category.name
        return root.allTitle
    }
    component LibraryButtonBackground: Rectangle {
        required property Button control
        property bool visuallyEnabled: control.enabled
        radius: Theme.radiusS
        color: !visuallyEnabled ? Theme.overlaySurface
            : control.down ? Theme.overlaySurfaceMuted
            : control.hovered ? Theme.overlaySurfaceInteractive
            : control.highlighted ? Theme.overlaySurfaceMuted : Theme.overlaySurfaceRaised
        border.width: 1
        border.color: control.visualFocus ? Theme.overlayTextSecondary : Theme.overlayBorder
    }
    component LibraryButton: AppButton {
        id: libraryButton
        highlighted: accent
        contentItem: Text {
            text: libraryButton.text
            font: libraryButton.font
            color: libraryButton.enabled ? Theme.overlayTextPrimary : Theme.overlayTextMuted
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            renderType: Text.NativeRendering
        }
        background: LibraryButtonBackground { control: libraryButton }
    }
    component LibraryIconButton: IconActionButton {
        id: libraryIconButton
        iconColor: visuallyEnabled ? Theme.overlayTextPrimary : Theme.overlayTextMuted
        background: LibraryButtonBackground { control: libraryIconButton; visuallyEnabled: libraryIconButton.visuallyEnabled }
    }
    component LibraryGlyph: Canvas {
        property string kind: "category"
        property color strokeColor: Theme.overlayTextSecondary
        implicitWidth: 22
        implicitHeight: 22
        onStrokeColorChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            ctx.strokeStyle = strokeColor
            ctx.lineWidth = 1.6
            ctx.lineCap = "round"
            if (kind === "search") {
                ctx.beginPath(); ctx.arc(9, 9, 6.5, 0, Math.PI * 2)
                ctx.moveTo(14, 14); ctx.lineTo(20, 20); ctx.stroke()
            } else if (kind === "refresh") {
                ctx.beginPath(); ctx.arc(11, 11, 8, 0.5, Math.PI * 1.85); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(18, 2); ctx.lineTo(18, 8); ctx.lineTo(12, 8); ctx.stroke()
            } else if (kind === "grid") {
                for (let row = 0; row < 2; ++row)
                    for (let column = 0; column < 2; ++column)
                        ctx.strokeRect(2 + column * 11, 2 + row * 11, 7, 7)
            } else {
                ctx.strokeRect(3, 2, 16, 18)
                for (let row = 0; row < 3; ++row) {
                    ctx.beginPath(); ctx.moveTo(7, 7 + row * 4)
                    ctx.lineTo(15, 7 + row * 4); ctx.stroke()
                }
            }
        }
    }
    component LibraryNav: ItemDelegate {
        id: nav
        property string glyphKind: "category"
        property url iconSource: ""
        implicitHeight: 48
        leftPadding: 14
        rightPadding: 14
        contentItem: RowLayout {
            spacing: 18
            LibraryGlyph { visible: nav.iconSource.toString().length === 0; kind: nav.glyphKind; strokeColor: nav.highlighted ? Theme.overlayTextPrimary : Theme.overlayTextSecondary }
            Image { visible: nav.iconSource.toString().length > 0; source: nav.iconSource; Layout.preferredWidth: 22; Layout.preferredHeight: 22; fillMode: Image.PreserveAspectFit }
            Label {
                Layout.fillWidth: true
                text: nav.text; elide: Text.ElideRight
                color: nav.highlighted ? Theme.overlayTextPrimary : Theme.overlayTextSecondary
                font.pixelSize: 16
            }
        }
        background: Rectangle {
            radius: Theme.radiusS
            color: nav.highlighted ? Theme.overlaySurfaceInteractive : nav.hovered ? Theme.overlaySurfaceRaised : "transparent"
            border.width: nav.visualFocus ? 1 : 0
            border.color: Theme.overlayTextSecondary
        }
        ToolTip.visible: hovered
        ToolTip.delay: 600
        ToolTip.text: text
    }
    component PosterListActions: Item {
        required property string movieKey
        required property bool toWatch
        required property bool favourite
        required property bool listsBusy
        property bool posterHovered: false
        readonly property bool buttonsHovered: watchButton.hovered || favouriteButton.hovered
        readonly property real buttonScale: root.posterWidth / 289.5
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 8 * buttonScale
        anchors.bottomMargin: 12 * buttonScale
        height: 42 * buttonScale
        z: 2
        visible: posterHovered || buttonsHovered
        VodMovieListButton {
            id: watchButton
            objectName: "ui.vod.toWatch." + parent.movieKey
            anchors.left: parent.left
            posterMode: true
            sizeScale: parent.buttonScale
            uiTransparency: root.uiTransparency
            marked: parent.toWatch
            enabled: !parent.listsBusy && !root.catalog.startingPlayback
            retainEnabledAppearance: parent.listsBusy && !root.catalog.startingPlayback
            onClicked: { root.cancelInitialSelection(); root.catalog.toggleToWatch(parent.movieKey) }
        }
        VodMovieListButton {
            id: favouriteButton
            objectName: "ui.vod.favourite." + parent.movieKey
            anchors.right: parent.right
            posterMode: true
            sizeScale: parent.buttonScale
            uiTransparency: root.uiTransparency
            favouriteAction: true
            marked: parent.favourite
            enabled: !parent.listsBusy && !root.catalog.startingPlayback
            retainEnabledAppearance: parent.listsBusy && !root.catalog.startingPlayback
            onClicked: { root.cancelInitialSelection(); root.catalog.toggleFavourite(parent.movieKey) }
        }
    }
    component ResolutionBadge: Rectangle {
        required property string label
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 8
        width: resolutionText.implicitWidth + 12
        height: 22
        radius: 3
        visible: label.length > 0
        color: Theme.vodResolutionColor(label)
        Text {
            id: resolutionText
            anchors.centerIn: parent
            text: parent.label
            color: Theme.vodResolutionText
            font.pixelSize: 11
            font.bold: true
        }
    }
    component LibraryCombo: VodSelector {
        popupHeightLimit: Math.max(height, root.height - height - 24)
        Keys.onPressed: event => { titleSelection.useKeyboard(); root.cancelInitialSelection(); event.accepted = false }
    }
    component CompactLibraryCombo: LibraryCombo {
        implicitHeight: 22
        font.pixelSize: 12
        topPadding: 2
        bottomPadding: 2
    }
    signal closeRequested()

    function closePopups() {
        sourcePicker.popup.close()
        sortPicker.popup.close()
        categoryPicker.popup.close()
        audioBeforePlay.popup.close()
        subtitlesBeforePlay.popup.close()
        if (root.seriesLibrary) seriesEpisodes.closePopup()
    }
    function handleEscape() {
        if (sourcePicker.popup.visible) sourcePicker.popup.close()
        else if (sortPicker.popup.visible) sortPicker.popup.close()
        else if (categoryPicker.popup.visible) categoryPicker.popup.close()
        else if (audioBeforePlay.popup.visible) audioBeforePlay.popup.close()
        else if (subtitlesBeforePlay.popup.visible) subtitlesBeforePlay.popup.close()
        else if (root.seriesLibrary && seriesEpisodes.seasonPopupOpen) seriesEpisodes.closePopup()
        else if (root.detailsOpen) {
            root.catalog.back()
            restoreBrowseFocus()
        } else root.closeRequested()
        return true
    }
    function prepareForOpen() {
        initialSelectionPending = !initialSelectionDone
        openingFocusPending = true
        root.catalog.open()
        activateForOpen()
        posterTimer.restart()
    }
    function activateForOpen() {
        if (!root.visible || !root.enabled || !openingFocusPending) return
        openingFocusPending = false
        if (root.detailsOpen && root.seriesLibrary) root.focusDetailEpisodes()
        else if (!initialSelectionPending && !root.detailsOpen) restoreBrowseFocus()
        else root.forceActiveFocus()
        Qt.callLater(root.reconcileSelection)
    }
    function requestVisiblePosters() {
        if (!root.visible || !movies.visible) return
        if (root.catalog.categoryId === "" && root.catalog.searchText.length === 0) {
            for (let i = 0; i < root.catalog.continueMovies.length; ++i) {
                root.catalog.requestProgress(-i - 1)
                root.catalog.requestResolution(-i - 1)
                root.catalog.requestPoster(-i - 1)
            }
        }
        const firstRow = Math.max(0, Math.floor(movies.contentY / movies.cellHeight))
        const lastRow = Math.ceil((movies.contentY + movies.height) / movies.cellHeight)
        for (let row = firstRow; row <= lastRow; ++row) {
            for (let column = 0; column < movies.columns; ++column) {
                const index = row * movies.columns + column
                if (index < root.catalog.count) {
                    root.catalog.requestProgress(index)
                    root.catalog.requestResolution(index)
                    root.catalog.requestPoster(index)
                }
            }
        }
    }
    Keys.onEscapePressed: event => { event.accepted = root.handleEscape() }
    Shortcut {
        sequence: "Ctrl+F"
        enabled: root.visible && root.enabled && !root.detailsOpen
        onActivated: { titleSelection.useKeyboard(); root.cancelInitialSelection(); search.forceActiveFocus() }
    }
    Timer { id: posterTimer; interval: 80; onTriggered: root.requestVisiblePosters() }
    Connections {
        target: root.catalog
        function onChanged() {
            if (root.visible) {
                posterTimer.restart()
                Qt.callLater(root.reconcileSelection)
            }
        }
    }
    onVisibleChanged: {
        if (!visible) {
            resizingSidebar = false
            openingFocusPending = false
            initialSelectionPending = false
            closePopups()
            posterTimer.stop()
            root.catalog.close()
        }
    }
    onEnabledChanged: { if (enabled) { root.activateForOpen(); Qt.callLater(root.reconcileSelection) } else resizingSidebar = false }
    onShowContinueChanged: Qt.callLater(root.reconcileSelection)
    onContinueItemsChanged: { root.reconcileContinueRows(); root.rememberShelfIdentity(); Qt.callLater(root.reconcileSelection) }
    onContinueShelfChanged: Qt.callLater(root.reconcileSelection)
    TapHandler { onPressedChanged: { if (pressed) root.cancelInitialSelection() } }

    Rectangle { anchors.fill: parent; color: Theme.uiBackground(Theme.vodLibraryBackground, root.uiTransparency) }
    // Consume pointer/wheel events so browsing cannot operate the underlying video.
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: wheel => { wheel.accepted = true } }

    MouseArea {
        id: sidebarResize
        objectName: "ui.vod.sidebarResize"
        z: 10
        x: groupSidebar.width - width / 2
        width: Theme.vodSidebarResizeHandleWidth
        height: parent.height
        visible: !root.compactLayout
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeHorCursor
        preventStealing: true
        property real pressX: 0
        property real initialWidth: 0
        onPressed: mouse => {
            root.cancelInitialSelection()
            pressX = mapToItem(root, mouse.x, mouse.y).x
            initialWidth = root.sidebarWidth
            root.draggedSidebarWidth = initialWidth
            root.resizingSidebar = true
        }
        onPositionChanged: mouse => {
            if (!pressed || !root.resizingSidebar) return
            root.draggedSidebarWidth = Math.max(Theme.vodSidebarMinimumWidth,
                Math.min(root.maximumSidebarWidth, initialWidth + mapToItem(root, mouse.x, mouse.y).x - pressX))
        }
        onReleased: {
            if (!root.resizingSidebar) return
            const newWidth = Math.round(root.sidebarWidth)
            if (newWidth !== Math.round(initialWidth)) root.sidebarWidthCommitted(newWidth)
            root.resizingSidebar = false
        }
        onCanceled: root.resizingSidebar = false
        Rectangle {
            anchors.centerIn: parent
            width: 2
            height: parent.height
            color: Theme.overlayTextSecondary
            visible: sidebarResize.containsMouse || root.resizingSidebar
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            id: groupSidebar
            objectName: "ui.vod.groupSidebar"
            visible: !root.compactLayout
            Layout.minimumWidth: root.sidebarWidth
            Layout.maximumWidth: root.sidebarWidth
            Layout.preferredWidth: root.sidebarWidth
            Layout.fillHeight: true
            color: Theme.uiBackground(Theme.vodLibrarySidebar, root.uiTransparency)
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.overlayBorder; opacity: 0.45 }
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 6
                Label {
                    text: "Library"; color: Theme.overlayTextSecondary
                    font.pixelSize: 20; font.bold: true
                    Layout.leftMargin: 10; Layout.topMargin: 14; Layout.bottomMargin: 10
                }
                LibraryNav {
                    objectName: "ui.vod.all"
                    Layout.fillWidth: true
                    text: root.allTitle; glyphKind: "grid"
                    highlighted: root.catalog.categoryId === ""
                    onClicked: root.chooseCategory("")
                }
                Rectangle {
                    Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10
                    Layout.topMargin: 16; Layout.bottomMargin: 12
                    implicitHeight: 1; color: Theme.overlayBorder; opacity: 0.65
                }
                Label {
                    text: "Categories"; color: Theme.overlayTextSecondary; font.pixelSize: 18; font.bold: true
                    Layout.leftMargin: 10; Layout.bottomMargin: 8
                }
                ListView {
                    id: categories
                    Layout.fillWidth: true; Layout.fillHeight: true
                    clip: true; spacing: 4
                    model: root.catalog.categories
                    ScrollBar.vertical: VodScrollBar {
                        id: categoryScrollBar
                    }
                    delegate: LibraryNav {
                        required property var modelData
                        width: Math.max(0, categories.width - categoryScrollBar.width - Theme.vodScrollBarGap)
                        text: modelData.name
                        iconSource: modelData.iconSource || ""
                        highlighted: root.catalog.categoryId === modelData.id
                        onClicked: root.chooseCategory(modelData.id)
                    }
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumWidth: 0
            Layout.margins: root.compactLayout ? 12 : 20
            Layout.topMargin: (root.compactLayout ? 12 : 20) + root.navigationTopInset
            spacing: root.compactLayout ? 10 : 22
            GridLayout {
                id: toolbar
                Layout.fillWidth: true
                columns: root.compactLayout ? 3 : 5
                columnSpacing: 12; rowSpacing: 8
                Item { visible: !root.compactLayout; Layout.fillWidth: true }
                FormTextField {
                    id: search
                    objectName: "ui.vod.search"
                    Layout.fillWidth: true
                    Layout.columnSpan: root.compactLayout ? 3 : 1
                    Layout.preferredWidth: 356
                    Layout.maximumWidth: root.compactLayout ? Infinity : 356
                    Layout.alignment: Qt.AlignRight
                    Layout.minimumWidth: 100
                    placeholderText: root.seriesLibrary ? "Search series…" : "Search movies…"
                    leftPadding: 48
                    text: root.catalog.searchText
                    enabled: !root.detailsOpen
                    onTextEdited: { titleSelection.useKeyboard(); root.cancelInitialSelection(); root.gridIndex = 0; root.catalog.searchText = text }
                    onActiveFocusChanged: { if (activeFocus) { titleSelection.useKeyboard(); root.cancelInitialSelection() } }
                    background: Rectangle {
                        radius: Theme.radiusS
                        color: Theme.overlaySurfaceRaised
                        border.width: 1
                        border.color: search.activeFocus ? Theme.accent : Theme.overlayBorder
                    }
                    LibraryGlyph { x: 14; anchors.verticalCenter: parent.verticalCenter; kind: "search" }
                    Keys.onDownPressed: root.restoreBrowseFocus()
                    Keys.onReturnPressed: root.restoreBrowseFocus()
                }
                LibraryCombo {
                    id: sourcePicker
                    objectName: "ui.vod.source"
                    Layout.fillWidth: root.compactLayout
                    Layout.preferredWidth: 200
                    Layout.minimumWidth: 100
                    model: root.catalog.sources
                    textRole: "name"
                    displayText: "Source: " + (currentText || "None")
                    currentIndex: {
                        for (let i = 0; i < root.catalog.sources.length; ++i)
                            if (root.catalog.sources[i].id === root.catalog.sourceId) return i
                        return -1
                    }
                    onActivated: index => {
                        root.cancelInitialSelection(); root.browseArea = "grid"; root.gridIndex = 0
                        root.shelfIndex = 0; root.shelfKey = ""; root.shelfOffset = 0
                        root.catalog.selectSource(root.catalog.sources[index].id)
                    }
                }
                LibraryIconButton {
                    objectName: "ui.vod.refresh"
                    compact: true
                    contentItem: Item {
                        LibraryGlyph { anchors.centerIn: parent; kind: "refresh"; visible: !root.catalog.busy }
                    }
                    caption: "Refresh library"; Accessible.name: caption
                    enabled: !root.catalog.busy && root.catalog.sources.length > 0
                    onClicked: root.catalog.refresh()
                    BusyIndicator { anchors.fill: parent; running: root.catalog.busy; visible: running }
                }
                LibraryIconButton {
                    objectName: "ui.vod.close"
                    compact: true; iconName: "windowClose"
                    caption: root.seriesLibrary ? "Close library (B)" : "Close library (V)"; Accessible.name: caption
                    onClicked: root.closeRequested()
                }
            }
            RowLayout {
                Layout.fillWidth: true
                visible: root.catalog.errorText.length > 0
                Label {
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    text: root.catalog.errorText; color: Theme.warning
                    wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight
                    ToolTip.visible: errorHover.hovered; ToolTip.text: text
                    HoverHandler { id: errorHover }
                }
                LibraryButton { text: "Retry"; compact: true; enabled: !root.catalog.busy; onClicked: root.catalog.refresh() }
            }
            Item {
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.minimumHeight: 0
                ColumnLayout {
                    anchors.fill: parent
                    visible: !root.detailsOpen
                    spacing: 14
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            visible: !root.compactLayout
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            text: root.catalog.categoryId === "" ? "" : root.categoryTitle; elide: Text.ElideRight
                            color: Theme.overlayTextPrimary; font.pixelSize: 16; font.bold: true
                        }
                        CompactLibraryCombo {
                            id: categoryPicker
                            objectName: "ui.vod.category"
                            visible: root.compactLayout
                            Layout.fillWidth: true; Layout.minimumWidth: 80
                            model: root.catalog.categories.slice(0, 1).concat([{id: "", name: root.allTitle}], root.catalog.categories.slice(1))
                            textRole: "name"
                            currentIndex: {
                                for (let i = 0; i < model.length; ++i)
                                    if (model[i].id === root.catalog.categoryId) return i
                                return 0
                            }
                            onActivated: index => root.chooseCategory(model[index].id)
                        }
                        CompactLibraryCombo {
                            id: sortPicker
                            objectName: "ui.vod.sort"
                            Layout.preferredWidth: root.compactLayout ? 140 : 196
                            model: ["Title A–Z", "Title Z–A"]
                            displayText: "Sort: " + currentText
                            currentIndex: root.catalog.descending ? 1 : 0
                            onActivated: index => { root.cancelInitialSelection(); root.gridIndex = 0; root.catalog.descending = index === 1 }
                        }
                    }
                    Item {
                        id: moviesViewport
                        Layout.fillWidth: true; Layout.fillHeight: true
                        Layout.minimumHeight: 0
                        Item {
                            id: posterViewport
                            objectName: "ui.vod.posterViewport"
                            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                            width: Math.max(0, moviesViewport.width - movieScrollBar.width - Theme.vodLibraryScrollBarGap)
                            clip: true
                            GridView {
                                id: movies
                                objectName: "ui.vod.grid"
                                // The clipped viewport excludes the scrollbar lane, its gap and the final cell's gap.
                                // A tiny tolerance keeps fractional cells from losing a column to rounding.
                                width: columns * cellWidth + 0.01
                                height: posterViewport.height
                                clip: true
                                readonly property int columns: Math.max(1, Math.floor(
                                    (posterViewport.width + root.cardSpacing) / (root.minimumPosterWidth + root.cardSpacing)))
                                cellWidth: root.posterWidth + root.cardSpacing
                                cellHeight: root.movieCardHeight
                                model: root.catalog
                                currentIndex: -1
                                keyNavigationEnabled: true
                                highlightMoveDuration: 100
                                highlightFollowsCurrentItem: true
                                cacheBuffer: 0
                                ScrollBar.vertical: VodScrollBar {
                                    id: movieScrollBar
                                    objectName: "ui.vod.movieScrollBar"
                                    parent: moviesViewport
                                    anchors.top: parent.top; anchors.bottom: parent.bottom; anchors.right: parent.right
                                }
                                onContentYChanged: {
                                    posterTimer.restart()
                                    if (contentY + height >= contentHeight - cellHeight * 2) root.catalog.fetchMoreMovies()
                                }
                                onCountChanged: {
                                    currentIndex = count > 0 && root.browseArea === "grid" ? Math.min(root.gridIndex, count - 1) : -1
                                    posterTimer.restart()
                                }
                                onWidthChanged: { posterTimer.restart(); Qt.callLater(root.revealResizedSelection) }
                                onHeightChanged: { posterTimer.restart(); Qt.callLater(root.revealResizedSelection) }
                                onCellHeightChanged: { posterTimer.restart(); Qt.callLater(root.revealResizedSelection) }
                                onColumnsChanged: { posterTimer.restart(); Qt.callLater(root.revealResizedSelection) }
                                onActiveFocusChanged: titleSelection.useKeyboard()
                                Keys.onPressed: event => {
                                    titleSelection.useKeyboard()
                                    root.cancelInitialSelection()
                                    if (event.key === Qt.Key_Up && currentIndex < columns && root.showContinue) {
                                        root.gridIndex = Math.max(0, currentIndex)
                                        root.focusShelf()
                                        event.accepted = true
                                    } else if ([Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down, Qt.Key_Home, Qt.Key_End].indexOf(event.key) >= 0) {
                                        Qt.callLater(function() { if (movies.currentIndex >= 0) root.gridIndex = movies.currentIndex })
                                    }
                                }
                                Keys.onReturnPressed: { if (currentIndex >= 0) root.openMovie("grid", currentIndex) }
                                Keys.onEnterPressed: { if (currentIndex >= 0) root.openMovie("grid", currentIndex) }
                                header: Column {
                                    width: posterViewport.width
                                    visible: root.catalog.categoryId === ""
                                    height: root.catalog.categoryId === "" ? implicitHeight + 14 : 0
                                    onHeightChanged: {
                                        if (root.browseArea === "continue" && !root.detailsOpen)
                                            Qt.callLater(function() { movies.positionViewAtBeginning() })
                                    }
                                    spacing: 12
                                    RowLayout {
                                        visible: root.showContinue
                                        width: parent.width
                                        Label { objectName: "ui.vod.continueHeading"; text: "Continue watching"; color: Theme.overlayTextPrimary; font.pixelSize: 20; font.bold: true; Layout.fillWidth: true }
                                        LibraryButton {
                                            text: "View all"; compact: true
                                            onClicked: root.chooseCategory("__continue_watching__")
                                        }
                                    }
                                    ListView {
                                        id: continueShelf
                                        Component.onCompleted: root.continueShelf = continueShelf
                                        objectName: "ui.vod.continue"
                                        visible: root.showContinue
                                        width: parent.width; height: root.movieCardHeight + 10
                                        orientation: ListView.Horizontal; spacing: root.cardSpacing; clip: true
                                        keyNavigationEnabled: false
                                        boundsBehavior: Flickable.StopAtBounds
                                        model: continueRows
                                        onContentXChanged: {
                                            if (dragging || flicking) root.shelfOffset = contentX
                                        }
                                        onWidthChanged: Qt.callLater(root.reconcileSelection)
                                        ScrollBar.horizontal: VodScrollBar {
                                            onPositionChanged: { if (pressed) root.shelfOffset = continueShelf.contentX }
                                        }
                                        onActiveFocusChanged: titleSelection.useKeyboard()
                                        Keys.onPressed: event => {
                                            titleSelection.useKeyboard()
                                            root.cancelInitialSelection()
                                            if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                                                root.selectShelf(currentIndex + (event.key === Qt.Key_Right ? 1 : -1))
                                                event.accepted = true
                                            } else if (event.key === Qt.Key_Down) {
                                                root.focusGrid()
                                                event.accepted = true
                                            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                                                if (currentIndex >= 0) root.openMovie("continue", currentIndex)
                                                event.accepted = true
                                            } else if (event.key === Qt.Key_Up) {
                                                event.accepted = true
                                            }
                                        }
                                        WheelHandler {
                                            target: null
                                            blocking: true
                                            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                                            onWheel: event => {
                                                root.cancelInitialSelection()
                                                const pixels = Math.abs(event.pixelDelta.x) > 0 ? event.pixelDelta.x : event.pixelDelta.y
                                                const angle = Math.abs(event.angleDelta.x) > 0 ? event.angleDelta.x : event.angleDelta.y
                                                const delta = pixels !== 0 ? pixels : angle / 120 * (root.posterWidth + root.cardSpacing)
                                                continueShelf.contentX = Math.max(0, Math.min(continueShelf.contentWidth - continueShelf.width, continueShelf.contentX - delta))
                                                root.shelfOffset = continueShelf.contentX
                                                event.accepted = true
                                            }
                                        }
                                        delegate: ItemDelegate {
                                            id: continueCard
                                            required property int index
                                            required property var modelData
                                            readonly property string marqueeKey: "continue|" + modelData.movieKey
                                            readonly property bool inViewport: x + width > continueShelf.contentX && x < continueShelf.contentX + continueShelf.width
                                                && movies.headerItem && movies.headerItem.y + continueShelf.y + y + height > movies.contentY
                                                && movies.headerItem.y + continueShelf.y + y < movies.contentY + movies.height
                                            HoverHandler {
                                                onHoveredChanged: titleSelection.hoverTarget(continueCard.marqueeKey, hovered)
                                            }
                                            Component.onDestruction: titleSelection.hoverTarget(marqueeKey, false)
                                            width: root.posterWidth; height: root.movieCardHeight; padding: 0
                                            background: Rectangle { color: continueCard.hovered ? Theme.overlaySurfaceInteractive : "transparent"; radius: Theme.radiusS }
                                            Accessible.name: modelData.title
                                            focusPolicy: Qt.NoFocus
                                            onClicked: root.openMovie("continue", index)
                                            contentItem: Column {
                                                spacing: 6
                                                Rectangle {
                                                    width: root.posterWidth; height: root.posterHeight; radius: Theme.radiusS
                                                    color: Theme.overlaySurfaceRaised
                                                    border.width: continueShelf.activeFocus && continueShelf.currentIndex === continueCard.index ? 3 : 1
                                                    border.color: continueShelf.activeFocus && continueShelf.currentIndex === continueCard.index ? Theme.accent : Theme.overlayBorder
                                                    Label { anchors.centerIn: parent; width: parent.width - 12; text: continueCard.modelData.title; wrapMode: Text.WordWrap; maximumLineCount: 4; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter; color: Theme.overlayTextSecondary }
                                                    Image { anchors.fill: parent; anchors.margins: 3; source: continueCard.modelData.poster; asynchronous: true; fillMode: Image.PreserveAspectCrop; sourceSize.width: root.maximumPosterWidth * 2; sourceSize.height: root.maximumPosterHeight * 2 }
                                                    HoverHandler { id: shelfPosterHover }
                                                    PosterListActions {
                                                        id: shelfListActions
                                                        movieKey: continueCard.modelData.movieKey || ""
                                                        toWatch: Boolean(continueCard.modelData.toWatch)
                                                        favourite: Boolean(continueCard.modelData.favourite)
                                                        listsBusy: Boolean(continueCard.modelData.listsBusy)
                                                        posterHovered: shelfPosterHover.hovered
                                                    }
                                                    ResolutionBadge {
                                                        objectName: "ui.vod.continueResolution." + continueCard.index
                                                        label: continueCard.modelData.resolutionLabel || ""
                                                    }
                                                    Rectangle {
                                                        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 3
                                                        height: 4; radius: 2; color: Theme.vodTimelineTrack
                                                        visible: continueCard.modelData.progressFraction > 0
                                                        Rectangle { width: parent.width * continueCard.modelData.progressFraction; height: 4; radius: 2; color: Theme.vodTimelineFill }
                                                    }
                                                }
                                                VodMarqueeTitle {
                                                    objectName: "ui.vod.continueTitle." + continueCard.index
                                                    width: root.posterWidth
                                                    text: continueCard.modelData.title
                                                    contentIdentity: continueCard.marqueeKey
                                                    indicated: continueCard.inViewport && titleSelection.activeTarget === continueCard.marqueeKey
                                                    color: Theme.overlayTextPrimary
                                                    font.pixelSize: 16; font.bold: true
                                                }
                                                Label { width: root.posterWidth; text: continueCard.modelData.year || ""; elide: Text.ElideRight; color: Theme.overlayTextSecondary; font.pixelSize: 14 }
                                            }
                                            ToolTip.visible: hovered && !shelfListActions.buttonsHovered; ToolTip.text: modelData.title
                                        }
                                    }
                                    Label {
                                        objectName: "ui.vod.allHeading"
                                        text: root.allTitle; color: Theme.overlayTextPrimary
                                        font.pixelSize: 22; font.bold: true
                                    }
                                }
                                delegate: Item {
                                    id: card
                                    required property int index
                                    required property string title
                                    required property string year
                                    required property string poster
                                    required property bool available
                                    required property real progressFraction
                                    required property string resolutionLabel
                                    required property string movieKey
                                    required property bool toWatch
                                    required property bool favourite
                                    required property bool listsBusy
                                    readonly property string marqueeKey: "grid|" + movieKey
                                    readonly property bool inViewport: y + height > movies.contentY && y < movies.contentY + movies.height
                                    Component.onDestruction: titleSelection.hoverTarget(marqueeKey, false)
                                    width: movies.cellWidth
                                    height: movies.cellHeight
                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: { movies.forceActiveFocus(); root.openMovie("grid", card.index) }
                                    }
                                    Rectangle {
                                        id: posterFrame
                                        width: root.posterWidth; height: root.posterHeight
                                        radius: 4
                                        color: Theme.overlaySurfaceRaised
                                        border.width: movies.currentIndex === card.index && movies.activeFocus && root.browseArea === "grid" ? 3 : 1
                                        border.color: movies.currentIndex === card.index && movies.activeFocus && root.browseArea === "grid" ? Theme.accent : Theme.overlayBorder
                                        Rectangle {
                                            anchors.fill: parent; anchors.margins: 3; radius: Theme.radiusS
                                            gradient: Gradient {
                                                GradientStop { position: 0; color: Theme.overlaySurfaceMuted }
                                                GradientStop { position: 1; color: Theme.overlaySurface }
                                            }
                                        }
                                        Column {
                                            anchors.centerIn: parent; width: parent.width - 24; spacing: 12
                                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "▸"; color: Theme.overlayTextMuted; font.pixelSize: 36 }
                                            Text { width: parent.width; text: card.title; color: Theme.overlayTextSecondary; font.pixelSize: 14; wrapMode: Text.WordWrap; maximumLineCount: 4; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
                                        }
                                        Image {
                                            anchors.fill: parent; anchors.margins: 3
                                            source: card.poster; asynchronous: true; fillMode: Image.PreserveAspectCrop
                                            sourceSize.width: 480; sourceSize.height: 720
                                            visible: status === Image.Ready
                                        }
                                        HoverHandler { id: posterHover }
                                        PosterListActions {
                                            id: cardListActions
                                            movieKey: card.movieKey
                                            toWatch: card.toWatch
                                            favourite: card.favourite
                                            listsBusy: card.listsBusy
                                            posterHovered: posterHover.hovered
                                        }
                                        ResolutionBadge {
                                            objectName: "ui.vod.resolution." + card.index
                                            label: card.resolutionLabel
                                        }
                                        Rectangle { anchors.fill: parent; color: Theme.accent; opacity: cardHover.hovered ? 0.1 : 0 }
                                        Rectangle {
                                            objectName: "ui.vod.progress." + card.index
                                            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                                            anchors.margins: 3
                                            height: 4
                                            radius: 2
                                            visible: card.progressFraction > 0
                                            color: Theme.vodTimelineTrack
                                            Rectangle {
                                                width: parent.width * Math.max(0, Math.min(1, card.progressFraction))
                                                height: parent.height
                                                radius: parent.radius
                                                color: Theme.vodTimelineFill
                                            }
                                        }
                                    }
                                    VodMarqueeTitle {
                                        objectName: "ui.vod.gridTitle." + card.index
                                        anchors.top: posterFrame.bottom; anchors.topMargin: 9
                                        width: posterFrame.width; text: card.title; color: card.available ? Theme.overlayTextPrimary : Theme.overlayTextMuted
                                        font.pixelSize: 16; font.bold: true
                                        contentIdentity: card.marqueeKey
                                        indicated: card.inViewport && titleSelection.activeTarget === card.marqueeKey
                                    }
                                    Text {
                                        anchors.top: posterFrame.bottom; anchors.topMargin: 29
                                        width: posterFrame.width; elide: Text.ElideRight
                                        text: card.year; color: Theme.overlayTextSecondary; font.pixelSize: 14
                                    }
                                    HoverHandler {
                                        id: cardHover
                                        onHoveredChanged: titleSelection.hoverTarget(card.marqueeKey, hovered)
                                    }
                                    ToolTip.visible: cardHover.hovered && !cardListActions.buttonsHovered
                                    ToolTip.delay: 600
                                    ToolTip.text: card.title
                                }
                                Column {
                                    anchors.centerIn: parent
                                    width: Math.min(parent.width - 32, 420)
                                    spacing: 12
                                    visible: movies.count === 0
                                    BusyIndicator {
                                        objectName: "ui.vod.libraryLoading"
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        running: root.catalog.busy && root.catalog.sources.length > 0
                                        visible: running
                                    }
                                    Label {
                                        width: parent.width
                                        horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                                        color: Theme.overlayTextSecondary; font.pixelSize: 16
                                        text: root.catalog.sources.length === 0 ? "Enable VOD for an Xtream source in Settings to browse " + (root.seriesLibrary ? "series." : "movies.")
                                            : root.catalog.busy ? (root.seriesLibrary ? "Loading your series library…" : "Loading your movie library…")
                                            : root.catalog.errorText.length > 0 ? "Your library is unavailable. Try again."
                                            : root.catalog.searchText.length > 0 ? root.seriesLibrary ? "No series match your search." : "No movies match your search."
                                            : root.seriesLibrary ? "No series in this library." : "No movies in this library."
                                    }
                                }
                            }
                        }
                    }
                    LibraryButton {
                        Layout.alignment: Qt.AlignHCenter
                        text: root.seriesLibrary ? "Load more series" : "Load more movies"; visible: root.catalog.hasMore
                        enabled: !root.catalog.busy
                        onClicked: root.catalog.fetchMoreMovies()
                    }
                }
                Flickable {
                    id: details
                    anchors.fill: parent; visible: root.detailsOpen
                    objectName: "ui.vod.detailsViewport"
                    clip: true; contentHeight: root.seriesLibrary ? height : detailColumn.implicitHeight
                    interactive: !root.seriesLibrary
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: VodScrollBar {
                        id: detailsScrollBar
                        policy: !root.seriesLibrary && size < 1 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                    }
                    ColumnLayout {
                        id: detailColumn
                        width: Math.max(0, details.width - (root.seriesLibrary ? 0 : detailsScrollBar.width + Theme.vodScrollBarGap))
                        height: root.seriesLibrary ? details.height : implicitHeight
                        spacing: 24
                        AppButton {
                            id: detailsBack
                            objectName: "ui.vod.detailsBack"
                            text: "Back"
                            compact: true
                            implicitWidth: contentItem.implicitWidth
                            leftPadding: 0
                            rightPadding: 0
                            background: Item { }
                            contentItem: Row {
                                spacing: 4
                                opacity: detailsBack.enabled ? (detailsBack.down ? 0.7 : 1) : 0.36
                                Image {
                                    width: 20
                                    height: 20
                                    anchors.verticalCenter: parent.verticalCenter
                                    source: "qrc:/resources/icons/left-arrow.svg"
                                    fillMode: Image.PreserveAspectFit
                                }
                                Text {
                                    text: detailsBack.text
                                    font.family: detailsBack.font.family
                                    font.pixelSize: detailsBack.font.pixelSize
                                    font.bold: true
                                    font.underline: detailsBack.hovered || detailsBack.visualFocus
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: Theme.overlayTextPrimary
                                    renderType: Text.NativeRendering
                                }
                            }
                            onClicked: { root.catalog.back(); root.restoreBrowseFocus() }
                        }
                        Flickable {
                            id: detailsInfo
                            objectName: "ui.vod.detailsInfo"
                            Layout.fillWidth: true
                            Layout.minimumHeight: 0
                            Layout.preferredHeight: root.seriesLibrary ? Math.min(infoGrid.implicitHeight,
                                Math.max(0, (details.height - detailsBack.implicitHeight - detailColumn.spacing * 2) / 2)) : infoGrid.implicitHeight
                            contentHeight: infoGrid.implicitHeight
                            contentWidth: width
                            interactive: root.seriesLibrary && contentHeight > height
                            flickableDirection: Flickable.VerticalFlick
                            boundsBehavior: Flickable.StopAtBounds
                            clip: true
                            ScrollBar.vertical: VodScrollBar {
                                id: infoScrollBar
                                policy: root.seriesLibrary && size < 1 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                            }
                            GridLayout {
                                id: infoGrid
                                width: Math.max(0, detailsInfo.width - (root.seriesLibrary ? infoScrollBar.width + Theme.vodScrollBarGap : 0))
                                columns: root.compactLayout ? 1 : 2
                                columnSpacing: 24; rowSpacing: 20
                                Rectangle {
                                    Layout.alignment: Qt.AlignTop
                                    Layout.preferredWidth: root.compactLayout ? 120 : root.width < 1000 ? 140 : 240
                                    Layout.preferredHeight: width * 1.5
                                    radius: Theme.radiusS; color: Theme.overlaySurfaceRaised
                                    Label { anchors.centerIn: parent; text: "▸"; font.pixelSize: 50; color: Theme.overlayTextMuted }
                                    Image { anchors.fill: parent; source: root.catalog.movie.poster || ""; asynchronous: true; fillMode: Image.PreserveAspectCrop }
                                    ResolutionBadge { label: root.catalog.movie.resolutionLabel || "" }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; spacing: 18
                                    Label { Layout.fillWidth: true; text: root.catalog.movie.title || ""; color: Theme.overlayTextPrimary; font.pixelSize: root.width < 1000 ? 24 : 34; font.bold: true; wrapMode: Text.WordWrap }
                                    Label {
                                        Layout.fillWidth: true
                                        text: [root.catalog.movie.year || "", root.catalog.movie.durationMinutes > 0 ? root.catalog.movie.durationMinutes + " min" : "", root.catalog.movie.resolution || "", root.catalog.movie.genres || ""].filter(part => part.length > 0).join("   ·   ")
                                        color: Theme.overlayTextSecondary; font.pixelSize: 13; wrapMode: Text.WordWrap
                                    }
                                    GridLayout {
                                        Layout.fillWidth: true
                                        columns: root.compactLayout ? 1 : 2
                                        columnSpacing: 14; rowSpacing: 10
                                        ColumnLayout {
                                            Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; spacing: 6
                                            Label { text: "AUDIO"; color: Theme.overlayTextMuted; font.pixelSize: 11; font.letterSpacing: 1.5 }
                                            LibraryCombo {
                                                id: audioBeforePlay
                                                objectName: "ui.vod.audioBeforePlay"
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: 0
                                                model: root.catalog.movie.audioTrackOptions || []
                                                textRole: "label"
                                                currentIndex: root.catalog.movie.audioTrackIndex === undefined ? 0 : root.catalog.movie.audioTrackIndex
                                                enabled: Boolean(root.catalog.movie.trackOptionsEditable) && Boolean(root.catalog.movie.progressLoaded)
                                                    && model.length > 1 && !root.catalog.startingPlayback
                                                onActivated: index => root.catalog.selectAudioOption(index)
                                            }
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; spacing: 6
                                            Label { text: "SUBTITLES"; color: Theme.overlayTextMuted; font.pixelSize: 11; font.letterSpacing: 1.5 }
                                            LibraryCombo {
                                                id: subtitlesBeforePlay
                                                objectName: "ui.vod.subtitlesBeforePlay"
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: 0
                                                model: root.catalog.movie.subtitleTrackOptions || []
                                                textRole: "label"
                                                currentIndex: root.catalog.movie.subtitleTrackIndex === undefined ? 0 : root.catalog.movie.subtitleTrackIndex
                                                enabled: Boolean(root.catalog.movie.subtitleOptionsEnabled) && !root.catalog.startingPlayback
                                                optionActionsEnabled: true
                                                onRemoveOption: index => root.catalog.removeSubtitleOption(index)
                                                onActivated: index => {
                                                    root.catalog.selectSubtitleOption(index)
                                                    currentIndex = Qt.binding(() => root.catalog.movie.subtitleTrackIndex === undefined ? 0 : root.catalog.movie.subtitleTrackIndex)
                                                }
                                            }
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        visible: Boolean(root.catalog.movie.subtitleError)
                                        text: root.catalog.movie.subtitleError || ""
                                        color: Theme.overlayTextSecondary
                                        wrapMode: Text.WordWrap
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        visible: Boolean(root.catalog.movie.mediaProbeLoading) || Boolean(root.catalog.movie.mediaProbeError)
                                        text: root.catalog.movie.mediaProbeLoading ? "Reading resolution and media tracks…" : root.catalog.movie.mediaProbeError || ""
                                        color: Theme.overlayTextMuted; font.pixelSize: 12; wrapMode: Text.WordWrap
                                    }
                                    Flow {
                                        Layout.fillWidth: true; spacing: 10
                                        AppButton {
                                            id: playButton
                                            objectName: "ui.vod.play"
                                            accent: true
                                            implicitHeight: 42
                                            readonly property url iconSource: "qrc:/resources/icons/play.svg"
                                            text: root.playbackChoice.resumeSeconds >= 60 ? "Resume · " + root.resumeTimestamp(root.playbackChoice.resumeSeconds) : "Play"
                                            Accessible.name: text
                                            ToolTip.visible: hovered
                                            ToolTip.text: text
                                            contentItem: Row {
                                                spacing: 8
                                                Image {
                                                    width: 20; height: 20
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    source: playButton.iconSource
                                                    fillMode: Image.PreserveAspectFit
                                                    opacity: playButton.visuallyEnabled ? 1 : 0.36
                                                }
                                                Text {
                                                    text: playButton.text
                                                    font: playButton.font
                                                    color: playButton.visuallyEnabled ? Theme.textPrimary : Theme.textMuted
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    renderType: Text.NativeRendering
                                                }
                                            }
                                            enabled: Boolean(root.playbackChoice.available) && !root.catalog.busy && !(root.episodes && root.episodes.busy) && !root.catalog.probePlayBlocked && !Boolean(root.catalog.movie.subtitleBusy)
                                            retainEnabledAppearance: Boolean(root.playbackChoice.available) && Boolean(root.catalog.movie.progressLoaded) && !root.catalog.startingPlayback && !(root.episodes && root.episodes.busy) && !root.catalog.probePlayBlocked && !Boolean(root.catalog.movie.subtitleBusy)
                                            onClicked: root.catalog.play(root.playbackChoice.resumeSeconds < 60)
                                        }
                                        VodMovieListButton {
                                            objectName: "ui.vod.detailsToWatch"
                                            uiTransparency: root.uiTransparency
                                            marked: Boolean(root.catalog.movie.toWatch)
                                            enabled: Boolean(root.catalog.movie.listsLoaded) && !root.catalog.movie.listsBusy && !root.catalog.startingPlayback
                                            retainEnabledAppearance: Boolean(root.catalog.movie.listsLoaded) && Boolean(root.catalog.movie.listsBusy) && !root.catalog.startingPlayback
                                            onClicked: root.catalog.toggleToWatch(root.catalog.movie.movieKey)
                                        }
                                        VodMovieListButton {
                                            objectName: "ui.vod.detailsFavourite"
                                            uiTransparency: root.uiTransparency
                                            favouriteAction: true
                                            marked: Boolean(root.catalog.movie.favourite)
                                            enabled: Boolean(root.catalog.movie.listsLoaded) && !root.catalog.movie.listsBusy && !root.catalog.startingPlayback
                                            retainEnabledAppearance: Boolean(root.catalog.movie.listsLoaded) && Boolean(root.catalog.movie.listsBusy) && !root.catalog.startingPlayback
                                            onClicked: root.catalog.toggleFavourite(root.catalog.movie.movieKey)
                                        }
                                        LibraryIconButton {
                                            objectName: "ui.vod.watched"
                                            compact: true
                                            padding: 9
                                            iconInset: 0
                                            readonly property bool watched: root.seriesLibrary ? Boolean(root.episodes && root.episodes.allWatched) : Boolean(root.playbackChoice.watched)
                                            iconSource: watched ? "qrc:/resources/icons/mark-unwatched.svg" : "qrc:/resources/icons/mark-watched.svg"
                                            caption: root.seriesLibrary ? (watched ? "Mark series as unwatched" : "Mark series as watched")
                                                : (watched ? "Mark as unwatched" : "Mark as watched")
                                            Accessible.name: caption
                                            enabled: root.seriesLibrary ? Boolean(root.episodes && root.episodes.statusReady && !root.episodes.statusBusy)
                                                : Boolean(root.catalog.movie.progressLoaded) && !root.catalog.busy
                                            retainEnabledAppearance: root.seriesLibrary ? Boolean(root.episodes && !root.episodes.busy)
                                                : Boolean(root.catalog.movie.progressLoaded) && !root.catalog.startingPlayback
                                            onClicked: root.catalog.toggleWatched()
                                        }
                                        LibraryIconButton {
                                            objectName: "ui.vod.playFromBeginning"
                                            compact: true
                                            padding: 9
                                            iconInset: 0
                                            iconSource: "qrc:/resources/icons/start-from-beginning.svg"
                                            caption: "Play from beginning"
                                            Accessible.name: caption
                                            visible: root.playbackChoice.resumeSeconds >= 60
                                            enabled: Boolean(root.playbackChoice.available) && !root.catalog.busy && !(root.episodes && root.episodes.busy) && !root.catalog.probePlayBlocked && !Boolean(root.catalog.movie.subtitleBusy)
                                            retainEnabledAppearance: Boolean(root.playbackChoice.available) && Boolean(root.catalog.movie.progressLoaded) && !root.catalog.startingPlayback && !(root.episodes && root.episodes.busy) && !root.catalog.probePlayBlocked && !Boolean(root.catalog.movie.subtitleBusy)
                                            onClicked: root.catalog.play(true)
                                        }
                                    }
                                    Label { Layout.fillWidth: true; text: root.catalog.movie.description || (root.catalog.busy ? "Loading details…" : "No description available."); color: Theme.overlayTextSecondary; font.pixelSize: 15; wrapMode: Text.WordWrap; lineHeight: 1.35 }
                                    Label { text: "CAST"; visible: Boolean(root.catalog.movie.cast); color: Theme.overlayTextMuted; font.pixelSize: 11; font.letterSpacing: 1.5 }
                                    Label { Layout.fillWidth: true; text: root.catalog.movie.cast || ""; visible: text.length > 0; color: Theme.overlayTextSecondary; font.pixelSize: 13; wrapMode: Text.WordWrap }
                                }
                            }
                        }
                        VodEpisodesList {
                            id: seriesEpisodes
                            objectName: "ui.vod.seriesDetails"
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumHeight: 0
                            visible: root.seriesLibrary
                            episodeModel: root.episodes
                            seasonKeyNavigation: true
                            uiTransparency: root.uiTransparency
                        }
                    }
                }
            }
        }
    }
}
