pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import "../theme/Theme.js" as Theme

Popup {
    id: root
    objectName: "ui.epgSearch.overlay"
    required property var controller
    property int uiTransparency: 100
    property bool inputBlocked: false
    property var loadMoreAction: null
    property string pendingActivationKey: ""
    property string pendingActivationQuery: ""
    property string pendingActivationFilter: ""
    property string revealedSelectionKey: ""
    property int revealedSelectionIndex: -1
    property string collapsedDetailsKey: ""
    readonly property var styleHints: Qt.styleHints
    readonly property bool compact: parent && parent.height < 400
    readonly property bool hasQuery: query.text.trim().length > 0
    readonly property bool expanded: hasQuery && controller.expanded && controller.status !== "idle"
    readonly property bool currentResults: expanded && controller.resultsCurrent && results.count > 0
    readonly property EpgSearchResultDelegate selectedDelegate: results.currentItem as EpgSearchResultDelegate
    readonly property var activeDetailsPane: currentResults && selectedDelegate ? selectedDelegate.detailsPane : null
    readonly property alias queryField: query
    readonly property alias resultsView: results
    readonly property string selectedResultKey: controller.selectedKey
    readonly property bool interactive: visible && !inputBlocked
    signal closeRequested()
    parent: Overlay.overlay
    popupType: Popup.Item
    modal: true
    dim: false
    focus: true
    closePolicy: Popup.NoAutoClose
    padding: 0
    width: parent ? Math.min(824, Math.max(128, parent.width - 24)) : 426
    height: Math.min(parent ? Math.max(0, parent.height - y - 12) : 240,
        112 + (hasQuery ? 6 + body.implicitHeight : 0))
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? Math.max(12, parent.height * 0.25 - 52) : 12
    background: Item {}
    onOpened: {
        revealedSelectionKey = ""
        revealedSelectionIndex = -1
        collapsedDetailsKey = ""
        focusQuery()
    }
    onClosed: { clearGesture(); pendingActivationKey = ""; collapsedDetailsKey = "" }
    onInputBlockedChanged: {
        if (inputBlocked) { pendingActivationKey = ""; clearGesture() }
        if (!inputBlocked && visible) Qt.callLater(root.focusQuery)
    }
    function activateResult(key) {
        if (!root.interactive || !root.currentResults || controller.selectedKey !== key) return
        if (controller.detailsBusy) {
            pendingActivationKey = key
            pendingActivationQuery = controller.query
            pendingActivationFilter = controller.timeFilter
        } else controller.activateSelected()
    }
    function finishPendingActivation() {
        if (!pendingActivationKey) return
        const current = root.interactive && root.currentResults && !controller.detailsBusy
            && controller.selectedKey === pendingActivationKey && controller.query === pendingActivationQuery
            && query.text === pendingActivationQuery && controller.timeFilter === pendingActivationFilter
        pendingActivationKey = ""
        if (current) controller.activateSelected()
    }
    function activateFromBeginningOrDefault() {
        if (!root.interactive || !root.currentResults || controller.detailsBusy || query.inputMethodComposing) return
        controller.activateSelectedFromBeginningOrDefault()
    }
    function clearGesture() {
        clickWindow.stop()
        resultPointer.firstKey = ""
        resultPointer.firstIndex = -1
        resultPointer.collapseKey = ""
    }
    function chooseResult(index, key) {
        if (!root.interactive || !root.currentResults) return
        resultPointer.collapseKey = ""
        if (controller.selectedKey !== key) {
            collapsedDetailsKey = ""
            controller.selectIndex(index)
        } else if (collapsedDetailsKey === key) {
            collapsedDetailsKey = ""
        } else {
            // Wait for a possible second click before removing the details.
            resultPointer.collapseKey = key
        }
        clickWindow.restart()
    }
    function collapseDetails(key) {
        if (!key || !root.interactive || !root.currentResults || controller.selectedKey !== key) return
        if (root.activeDetailsPane && root.activeDetailsPane.activeFocus) results.forceActiveFocus()
        collapsedDetailsKey = key
    }
    function headerAt(x, y) {
        if (!root.currentResults || x < 0 || y < 0 || x >= results.width || y >= results.height) return null
        const item = results.itemAt(x, y + results.contentY) as EpgSearchResultDelegate
        return item && y + results.contentY >= item.y && y + results.contentY - item.y < item.headerHeight ? item : null
    }
    function revealSelection() {
        if (!root.currentResults || !root.interactive) return
        const item = results.itemAtIndex(root.controller.selectedIndex) as EpgSearchResultDelegate
        if (!item) {
            results.positionViewAtIndex(root.controller.selectedIndex, ListView.Beginning)
            return
        }
        // Reveal the header, rather than the whole expanded card, on small screens.
        if (item.y < results.contentY) results.contentY = item.y
        else if (item.y + item.headerHeight > results.contentY + results.height)
            results.contentY = item.y + item.headerHeight - results.height
    }
    function revealControl(item) {
        if (!root.interactive || !item) return
        const top = item.mapToItem(results.contentItem, 0, 0).y
        if (top < results.contentY) results.contentY = top
        else if (top + item.height > results.contentY + results.height)
            results.contentY = top + item.height - results.height
    }
    function focusQuery() {
        if (!root.interactive) return
        query.forceActiveFocus()
        query.selectAll()
    }
    function cycleFocus(backwards) {
        const candidates = [query, closeButton]
        for (let i = 0; i < filterButtons.count; ++i) candidates.push(filterButtons.itemAt(i))
        candidates.push(results, retryButton, currentRetryButton, root.loadMoreAction)
        if (root.activeDetailsPane) {
            for (const target of root.activeDetailsPane.focusTargets) candidates.push(target)
        }
        const targets = candidates.filter(item => item && item.visible && item.enabled)
        const current = root.contentItem.Window.window ? root.contentItem.Window.window.activeFocusItem : null
        let index = targets.indexOf(current)
        if (index < 0) {
            index = targets.findIndex(item => item.activeFocus)
        }
        if (targets.length > 0) {
            const next = targets[(index + (backwards ? -1 : 1) + targets.length) % targets.length]
            // A ListView focus scope otherwise restores its focused descendant.
            if (next === results && root.activeDetailsPane) root.activeDetailsPane.focus = false
            next.forceActiveFocus()
        }
    }
    function cycleTimeFilter() {
        if (!root.interactive || query.inputMethodComposing) return
        const filters = ["all", "now", "upcoming", "past"]
        const index = filters.indexOf(controller.timeFilter)
        controller.setTimeFilter(filters[(index + 1) % filters.length])
        // Detail controls may disappear while the new results load. Keep the
        // editor ready for typing without changing its selection or cursor.
        query.forceActiveFocus()
    }
    function handleTab(backwards) {
        if (!root.interactive || query.inputMethodComposing) return
        if (backwards) cycleFocus(true)
        else cycleTimeFilter()
    }
    function tabKey(event) {
        if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
            && (event.modifiers === Qt.NoModifier || event.modifiers === Qt.ShiftModifier)) {
            if (!event.isAutoRepeat) handleTab(event.key === Qt.Key_Backtab || event.modifiers === Qt.ShiftModifier)
            event.accepted = true
        }
    }
    function resultKey(event) {
        tabKey(event)
        if (event.accepted || !root.interactive || event.modifiers !== Qt.NoModifier || query.inputMethodComposing) return
        if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
            controller.moveSelection(event.key === Qt.Key_Down ? 1 : -1)
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            if (!event.isAutoRepeat && controller.resultsCurrent) controller.activateSelected()
            event.accepted = true
        }
    }
    function stateMessage() {
        const status = controller.status
        if (controller.errorText.length > 0) return controller.errorText
        if (status === "no-source") return qsTr("Choose a Live TV source to search its programme guide.")
        if (status === "no-channels") return qsTr("No channels are available in this source's search scope.")
        if (status === "no-epg") return qsTr("No local EPG data for this source.")
        if (status === "preparing") return qsTr("Preparing EPG search…")
        if (controller.busy) return qsTr("Searching…")
        if (status === "idle" || controller.query.trim().length < 2) return qsTr("Enter at least 2 characters")
        if (status === "empty" || status === "no-results") return qsTr("No results. Try another title or time filter.")
        return ""
    }
    Shortcut { sequence: "Ctrl+F"; autoRepeat: false; enabled: root.interactive; onActivated: root.focusQuery() }
    Shortcut { sequence: "Escape"; autoRepeat: false; enabled: root.interactive; onActivated: root.closeRequested() }
    Shortcut { sequence: "Ctrl+R"; autoRepeat: false; enabled: root.interactive; onActivated: if (root.controller.resultsCurrent) root.controller.toggleSelectedRecording() }
    Shortcut { sequence: "Ctrl+D"; autoRepeat: false; enabled: root.interactive; onActivated: if (root.controller.resultsCurrent) root.controller.downloadSelected() }
    Shortcut { sequence: "Ctrl+Return"; autoRepeat: false; enabled: root.interactive && !query.inputMethodComposing; onActivated: root.activateFromBeginningOrDefault() }
    Shortcut { sequence: "Ctrl+Enter"; autoRepeat: false; enabled: root.interactive && !query.inputMethodComposing; onActivated: root.activateFromBeginningOrDefault() }
    Connections {
        target: root.controller
        function onShowDetailsRequested() {
            root.collapsedDetailsKey = ""
            resultPointer.collapseKey = ""
            results.positionViewAtIndex(root.controller.selectedIndex, ListView.Beginning)
            Qt.callLater(function() {
                if (root.interactive && root.activeDetailsPane) root.activeDetailsPane.focusDetails()
            })
        }
        function onStateChanged() {
            if (root.pendingActivationKey) {
                if (!root.interactive || !root.controller.resultsCurrent
                    || root.controller.selectedKey !== root.pendingActivationKey
                    || root.controller.query !== root.pendingActivationQuery
                    || root.controller.timeFilter !== root.pendingActivationFilter)
                    root.pendingActivationKey = ""
                else if (!root.controller.detailsBusy) Qt.callLater(root.finishPendingActivation)
            }
            if (!root.controller.resultsCurrent) {
                root.clearGesture()
                root.collapsedDetailsKey = ""
            }
            if (root.controller.selectedKey !== root.collapsedDetailsKey) root.collapsedDetailsKey = ""
            if (root.controller.selectedKey !== resultPointer.collapseKey) resultPointer.collapseKey = ""
            // Action labels, details and page appends also notify stateChanged.
            // Reveal only an actual selection move; preserve the browsing offset.
            if (root.controller.resultsCurrent && root.controller.selectedIndex >= 0
                && (root.controller.selectedKey !== root.revealedSelectionKey
                    || root.controller.selectedIndex !== root.revealedSelectionIndex)) {
                root.revealedSelectionKey = root.controller.selectedKey
                root.revealedSelectionIndex = root.controller.selectedIndex
                Qt.callLater(root.revealSelection)
            }
            if (!query.inputMethodComposing && query.text !== root.controller.query) query.text = root.controller.query
        }
    }
    component SearchButton: Button {
        id: button
        implicitHeight: root.compact ? 24 : 30
        font.pixelSize: 13
        activeFocusOnTab: true
        Accessible.name: text
        Keys.onPressed: event => {
            root.tabKey(event)
            if (!event.accepted && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                && event.modifiers === Qt.NoModifier && !event.isAutoRepeat) {
                button.clicked()
                event.accepted = true
            }
        }
        contentItem: Text {
            text: button.text
            textFormat: Text.PlainText
            font: button.font
            color: button.enabled ? Theme.textPrimary : Theme.textMuted
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            color: Theme.uiBackground(button.down ? Theme.liveRailPressed
                : button.hovered || button.checked ? Theme.liveRailSelection : Theme.liveRailBackground, root.uiTransparency)
            radius: 4
            border.width: button.activeFocus ? 1 : 0
            border.color: Theme.borderStrong
        }
    }
    contentItem: FocusScope {
        id: content
        enabled: !root.inputBlocked
        Keys.onPressed: event => root.tabKey(event)
        TextField {
            id: query
            objectName: "ui.epgSearch.query"
            x: 52
            y: 52
            width: parent.width - 104
            height: 60
            leftPadding: 12
            rightPadding: 12
            topPadding: 10
            bottomPadding: 10
            font.pixelSize: Theme.playbackSearchFontSize * 2
            text: root.controller.query
            placeholderText: qsTr("Enter a programme title…")
            placeholderTextColor: Theme.textMuted
            color: Theme.textPrimary
            selectByMouse: true
            Accessible.name: qsTr("Search EPG by title or subtitle")
            onTextChanged: if (root.pendingActivationKey && text !== root.pendingActivationQuery) root.pendingActivationKey = ""
            onTextEdited: if (!inputMethodComposing) root.controller.setQuery(text)
            onInputMethodComposingChanged: if (!inputMethodComposing) root.controller.setQuery(text)
            Keys.onPressed: event => root.resultKey(event)
            background: Rectangle {
                color: Theme.uiBackground(Theme.playbackSearchBackground, root.uiTransparency)
                radius: 4
            }
        }
        SearchButton {
            id: closeButton
            objectName: "ui.epgSearch.close"
            x: query.x + query.width + 12
            y: 0
            width: 40
            height: 40
            text: "×"
            font.pixelSize: 28
            Accessible.name: qsTr("Close EPG search")
            onClicked: root.closeRequested()
        }
        Rectangle {
            id: body
            objectName: "ui.epgSearch.body"
            x: query.x
            y: query.y + query.height + 6
            width: query.width
            height: Math.max(0, content.height - y)
            implicitHeight: (filters.visible ? filters.implicitHeight + 4 : 0)
                + (root.currentResults ? Math.min(320, results.contentHeight) : Math.max(64, message.implicitHeight + 16))
                + (footer.visible ? footer.implicitHeight + 4 : 0)
            visible: root.hasQuery
            color: Theme.uiBackground(Theme.liveRailBackground, root.uiTransparency)
            radius: 4
            clip: true
            ColumnLayout {
                anchors.fill: parent
                spacing: 4
                Flickable {
                    id: filters
                    objectName: "ui.epgSearch.filters"
                    Layout.fillWidth: true
                    Layout.preferredHeight: root.compact ? 24 : 30
                    implicitHeight: root.compact ? 24 : 30
                    visible: root.expanded
                    contentWidth: filterRow.width
                    contentHeight: height
                    clip: true
                    flickableDirection: Flickable.HorizontalFlick
                    Row {
                        id: filterRow
                        spacing: 6
                        Repeater {
                            id: filterButtons
                            model: [{label: qsTr("All"), key: "all"}, {label: qsTr("Now"), key: "now"}, {label: qsTr("Upcoming"), key: "upcoming"}, {label: qsTr("Past"), key: "past"}]
                            SearchButton {
                                required property var modelData
                                objectName: "ui.epgSearch.filter." + modelData.key
                                text: modelData.label
                                checked: root.controller.timeFilter === modelData.key
                                onClicked: root.controller.setTimeFilter(modelData.key)
                                ToolTip.text: modelData.key === "past" ? qsTr("Ended programmes in local EPG; playback availability varies.") : ""
                                ToolTip.visible: hovered && modelData.key === "past"
                            }
                        }
                    }
                }
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    ListView {
                        id: results
                        objectName: "ui.epgSearch.results"
                        anchors.fill: parent
                        clip: true
                        visible: root.currentResults
                        model: root.visible ? root.controller.model : null
                        reuseItems: true
                        currentIndex: root.controller.selectedIndex
                        keyNavigationEnabled: false
                        activeFocusOnTab: true
                        enabled: root.currentResults && !root.inputBlocked
                        Keys.onPressed: event => root.resultKey(event)
                        delegate: EpgSearchResultDelegate {
                            id: resultDelegate
                            required property int index
                            width: results.width - 12
                            compact: root.compact
                            uiTransparency: root.uiTransparency
                            pointerHovered: resultPointer.hoveredHeader === resultDelegate
                            controller: root.controller
                            selected: resultKey === root.controller.selectedKey
                            showDetails: root.currentResults && selected && root.collapsedDetailsKey !== resultKey
                            actionsCurrent: root.controller.resultsCurrent && !root.inputBlocked
                            onChosen: root.chooseResult(index, resultKey)
                            onActivated: { root.controller.selectIndex(index); root.activateResult(resultKey) }
                            onTabRequested: backwards => root.handleTab(backwards)
                            onControlFocused: item => root.revealControl(item)
                        }
                        MouseArea {
                            id: resultPointer
                            anchors.fill: parent
                            z: 2
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton
                            property string firstKey: ""
                            property int firstIndex: -1
                            property string collapseKey: ""
                            property point firstPosition: Qt.point(0, 0)
                            readonly property EpgSearchResultDelegate hoveredHeader: containsMouse ? root.headerAt(mouseX, mouseY) : null
                            containmentMask: QtObject {
                                function contains(point: point): bool {
                                    // Catch the second press at the original position even
                                    // after expansion moves a different child beneath it.
                                    const distance = root.styleHints.mouseDoubleClickDistance
                                    const pending = clickWindow.running && resultPointer.firstIndex >= 0
                                        && Math.abs(point.x - resultPointer.firstPosition.x) <= distance
                                        && Math.abs(point.y - resultPointer.firstPosition.y) <= distance
                                    return pending || root.headerAt(point.x, point.y) !== null
                                }
                            }
                            onPressed: mouse => {
                                if (clickWindow.running
                                    && Math.abs(mouse.x - firstPosition.x) <= root.styleHints.mouseDoubleClickDistance
                                    && Math.abs(mouse.y - firstPosition.y) <= root.styleHints.mouseDoubleClickDistance) return
                                clickWindow.stop()
                                collapseKey = ""
                                const item = root.headerAt(mouse.x, mouse.y)
                                firstKey = item ? item.resultKey : ""
                                firstIndex = item ? results.indexAt(mouse.x, mouse.y + results.contentY) : -1
                                firstPosition = Qt.point(mouse.x, mouse.y)
                            }
                            onClicked: {
                                const item = results.itemAtIndex(firstIndex) as EpgSearchResultDelegate
                                if (root.currentResults && item && item.resultKey === firstKey) item.chosen()
                            }
                            onDoubleClicked: {
                                const item = results.itemAtIndex(firstIndex) as EpgSearchResultDelegate
                                if (root.interactive && root.currentResults && item && item.resultKey === firstKey) item.activated()
                                root.clearGesture()
                            }
                            onCanceled: {
                                root.clearGesture()
                            }
                            Timer {
                                id: clickWindow
                                interval: root.styleHints.mouseDoubleClickInterval
                                onTriggered: {
                                    const key = resultPointer.collapseKey
                                    root.clearGesture()
                                    root.collapseDetails(key)
                                }
                            }
                        }
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        Rectangle { anchors.fill: parent; color: "transparent"; border.color: Theme.borderStrong; visible: results.activeFocus }
                        onContentYChanged: {
                            if (root.currentResults && root.controller.hasMore && !root.controller.fetchingMore
                                && contentY + height >= contentHeight - 160) root.controller.fetchNextPage()
                        }
                        footer: Item {
                            Component.onCompleted: root.loadMoreAction = loadMoreButton
                            width: results.width
                            height: root.controller.fetchingMore || root.controller.hasMore ? 40 : 0
                            BusyIndicator { anchors.centerIn: parent; width: 30; height: 30; running: root.controller.fetchingMore }
                            SearchButton {
                                id: loadMoreButton
                                objectName: "ui.epgSearch.loadMore"
                                anchors.centerIn: parent
                                visible: root.controller.hasMore && !root.controller.fetchingMore
                                text: qsTr("Load more")
                                onActiveFocusChanged: if (activeFocus) root.revealControl(loadMoreButton)
                                onClicked: root.controller.fetchNextPage()
                            }
                        }
                    }
                    Column {
                        id: message
                        anchors.centerIn: parent
                        width: Math.max(0, parent.width - 24)
                        visible: !root.currentResults
                        spacing: 8
                        BusyIndicator { anchors.horizontalCenter: parent.horizontalCenter; width: 24; height: 24; running: root.controller.busy; visible: running }
                        Text {
                            width: parent.width
                            text: root.stateMessage()
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            horizontalAlignment: Text.AlignHCenter
                            color: Theme.textSecondary
                            font.pixelSize: root.compact ? 12 : 14
                        }
                        SearchButton {
                            id: retryButton
                            objectName: "ui.epgSearch.retry"
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: qsTr("Retry")
                            visible: root.controller.errorText.length > 0
                            onClicked: root.controller.retry()
                        }
                    }
                }
                RowLayout {
                    id: footer
                    Layout.fillWidth: true
                    visible: root.currentResults && (!root.compact || root.controller.errorText.length > 0)
                    spacing: 8
                    Text {
                        objectName: "ui.epgSearch.resultCount"
                        visible: !root.compact
                        Layout.fillWidth: true
                        text: root.controller.hasMore ? qsTr("Shown %1 results · more available").arg(results.count) : qsTr("Shown %1 results").arg(results.count)
                        color: Theme.textMuted
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    Text {
                        visible: root.controller.errorText.length > 0
                        Layout.maximumWidth: body.width * 0.45
                        text: root.controller.errorText
                        textFormat: Text.PlainText
                        color: Theme.danger
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    SearchButton {
                        id: currentRetryButton
                        objectName: "ui.epgSearch.retryCurrent"
                        visible: root.controller.errorText.length > 0
                        text: qsTr("Retry")
                        onClicked: root.controller.retry()
                    }
                }
            }
        }
    }
}
