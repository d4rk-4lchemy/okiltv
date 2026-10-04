pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme/Theme.js" as Theme

FocusScope {
    id: root
    required property var episodeModel
    property bool directPlay: false
    property bool seasonKeyNavigation: false
    property bool marqueeEnabled: false
    property int uiTransparency: 100
    readonly property bool seasonPopupOpen: seasonPicker.popup.visible
    readonly property bool listActive: episodeList.activeFocus
    readonly property bool searchActive: seasonPicker.activeFocus
    property string anchorKey: ""
    property int anchorIndex: 0
    property real anchorOffset: 0
    function rowData(index) {
        const model = episodeList.model
        const data = model && model.get ? model.get(index) : model ? model[index] : null
        return data && data.modelData ? data.modelData : data
    }
    function captureAnchor() {
        anchorIndex = episodeList.indexAt(1, episodeList.contentY + 1)
        if (anchorIndex < 0) {
            for (let i = 0; i < episodeList.count; ++i) {
                const item = episodeList.itemAtIndex(i)
                if (item && item.y + item.height > episodeList.contentY) { anchorIndex = i; break }
            }
        }
        const data = rowData(anchorIndex)
        const item = episodeList.itemAtIndex(anchorIndex)
        anchorKey = data ? data.episodeKey : ""
        anchorOffset = item ? episodeList.contentY - item.y : 0
    }
    function restoreAnchor() {
        if (!episodeList.count) return
        let target = Math.max(0, Math.min(anchorIndex, episodeList.count - 1))
        for (let i = 0; i < episodeList.count; ++i) {
            const data = rowData(i)
            if (data && data.episodeKey === anchorKey) { target = i; break }
        }
        episodeList.forceLayout()
        episodeList.positionViewAtIndex(target, ListView.Beginning)
        const item = episodeList.itemAtIndex(target)
        if (item) episodeList.contentY = Math.max(episodeList.originY, Math.min(item.y + anchorOffset,
            episodeList.originY + Math.max(0, episodeList.contentHeight - episodeList.height)))
    }
    VodMarqueeController {
        id: titleSelection
        anchors.fill: parent
        ready: root.marqueeEnabled && !root.seasonPopupOpen
        keyboardItem: episodeList.activeFocus ? episodeList.currentItem : null
    }
    function closePopup() { seasonPicker.popup.close() }
    function focusEpisodes() { episodeList.forceActiveFocus() }
    function handleKey(event) {
        if (event.modifiers !== Qt.NoModifier || !root.episodeModel || root.seasonPopupOpen) return false
        if (root.seasonKeyNavigation && (event.key === Qt.Key_Left || event.key === Qt.Key_Right)) {
            titleSelection.useKeyboard()
            const seasons = root.episodeModel.seasons
            const current = seasons.findIndex(item => item.id === root.episodeModel.seasonId)
            const next = current + (event.key === Qt.Key_Right ? 1 : -1)
            if (current >= 0 && next >= 0 && next < seasons.length && !root.episodeModel.busy) {
                root.episodeModel.selectSeason(seasons[next].id)
                Qt.callLater(function() {
                    if (episodeList.currentIndex >= 0)
                        episodeList.positionViewAtIndex(episodeList.currentIndex, ListView.Contain)
                })
            }
            return true
        }
        const selectedIndex = root.episodeModel.currentIndex !== undefined
            ? root.episodeModel.currentIndex : episodeList.currentIndex
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            titleSelection.useKeyboard()
            root.episodeModel.playEpisode(selectedIndex); return true
        }
        if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
            titleSelection.useKeyboard()
            root.episodeModel.selectEpisode(Math.max(0, Math.min(episodeList.count - 1,
                selectedIndex + (event.key === Qt.Key_Up ? -1 : 1))))
            episodeList.positionViewAtIndex(episodeList.currentIndex, ListView.Contain); return true
        }
        return false
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 10
        VodSelector {
            id: seasonPicker
            objectName: "ui.vod.season"
            Layout.fillWidth: true
            uiTransparency: root.uiTransparency
            popupHeightLimit: Math.max(height, root.height - height - 24)
            model: root.episodeModel ? root.episodeModel.seasons : []
            textRole: "name"
            currentIndex: root.episodeModel ? Math.max(0, model.findIndex(item => item.id === root.episodeModel.seasonId)) : 0
            enabled: root.episodeModel && !root.episodeModel.busy
            onActivated: index => {
                root.episodeModel.selectSeason(model[index].id)
                Qt.callLater(function() { episodeList.positionViewAtIndex(episodeList.currentIndex, ListView.Contain) })
            }
            Keys.onPressed: event => {
                if (!popup.visible && event.key === Qt.Key_Down && event.modifiers === Qt.NoModifier) {
                    episodeList.forceActiveFocus(); event.accepted = true
                }
            }
        }
        ListView {
            id: episodeList
            objectName: "ui.vod.episodes"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            Layout.minimumHeight: 0
            boundsBehavior: Flickable.StopAtBounds
            highlightRangeMode: ListView.NoHighlightRange
            keyNavigationEnabled: false
            model: root.episodeModel ? (root.episodeModel.rows || root.episodeModel.episodes) : []
            currentIndex: root.episodeModel && root.episodeModel.currentIndex !== undefined ? root.episodeModel.currentIndex
                : model.findIndex(item => item.selected)
            ScrollBar.vertical: VodScrollBar {
                id: episodeScrollBar
                objectName: "ui.vod.episodeScrollBar"
            }
            onActiveFocusChanged: titleSelection.useKeyboard()
            Keys.onPressed: event => { if (root.handleKey(event)) event.accepted = true }
            delegate: ItemDelegate {
                id: row
                required property int index
                required property var modelData
                readonly property string marqueeKey: modelData.episodeKey
                readonly property bool inViewport: y + height > episodeList.contentY && y < episodeList.contentY + episodeList.height
                HoverHandler {
                    onHoveredChanged: titleSelection.hoverTarget(row.marqueeKey, hovered)
                }
                Component.onDestruction: titleSelection.hoverTarget(marqueeKey, false)
                text: modelData.label + " · " + modelData.title
                objectName: "ui.vod.episode." + index
                width: Math.max(0, ListView.view.width - episodeScrollBar.width - Theme.vodScrollBarGap)
                height: Math.max(72, rowText.implicitHeight + 20)
                enabled: root.episodeModel && !root.episodeModel.busy
                focusPolicy: Qt.NoFocus
                rightPadding: 12
                highlighted: modelData.selected
                background: Rectangle {
                    radius: 4
                    color: Theme.uiBackground(row.highlighted ? Theme.overlaySurfaceInteractive : row.hovered ? Theme.overlaySurfaceRaised : Theme.overlaySurface, root.uiTransparency)
                    border.color: row.highlighted ? Theme.accent : Theme.overlayBorder
                    border.width: row.highlighted ? 2 : 1
                }
                contentItem: RowLayout {
                    spacing: 10
                    Image { source: row.modelData.poster; visible: source.toString().length > 0; Layout.preferredWidth: 80; Layout.preferredHeight: 48; fillMode: Image.PreserveAspectCrop; asynchronous: true }
                    ColumnLayout {
                        id: rowText
                        Layout.fillWidth: true
                        spacing: 4
                        VodMarqueeTitle {
                            objectName: "ui.vod.episodeTitle." + row.modelData.episodeKey
                            Layout.fillWidth: true; Layout.rightMargin: watchedButton.width + 8
                            text: row.modelData.label + " · " + row.modelData.title
                            color: Theme.overlayTextPrimary; font.bold: row.highlighted
                            contentIdentity: row.marqueeKey
                            indicated: row.inViewport && titleSelection.activeTarget === row.marqueeKey
                        }
                        Label { Layout.fillWidth: true; Layout.rightMargin: watchedButton.width + 8; text: (row.modelData.durationMinutes > 0 ? row.modelData.durationMinutes + " min" : "") + (row.modelData.watched ? "  ·  Watched" : row.modelData.resumeSeconds > 0 ? "  ·  In progress" : ""); color: Theme.overlayTextSecondary; font.pixelSize: 12 }
                        Rectangle {
                            objectName: "ui.vod.episodeProgress." + row.modelData.episodeKey
                            Layout.fillWidth: true; Layout.preferredHeight: 3; color: Theme.overlayBorder
                            Rectangle { width: parent.width * row.modelData.progressFraction; height: parent.height; color: Theme.accent }
                        }
                    }
                }
                IconActionButton {
                    id: watchedButton
                    readonly property real sizeFactor: 0.6
                    objectName: "ui.vod.episodeWatched." + row.modelData.episodeKey
                    anchors.top: parent.top; anchors.right: parent.right
                    anchors.margins: 8
                    implicitWidth: 42 * sizeFactor
                    implicitHeight: 42 * sizeFactor
                    compact: true; padding: 9 * sizeFactor; iconInset: 0; scale: 1
                    focusPolicy: Qt.TabFocus
                    uiTransparency: root.uiTransparency
                    iconSource: row.modelData.watched ? "qrc:/resources/icons/mark-unwatched.svg" : "qrc:/resources/icons/mark-watched.svg"
                    caption: row.modelData.watched ? "Mark episode as unwatched" : "Mark episode as watched"
                    Accessible.name: caption
                    enabled: Boolean(root.episodeModel && root.episodeModel.statusReady && !root.episodeModel.statusBusy)
                    retainEnabledAppearance: Boolean(root.episodeModel && !root.episodeModel.busy)
                    background: Rectangle {
                        radius: Theme.radiusS * watchedButton.sizeFactor
                        color: Theme.uiBackground(watchedButton.down || watchedButton.hovered ? Theme.overlaySurfaceInteractive : Theme.overlaySurfaceRaised, root.uiTransparency)
                        border.width: 1
                        border.color: watchedButton.visualFocus ? Theme.overlayTextSecondary : Theme.overlayBorder
                    }
                    onClicked: root.episodeModel.toggleEpisodeWatched(row.modelData.episodeKey)
                }
                ToolTip.visible: hovered && !watchedButton.hovered && modelData.description.length > 0
                ToolTip.text: modelData.description
                onClicked: {
                    if (!row.modelData.available) return
                    episodeList.forceActiveFocus()
                    if (root.directPlay) root.episodeModel.playEpisode(index)
                    else root.episodeModel.selectEpisode(index)
                }
            }
        }
        Label { Layout.fillWidth: true; visible: text.length > 0; text: root.episodeModel ? root.episodeModel.errorText : ""; wrapMode: Text.WordWrap; color: Theme.overlayTextSecondary }
    }
    Timer {
        interval: 200; repeat: true
        running: root.visible && root.enabled && root.episodeModel && !root.episodeModel.busy
        onTriggered: {
            if (!root.episodeModel.requestArtwork) return
            for (let i = 0; i < episodeList.count; ++i) {
                const item = episodeList.itemAtIndex(i)
                if (item && item.y + item.height > episodeList.contentY && item.y < episodeList.contentY + episodeList.height)
                    root.episodeModel.requestArtwork(i)
            }
        }
    }
    Connections {
        target: root.episodeModel
        ignoreUnknownSignals: true
        function onRowsChanging() { root.captureAnchor() }
        function onRowsChanged() { root.restoreAnchor() }
        function onLoaded() {
            Qt.callLater(function() {
                if (episodeList.currentIndex >= 0) episodeList.positionViewAtIndex(episodeList.currentIndex, ListView.Contain)
            })
        }
    }
}
