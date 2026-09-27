pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../theme/Theme.js" as Theme

FocusScope {
    id: root
    required property var catalog
    property int uiTransparency: 100
    readonly property bool detailsOpen: Boolean(catalog.movie.title)
    readonly property bool searchActive: search.activeFocus
    readonly property bool compactLayout: width < 760
    readonly property int posterWidth: 199
    readonly property int posterHeight: 282
    readonly property int cardSpacing: 16
    readonly property int movieCardHeight: posterHeight + 54
    readonly property bool showContinue: catalog.categoryId === "" && catalog.searchText.length === 0 && catalog.continueMovies.length > 0
    function resumeTimestamp(seconds) {
        const minutes = Math.floor(seconds / 60)
        return String(Math.floor(minutes / 60)).padStart(2, "0") + ":" + String(minutes % 60).padStart(2, "0")
    }
    readonly property string categoryTitle: {
        for (const category of catalog.categories)
            if (category.id === catalog.categoryId) return category.name
        return "All movies"
    }
    component LibraryGlyph: Canvas {
        property string kind: "category"
        property color strokeColor: Theme.textSecondary
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
        implicitHeight: 48
        leftPadding: 14
        rightPadding: 14
        contentItem: RowLayout {
            spacing: 18
            LibraryGlyph { kind: nav.glyphKind; strokeColor: nav.highlighted ? Theme.accent : Theme.textSecondary }
            Label {
                Layout.fillWidth: true
                text: nav.text; elide: Text.ElideRight
                color: nav.highlighted ? Theme.accent : Theme.textSecondary
                font.pixelSize: 16
            }
        }
        background: Rectangle {
            radius: Theme.radiusS
            color: nav.highlighted ? Theme.surfaceInteractive : nav.hovered ? Theme.surfaceRaised : "transparent"
            border.width: nav.visualFocus ? 1 : 0
            border.color: Theme.accent
        }
        ToolTip.visible: hovered
        ToolTip.delay: 600
        ToolTip.text: text
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
    component LibraryCombo: ComboBox {
        id: combo
        implicitHeight: 44
        font.pixelSize: 14
        leftPadding: 14
        rightPadding: 30
        contentItem: Text {
            text: combo.displayText
            font: combo.font
            color: combo.enabled ? Theme.textSecondary : Theme.textMuted
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: Theme.radiusS
            color: combo.hovered ? Theme.surfaceInteractive : Theme.surfaceRaised
            border.width: 1
            border.color: combo.visualFocus ? Theme.accent : Theme.border
        }
        palette.button: Theme.surfaceRaised
        palette.buttonText: Theme.textPrimary
        palette.base: Theme.surface
        palette.text: Theme.textPrimary
        palette.window: Theme.surface
        palette.windowText: Theme.textPrimary
        palette.highlight: Theme.accentMuted
        palette.highlightedText: Theme.textPrimary
    }
    component CompactLibraryCombo: LibraryCombo {
        id: compactCombo
        implicitHeight: 22
        font.pixelSize: 12
        topPadding: 2
        bottomPadding: 2
        delegate: ItemDelegate {
            required property int index
            width: compactCombo.popup.width
            text: compactCombo.textAt(index)
            font: compactCombo.font
            highlighted: compactCombo.highlightedIndex === index
        }
    }
    signal closeRequested()

    function handleEscape() {
        if (sourcePicker.popup.visible) sourcePicker.popup.close()
        else if (sortPicker.popup.visible) sortPicker.popup.close()
        else if (categoryPicker.popup.visible) categoryPicker.popup.close()
        else if (audioBeforePlay.popup.visible) audioBeforePlay.popup.close()
        else if (subtitlesBeforePlay.popup.visible) subtitlesBeforePlay.popup.close()
        else if (root.detailsOpen) {
            root.catalog.back()
            movies.forceActiveFocus()
        } else root.closeRequested()
        return true
    }
    function prepareForOpen() {
        root.catalog.open()
        movies.forceActiveFocus()
        posterTimer.restart()
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
        enabled: root.visible && !root.detailsOpen
        onActivated: search.forceActiveFocus()
    }
    Timer { id: posterTimer; interval: 80; onTriggered: root.requestVisiblePosters() }
    Connections {
        target: root.catalog
        function onChanged() { if (root.visible) posterTimer.restart() }
    }
    onVisibleChanged: {
        if (!visible) root.catalog.close()
    }

    Rectangle { anchors.fill: parent; color: Theme.uiBackground(Theme.vodLibraryBackground, root.uiTransparency) }
    // Consume pointer/wheel events so browsing cannot operate the underlying video.
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: wheel => { wheel.accepted = true } }

    RowLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            visible: !root.compactLayout
            Layout.preferredWidth: root.width < 1200 ? 208 : 268
            Layout.fillHeight: true
            color: Theme.uiBackground(Theme.vodLibrarySidebar, root.uiTransparency)
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border; opacity: 0.45 }
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 6
                Label {
                    text: "Library"; color: Theme.textSecondary
                    font.pixelSize: 20; font.bold: true
                    Layout.leftMargin: 10; Layout.topMargin: 14; Layout.bottomMargin: 10
                }
                LibraryNav {
                    objectName: "ui.vod.all"
                    Layout.fillWidth: true
                    text: "All movies"; glyphKind: "grid"
                    highlighted: root.catalog.categoryId === ""
                    onClicked: { root.catalog.back(); root.catalog.selectCategory("") }
                }
                Rectangle {
                    Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10
                    Layout.topMargin: 16; Layout.bottomMargin: 12
                    implicitHeight: 1; color: Theme.border; opacity: 0.65
                }
                Label {
                    text: "Categories"; color: Theme.textSecondary; font.pixelSize: 18; font.bold: true
                    Layout.leftMargin: 10; Layout.bottomMargin: 8
                }
                ListView {
                    id: categories
                    Layout.fillWidth: true; Layout.fillHeight: true
                    clip: true; spacing: 4
                    model: root.catalog.categories
                    ScrollBar.vertical: ScrollBar { }
                    delegate: LibraryNav {
                        required property var modelData
                        width: categories.width
                        text: modelData.name
                        highlighted: root.catalog.categoryId === modelData.id
                        onClicked: { root.catalog.back(); root.catalog.selectCategory(modelData.id) }
                    }
                }
                Label {
                    text: "Esc to go back"; color: Theme.textMuted; font.pixelSize: 11
                    Layout.leftMargin: 10; Layout.bottomMargin: 6
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumWidth: 0
            Layout.margins: root.compactLayout ? 12 : 20
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
                    placeholderText: "Search movies…"
                    leftPadding: 48
                    text: root.catalog.searchText
                    enabled: !root.detailsOpen
                    onTextEdited: root.catalog.searchText = text
                    background: Rectangle {
                        radius: Theme.radiusS
                        color: Theme.surfaceRaised
                        border.width: 1
                        border.color: search.activeFocus ? Theme.accent : Theme.border
                    }
                    LibraryGlyph { x: 14; anchors.verticalCenter: parent.verticalCenter; kind: "search" }
                    Keys.onDownPressed: { movies.forceActiveFocus(); if (movies.currentIndex < 0 && movies.count > 0) movies.currentIndex = 0 }
                    Keys.onReturnPressed: { movies.forceActiveFocus(); if (movies.currentIndex < 0 && movies.count > 0) movies.currentIndex = 0 }
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
                    onActivated: index => root.catalog.selectSource(root.catalog.sources[index].id)
                }
                IconActionButton {
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
                IconActionButton {
                    objectName: "ui.vod.close"
                    compact: true; iconName: "windowClose"
                    caption: "Close library (V)"; Accessible.name: caption
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
                AppButton { text: "Retry"; compact: true; enabled: !root.catalog.busy; onClicked: root.catalog.refresh() }
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
                            color: Theme.textPrimary; font.pixelSize: 16; font.bold: true
                        }
                        CompactLibraryCombo {
                            id: categoryPicker
                            objectName: "ui.vod.category"
                            visible: root.compactLayout
                            Layout.fillWidth: true; Layout.minimumWidth: 80
                            model: root.catalog.categories.slice(0, 1).concat([{id: "", name: "All movies"}], root.catalog.categories.slice(1))
                            textRole: "name"
                            currentIndex: {
                                for (let i = 0; i < model.length; ++i)
                                    if (model[i].id === root.catalog.categoryId) return i
                                return 0
                            }
                            onActivated: index => { root.catalog.back(); root.catalog.selectCategory(model[index].id) }
                        }
                        CompactLibraryCombo {
                            id: sortPicker
                            objectName: "ui.vod.sort"
                            Layout.preferredWidth: root.compactLayout ? 140 : 196
                            model: ["Title A–Z", "Title Z–A"]
                            displayText: "Sort: " + currentText
                            currentIndex: root.catalog.descending ? 1 : 0
                            onActivated: index => { root.catalog.descending = index === 1 }
                        }
                    }
                    GridView {
                        id: movies
                        objectName: "ui.vod.grid"
                        Layout.fillWidth: true; Layout.fillHeight: true
                        Layout.minimumHeight: 0
                        clip: true
                        readonly property int columns: Math.max(1, Math.floor(width / cellWidth))
                        cellWidth: root.posterWidth + root.cardSpacing
                        cellHeight: root.movieCardHeight
                        model: root.catalog
                        currentIndex: -1
                        keyNavigationEnabled: true
                        highlightMoveDuration: 100
                        highlightFollowsCurrentItem: true
                        cacheBuffer: 0
                        ScrollBar.vertical: ScrollBar { }
                        onContentYChanged: {
                            posterTimer.restart()
                            if (contentY + height >= contentHeight - cellHeight * 2) root.catalog.fetchMoreMovies()
                        }
                        onCountChanged: {
                            if (currentIndex < 0 && count > 0) currentIndex = 0
                            posterTimer.restart()
                        }
                        onWidthChanged: posterTimer.restart()
                        onHeightChanged: posterTimer.restart()
                        Keys.onReturnPressed: { if (currentIndex >= 0) root.catalog.selectMovie(currentIndex) }
                        Keys.onEnterPressed: { if (currentIndex >= 0) root.catalog.selectMovie(currentIndex) }
                        header: Column {
                            width: movies.width
                            visible: root.catalog.categoryId === ""
                            height: root.catalog.categoryId === "" ? implicitHeight + 14 : 0
                            onHeightChanged: { if (root.catalog.categoryId === "" && !root.detailsOpen) Qt.callLater(function() { movies.positionViewAtBeginning() }) }
                            spacing: 12
                            RowLayout {
                                visible: root.showContinue
                                width: parent.width
                                Label { text: "Continue watching"; color: Theme.textPrimary; font.pixelSize: 20; font.bold: true; Layout.fillWidth: true }
                                AppButton {
                                    text: "View all"; compact: true
                                    onClicked: root.catalog.selectCategory("__continue_watching__")
                                }
                            }
                            ListView {
                                id: continueShelf
                                objectName: "ui.vod.continue"
                                visible: root.showContinue
                                width: parent.width; height: root.movieCardHeight + 10
                                orientation: ListView.Horizontal; spacing: root.cardSpacing; clip: true
                                model: root.catalog.continueMovies
                                ScrollBar.horizontal: ScrollBar { }
                                delegate: ItemDelegate {
                                    id: continueCard
                                    required property int index
                                    required property var modelData
                                    width: root.posterWidth; height: root.movieCardHeight; padding: 0
                                    background: Rectangle { color: continueCard.hovered ? Theme.surfaceInteractive : "transparent"; radius: Theme.radiusS }
                                    Accessible.name: modelData.title
                                    onClicked: root.catalog.selectMovie(-index - 1)
                                    contentItem: Column {
                                        spacing: 6
                                        Rectangle {
                                            width: root.posterWidth; height: root.posterHeight; radius: Theme.radiusS
                                            color: Theme.surfaceRaised
                                            border.color: continueCard.visualFocus ? Theme.accent : Theme.border
                                            Label { anchors.centerIn: parent; width: parent.width - 12; text: continueCard.modelData.title; wrapMode: Text.WordWrap; maximumLineCount: 4; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter; color: Theme.textSecondary }
                                            Image { anchors.fill: parent; anchors.margins: 3; source: continueCard.modelData.poster; asynchronous: true; fillMode: Image.PreserveAspectCrop; sourceSize.width: root.posterWidth * 2; sourceSize.height: root.posterHeight * 2 }
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
                                        Label { width: root.posterWidth; text: continueCard.modelData.title; elide: Text.ElideRight; color: Theme.textPrimary; font.pixelSize: 16; font.bold: true }
                                        Label { width: root.posterWidth; text: continueCard.modelData.year || ""; color: Theme.textSecondary; font.pixelSize: 14 }
                                    }
                                    ToolTip.visible: hovered; ToolTip.text: modelData.title
                                }
                            }
                            Label {
                                objectName: "ui.vod.allHeading"
                                text: "All movies"; color: Theme.textPrimary
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
                            width: movies.cellWidth
                            height: movies.cellHeight
                            Rectangle {
                                id: posterFrame
                                width: root.posterWidth; height: root.posterHeight
                                radius: 4
                                color: Theme.surfaceRaised
                                border.width: movies.currentIndex === card.index && movies.activeFocus ? 3 : 1
                                border.color: movies.currentIndex === card.index && movies.activeFocus ? Theme.accent : Theme.border
                                Rectangle {
                                    anchors.fill: parent; anchors.margins: 3; radius: Theme.radiusS
                                    gradient: Gradient {
                                        GradientStop { position: 0; color: Theme.surfaceMuted }
                                        GradientStop { position: 1; color: Theme.surface }
                                    }
                                }
                                Column {
                                    anchors.centerIn: parent; width: parent.width - 24; spacing: 12
                                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: "▸"; color: Theme.textMuted; font.pixelSize: 36 }
                                    Text { width: parent.width; text: card.title; color: Theme.textSecondary; font.pixelSize: 14; wrapMode: Text.WordWrap; maximumLineCount: 4; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
                                }
                                Image {
                                    anchors.fill: parent; anchors.margins: 3
                                    source: card.poster; asynchronous: true; fillMode: Image.PreserveAspectCrop
                                    sourceSize.width: 480; sourceSize.height: 720
                                    visible: status === Image.Ready
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
                            Text {
                                anchors.top: posterFrame.bottom; anchors.topMargin: 9
                                width: posterFrame.width; text: card.title; color: card.available ? Theme.textPrimary : Theme.textMuted
                                font.pixelSize: 16; font.bold: true; elide: Text.ElideRight
                            }
                            Text {
                                anchors.top: posterFrame.bottom; anchors.topMargin: 29
                                text: card.year; color: Theme.textSecondary; font.pixelSize: 14
                            }
                            HoverHandler { id: cardHover }
                            TapHandler { onTapped: { movies.currentIndex = card.index; movies.forceActiveFocus(); root.catalog.selectMovie(card.index) } }
                            ToolTip.visible: cardHover.hovered
                            ToolTip.delay: 600
                            ToolTip.text: card.title
                        }
                        Label {
                            anchors.centerIn: parent
                            width: Math.min(parent.width - 32, 420)
                            horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                            color: Theme.textSecondary; font.pixelSize: 16
                            visible: movies.count === 0
                            text: root.catalog.sources.length === 0 ? "Add an Xtream source in Settings to browse movies."
                                : root.catalog.busy ? "Loading your movie library…"
                                : root.catalog.errorText.length > 0 ? "Your library is unavailable. Try again."
                                : root.catalog.searchText.length > 0 ? "No movies match your search."
                                : "No movies in this library."
                        }
                    }
                    AppButton {
                        Layout.alignment: Qt.AlignHCenter
                        text: "Load more movies"; visible: root.catalog.hasMore
                        enabled: !root.catalog.busy
                        onClicked: root.catalog.fetchMoreMovies()
                    }
                }
                Flickable {
                    id: details
                    anchors.fill: parent; visible: root.detailsOpen
                    clip: true; contentHeight: detailColumn.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { }
                    ColumnLayout {
                        id: detailColumn
                        width: details.width
                        spacing: 24
                        AppButton { text: "‹  Back to movies"; compact: true; onClicked: { root.catalog.back(); movies.forceActiveFocus() } }
                        GridLayout {
                            columns: root.compactLayout ? 1 : 2
                            Layout.fillWidth: true; columnSpacing: 24; rowSpacing: 20
                            Rectangle {
                                Layout.alignment: Qt.AlignTop
                                Layout.preferredWidth: root.compactLayout ? 120 : root.width < 1000 ? 140 : 240
                                Layout.preferredHeight: width * 1.5
                                radius: Theme.radiusS; color: Theme.surfaceRaised
                                Label { anchors.centerIn: parent; text: "▸"; font.pixelSize: 50; color: Theme.textMuted }
                                Image { anchors.fill: parent; source: root.catalog.movie.poster || ""; asynchronous: true; fillMode: Image.PreserveAspectCrop }
                                ResolutionBadge { label: root.catalog.movie.resolutionLabel || "" }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; spacing: 18
                                Label { Layout.fillWidth: true; text: root.catalog.movie.title || ""; color: Theme.textPrimary; font.pixelSize: root.width < 1000 ? 24 : 34; font.bold: true; wrapMode: Text.WordWrap }
                                Label {
                                    Layout.fillWidth: true
                                    text: [root.catalog.movie.year || "", root.catalog.movie.durationMinutes > 0 ? root.catalog.movie.durationMinutes + " min" : "", root.catalog.movie.resolution || "", root.catalog.movie.genres || ""].filter(part => part.length > 0).join("   ·   ")
                                    color: Theme.textSecondary; font.pixelSize: 13; wrapMode: Text.WordWrap
                                }
                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: root.compactLayout ? 1 : 2
                                    columnSpacing: 14; rowSpacing: 10
                                    ColumnLayout {
                                        Layout.fillWidth: true; spacing: 6
                                        Label { text: "AUDIO"; color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1.5 }
                                        LibraryCombo {
                                            id: audioBeforePlay
                                            objectName: "ui.vod.audioBeforePlay"
                                            Layout.fillWidth: true
                                            model: root.catalog.movie.audioTrackOptions || []
                                            textRole: "label"
                                            currentIndex: root.catalog.movie.audioTrackIndex === undefined ? 0 : root.catalog.movie.audioTrackIndex
                                            enabled: Boolean(root.catalog.movie.mediaProbeReady) && Boolean(root.catalog.movie.progressLoaded)
                                                && model.length > 1 && !root.catalog.startingPlayback
                                            onActivated: index => root.catalog.selectAudioOption(index)
                                        }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true; spacing: 6
                                        Label { text: "SUBTITLES"; color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1.5 }
                                        LibraryCombo {
                                            id: subtitlesBeforePlay
                                            objectName: "ui.vod.subtitlesBeforePlay"
                                            Layout.fillWidth: true
                                            model: root.catalog.movie.subtitleTrackOptions || []
                                            textRole: "label"
                                            currentIndex: root.catalog.movie.subtitleTrackIndex === undefined ? 0 : root.catalog.movie.subtitleTrackIndex
                                            enabled: Boolean(root.catalog.movie.mediaProbeReady) && Boolean(root.catalog.movie.progressLoaded)
                                                && model.length > 2 && !root.catalog.startingPlayback
                                            onActivated: index => root.catalog.selectSubtitleOption(index)
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: Boolean(root.catalog.movie.mediaProbeLoading) || Boolean(root.catalog.movie.mediaProbeError)
                                    text: root.catalog.movie.mediaProbeLoading ? "Reading resolution and media tracks…" : root.catalog.movie.mediaProbeError || ""
                                    color: Theme.textMuted; font.pixelSize: 12; wrapMode: Text.WordWrap
                                }
                                Flow {
                                    Layout.fillWidth: true; spacing: 10
                                    AppButton {
                                        objectName: "ui.vod.play"
                                        accent: true
                                        text: root.catalog.startingPlayback ? "Starting…" : root.catalog.movie.resumeSeconds >= 60 ? "Resume · " + root.resumeTimestamp(root.catalog.movie.resumeSeconds) : "Play movie"
                                        enabled: Boolean(root.catalog.movie.available) && !root.catalog.busy
                                        onClicked: root.catalog.play(root.catalog.movie.resumeSeconds < 60)
                                    }
                                    AppButton {
                                        objectName: "ui.vod.watched"
                                        text: root.catalog.movie.watched ? "Mark as unwatched" : "Mark as watched"
                                        enabled: Boolean(root.catalog.movie.progressLoaded) && !root.catalog.busy
                                        onClicked: root.catalog.toggleWatched()
                                    }
                                    AppButton {
                                        text: "Play from beginning"
                                        visible: root.catalog.movie.resumeSeconds >= 60
                                        enabled: Boolean(root.catalog.movie.available) && !root.catalog.busy
                                        onClicked: root.catalog.play(true)
                                    }
                                }
                                Label { Layout.fillWidth: true; text: root.catalog.movie.description || (root.catalog.busy ? "Loading details…" : "No description available."); color: Theme.textSecondary; font.pixelSize: 15; wrapMode: Text.WordWrap; lineHeight: 1.35 }
                                Label { text: "CAST"; visible: Boolean(root.catalog.movie.cast); color: Theme.textMuted; font.pixelSize: 11; font.letterSpacing: 1.5 }
                                Label { Layout.fillWidth: true; text: root.catalog.movie.cast || ""; visible: text.length > 0; color: Theme.textSecondary; font.pixelSize: 13; wrapMode: Text.WordWrap }
                            }
                        }
                    }
                }
            }
        }
    }
}
