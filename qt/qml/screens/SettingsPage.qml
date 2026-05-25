pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml
import OKILTV
import "../theme/Theme.js" as Theme

Item {
    id: root

    property bool overlayMode: false
    // qmllint disable unqualified
    readonly property var shell: shellController
    readonly property var settings: settingsController
    readonly property var app: appController
    readonly property var groups: settingsSourceGroupsModel
    readonly property var guideState: guideStateModel
    readonly property var portableRuntime: portableRuntimeController
    // qmllint enable unqualified
    property string activeSection: "appearance"
    property string diagnosticsText: ""
    property bool confirmDiscardVisible: false
    property bool confirmNoGroupsSelectedVisible: false
    property string pendingSectionAfterGroupWarning: ""
    property string pendingSectionAfterDiscard: ""
    property bool closeAfterGroupWarning: false
    property var sourcesPaneRef: null
    readonly property bool prefersWideOverlay: root.activeSection === "sources"
    readonly property string iconBasePath: "qrc:/resources/icons/"
    readonly property color railSurface: "#74070d12"
    readonly property color contentSurface: "#18091119"
    readonly property color sectionSurface: "#7a0b1117"
    readonly property color rowSurface: "#5a0c141b"
    readonly property color activeTabSurface: "#6e182127"
    readonly property color hoverTabSurface: "#42141b21"
    readonly property int contentMargin: root.shell.layoutBand === "compact" ? Theme.spacingM : Theme.spacingL
    readonly property int settingsColumnMaxWidth: root.shell.layoutBand === "compact" ? 600 : 700
    readonly property int preferredNarrowOverlayWidth: Theme.railWidth + root.settingsColumnMaxWidth + root.contentMargin * 2
    readonly property int shortFieldWidth: root.shell.layoutBand === "compact" ? 250 : 320
    readonly property int longFieldWidth: root.shell.layoutBand === "compact" ? 420 : 520
    readonly property int numericFieldWidth: root.shell.layoutBand === "compact" ? 164 : 180
    readonly property bool ffmpegToolsAvailable: root.settings.ffmpegToolsAvailable
    readonly property string ffmpegToolsUnavailableText: "ffmpeg/ffprobe not available in the system"
    readonly property var sections: [
        {
            id: "appearance",
            caption: "Appearance",
            iconFile: "appearance.svg",
            glyph: "A",
            title: "Appearance",
            description: "Overlay behavior and browsing defaults for the live shell."
        },
        {
            id: "epg",
            caption: "EPG",
            iconFile: "epg.svg",
            glyph: "E",
            title: "EPG",
            description: "Guide refresh cadence and time window configuration."
        },
        {
            id: "sources",
            caption: "Sources",
            iconFile: "sources.svg",
            glyph: "S",
            title: "Sources",
            description: "Profiles, activation, refresh, and playlist credentials."
        },
        {
            id: "player",
            caption: "Player",
            iconFile: "player.svg",
            glyph: "P",
            title: "Player",
            description: "Playback tuning, mpv runtime override, and validation status."
        },
        {
            id: "timeshift",
            caption: "Timeshift",
            iconFile: "timeshift.svg",
            glyph: "TS",
            title: "Timeshift",
            description: "Live-buffer controls for pausing, rewinding, and retaining local HLS playback."
        },
        {
            id: "multiview",
            caption: "Multiview",
            iconFile: "multiview.svg",
            glyph: "MV",
            title: "Multiview",
            description: "PiP and grid layout controls, tile limits, and decoding preferences."
        },
        {
            id: "dvr",
            caption: "DVR",
            iconFile: "dvr.svg",
            glyph: "DV",
            title: "DVR",
            description: "Background recording schedule, offsets, and storage/remux behavior."
        },
        {
            id: "diagnostics",
            caption: "Diagnostics",
            iconFile: "diagnostics.svg",
            glyph: "D",
            title: "Diagnostics",
            description: "Runtime details for packaging and playback debugging, plus portable data-root controls when available."
        }
    ]
    readonly property bool saveEnabled: root.activeSection === "sources"
        ? (!!root.sourcesPaneRef && root.sourcesPaneRef.dirty)
        : (root.activeSection !== "diagnostics" && root.settings.dirty)

    function refreshDiagnostics() {
        diagnosticsText = root.app.debugSummary()
    }

    function iconPath(fileName) {
        return root.iconBasePath + fileName
    }

    function parseDecimalInput(rawText) {
        const normalized = rawText.trim().replace(",", ".")
        if (normalized.length === 0) {
            return NaN
        }
        const parsed = Number(normalized)
        return Number.isNaN(parsed) ? NaN : parsed
    }

    function decimalInputInRange(rawText, minimumValue, maximumValue) {
        const parsed = root.parseDecimalInput(rawText)
        return !Number.isNaN(parsed) && parsed >= minimumValue && parsed <= maximumValue
    }

    function formatDecimalInput(value) {
        const parsed = Number(value)
        if (Number.isNaN(parsed)) {
            return "0.0"
        }
        return parsed.toFixed(1)
    }

    function normalizeSection(sectionId) {
        for (let i = 0; i < root.sections.length; ++i) {
            if (root.sections[i].id === sectionId) {
                return sectionId
            }
        }
        return "appearance"
    }

    function sectionMeta(sectionId) {
        const normalizedSection = normalizeSection(sectionId)
        for (let i = 0; i < root.sections.length; ++i) {
            const section = root.sections[i]
            if (section.id === normalizedSection) {
                return section
            }
        }
        return root.sections[0]
    }

    function syncOverlaySection() {
        if (!root.overlayMode) {
            return
        }

        const nextSection = normalizeSection(root.shell.overlaySection)
        if (root.activeSection !== nextSection) {
            root.activeSection = nextSection
        }
    }

    function finalizeClose() {
        root.settings.cancel()
        if (root.sourcesPaneRef) {
            root.sourcesPaneRef.discardDraftChanges()
        }
        root.confirmDiscardVisible = false
        root.pendingSectionAfterDiscard = ""
        root.shell.clearOverlay()
    }

    function discardChangesAndContinue() {
        root.settings.cancel()
        if (root.sourcesPaneRef) {
            root.sourcesPaneRef.discardDraftChanges()
        }
        root.confirmDiscardVisible = false

        const nextSection = root.pendingSectionAfterDiscard
        root.pendingSectionAfterDiscard = ""
        if (nextSection.length > 0) {
            root.activeSection = nextSection
            return
        }

        root.shell.clearOverlay()
    }

    function requestClose() {
        root.pendingSectionAfterDiscard = ""
        if (root.activeSection === "sources"
            && root.groups.hasGroups
            && root.groups.selectedCount === 0) {
            root.pendingSectionAfterGroupWarning = ""
            root.closeAfterGroupWarning = true
            root.confirmNoGroupsSelectedVisible = true
            return
        }
        if (root.settings.dirty || (root.sourcesPaneRef && root.sourcesPaneRef.dirty)) {
            root.confirmDiscardVisible = true
            return
        }
        finalizeClose()
    }

    function requestSectionChange(sectionId) {
        const normalizedSection = normalizeSection(sectionId)
        if (normalizedSection === root.activeSection) {
            return
        }

        if (root.activeSection === "sources"
            && root.groups.hasGroups
            && root.groups.selectedCount === 0) {
            root.pendingSectionAfterGroupWarning = normalizedSection
            root.closeAfterGroupWarning = false
            root.confirmNoGroupsSelectedVisible = true
            return
        }

        if (root.activeSection === "sources"
            && root.sourcesPaneRef
            && root.sourcesPaneRef.dirty) {
            root.pendingSectionAfterDiscard = normalizedSection
            root.confirmDiscardVisible = true
            return
        }

        root.activeSection = normalizedSection
    }

    function continueAfterNoGroupsWarning() {
        const nextSection = root.pendingSectionAfterGroupWarning
        const shouldClose = root.closeAfterGroupWarning
        root.pendingSectionAfterGroupWarning = ""
        root.closeAfterGroupWarning = false
        root.confirmNoGroupsSelectedVisible = false
        if (shouldClose) {
            finalizeClose()
            return
        }
        if (nextSection.length > 0) {
            if (root.sourcesPaneRef && root.sourcesPaneRef.dirty) {
                root.pendingSectionAfterDiscard = nextSection
                root.confirmDiscardVisible = true
                return
            }
            root.activeSection = nextSection
        }
    }

    onActiveSectionChanged: {
        const normalizedSection = normalizeSection(root.activeSection)
        if (root.activeSection !== normalizedSection) {
            root.activeSection = normalizedSection
            return
        }

        if (root.overlayMode) {
            root.shell.overlaySection = normalizedSection
        }
        if (normalizedSection === "diagnostics") {
            root.refreshDiagnostics()
        }
    }

    Component.onCompleted: syncOverlaySection()

    Connections {
        target: root.shell

        function onOverlaySectionChanged() {
            root.syncOverlaySection()
        }
    }

    Connections {
        target: root.settings

        function onSettingsChanged() {
            root.guideState.previewEnabled = root.settings.guidePreviewEnabled
        }
    }

    Connections {
        target: root.portableRuntime

        function onStateChanged() {
            root.refreshDiagnostics()
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: !root.overlayMode
        color: Theme.window
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            objectName: "ui.region.settings_content"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0

            Rectangle {
                anchors.fill: parent
                color: root.contentSurface
            }

            Loader {
                anchors.fill: parent
                sourceComponent: root.activeSection === "sources" ? sourcesSection : standardSettingsSection
            }
        }

        Rectangle {
            id: rail
            objectName: "ui.region.settings_rail"

            Layout.preferredWidth: Theme.railWidth
            Layout.fillHeight: true
            color: root.railSurface

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: Theme.spacingM
                anchors.bottomMargin: Theme.spacingM
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: Theme.spacingS

                SettingsRailButton {
                    Layout.alignment: Qt.AlignHCenter
                    iconSource: root.iconPath("close.svg")
                    glyph: "X"
                    caption: "Close without saving"
                    activeFillColor: root.activeTabSurface
                    hoverFillColor: root.hoverTabSurface
                    onClicked: root.requestClose()
                }

                Item {
                    Layout.preferredHeight: Theme.spacingS
                }

                ScrollView {
                    Layout.fillHeight: true
                    Layout.fillWidth: true
                    clip: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    ColumnLayout {
                        width: rail.width - 16
                        spacing: Theme.spacingS

                        Repeater {
                            model: root.sections

                            delegate: SettingsRailButton {
                                required property var modelData

                                Layout.alignment: Qt.AlignHCenter
                                iconSource: root.iconPath(modelData.iconFile)
                                glyph: modelData.glyph
                                caption: modelData.caption
                                active: root.activeSection === modelData.id
                                activeFillColor: root.activeTabSurface
                                hoverFillColor: root.hoverTabSurface
                                onClicked: root.requestSectionChange(modelData.id)
                            }
                        }
                    }
                }

                SettingsRailButton {
                    Layout.alignment: Qt.AlignHCenter
                    iconSource: root.iconPath("save.svg")
                    glyph: "SV"
                    caption: root.activeSection === "sources"
                        ? (root.saveEnabled ? "Save source changes" : "No unsaved source changes")
                        : (root.activeSection === "diagnostics"
                            ? "Diagnostics has nothing to save"
                            : (root.settings.dirty ? "Save settings" : "No unsaved changes"))
                    active: root.saveEnabled
                    accent: root.saveEnabled
                    enabled: root.saveEnabled
                    activeFillColor: root.activeTabSurface
                    hoverFillColor: root.hoverTabSurface
                    onClicked: {
                        if (root.activeSection === "sources") {
                            if (root.sourcesPaneRef) {
                                root.sourcesPaneRef.saveAllChanges()
                            }
                            return
                        }
                        root.settings.save()
                    }
                }
            }
        }
    }

    Component {
        id: sourcesSection

        Item {
            anchors.fill: parent

            SourceManagerPane {
                id: sourcesPane
                anchors.fill: parent
                anchors.margins: root.contentMargin
            }

            Component.onCompleted: root.sourcesPaneRef = sourcesPane
            Component.onDestruction: {
                if (root.sourcesPaneRef === sourcesPane) {
                    root.sourcesPaneRef = null
                }
            }
        }
    }

    Component {
        id: standardSettingsSection

        Item {
            anchors.fill: parent

            ScrollView {
                id: settingsScroll
                anchors.fill: parent
                anchors.margins: root.contentMargin
                clip: true

                ColumnLayout {
                    width: Math.min(settingsScroll.availableWidth, root.settingsColumnMaxWidth)
                    spacing: Theme.spacingM

                    OverlaySectionPanel {
                        Layout.fillWidth: true
                        panelColor: root.sectionSurface
                        panelSpacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingM

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4

                                Text {
                                    text: root.sectionMeta(root.activeSection).title
                                    color: Theme.textPrimary
                                    font.pixelSize: root.shell.layoutBand === "compact" ? 28 : 32
                                    font.bold: true
                                    renderType: Text.NativeRendering
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: root.sectionMeta(root.activeSection).description
                                    color: Theme.textSecondary
                                    font.pixelSize: 13
                                    wrapMode: Text.Wrap
                                }
                            }

                            Text {
                                text: root.activeSection === "diagnostics"
                                    ? (root.portableRuntime.portableModeEnabled ? "RUNTIME CONTROLS" : "READ ONLY")
                                    : (root.settings.dirty ? "UNSAVED CHANGES" : "ALL CHANGES SAVED")
                                color: root.settings.dirty ? Theme.textPrimary : Theme.textSecondary
                                font.pixelSize: 11
                                font.bold: true
                                horizontalAlignment: Text.AlignRight
                                verticalAlignment: Text.AlignVCenter
                                renderType: Text.NativeRendering
                            }
                        }
                    }

                    Loader {
                        Layout.fillWidth: true
                        sourceComponent: {
                            if (root.activeSection === "appearance") {
                                return appearanceSection
                            }
                            if (root.activeSection === "epg") {
                                return epgSection
                            }
                            if (root.activeSection === "player") {
                                return playerSection
                            }
                            if (root.activeSection === "timeshift") {
                                return timeshiftSection
                            }
                            if (root.activeSection === "multiview") {
                                return multiviewSection
                            }
                            if (root.activeSection === "dvr") {
                                return dvrSection
                            }
                            return diagnosticsSection
                        }
                    }
                }
            }
        }
    }

    Component {
        id: appearanceSection

        ColumnLayout {
            width: parent ? parent.width : 0
            spacing: Theme.spacingM

            OverlaySectionPanel {
                Layout.fillWidth: true
                panelColor: root.sectionSurface
                panelSpacing: Theme.spacingS

                Text {
                    text: "Behavior"
                    color: Theme.textPrimary
                    font.pixelSize: 18
                    font.bold: true
                    renderType: Text.NativeRendering
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 76

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Auto-hide live overlays"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Hide the live chrome after inactivity in both windowed and fullscreen modes."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSwitch {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            checked: root.settings.overlayAutoHide
                            onToggled: root.settings.overlayAutoHide = checked
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 76

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Prevent screen sleep"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Keep the display awake while video is playing and the app window is not minimized."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSwitch {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            checked: root.settings.preventDisplaySleep
                            onToggled: root.settings.preventDisplaySleep = checked
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 76

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Show on-top mode indicator"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Show a small top-left icon while always-on-top mode is active."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSwitch {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            checked: root.settings.showOnTopModeIndicator
                            onToggled: root.settings.showOnTopModeIndicator = checked
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 76
                    visible: root.settings.overlayAutoHide

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Overlay auto-hide delay"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Seconds of inactivity before live overlays hide."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSpinBox {
                            id: overlayAutoHideSecondsField

                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            Layout.preferredWidth: root.numericFieldWidth
                            from: 1
                            to: 30
                            onValueChanged: root.settings.overlayAutoHideSeconds = value

                            Binding {
                                target: overlayAutoHideSecondsField
                                property: "value"
                                value: root.settings.overlayAutoHideSeconds
                                when: !overlayAutoHideSecondsField.editing
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 76

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Guide preview"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Keep browsing state separate from playback until tune is confirmed."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSwitch {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            checked: root.settings.guidePreviewEnabled
                            onToggled: root.settings.guidePreviewEnabled = checked
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 76

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Minimize to tray"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "When minimizing the window, hide to tray instead of staying in the taskbar. Closing the window still exits."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSwitch {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            checked: root.settings.minimizeToTrayOnMinimize
                            onToggled: root.settings.minimizeToTrayOnMinimize = checked
                        }
                    }
                }
            }
        }
    }

    Component {
        id: epgSection

        ColumnLayout {
            width: parent ? parent.width : 0
            spacing: Theme.spacingM

            OverlaySectionPanel {
                Layout.fillWidth: true
                panelColor: root.sectionSurface
                panelSpacing: Theme.spacingS

                Text {
                    text: "Guide Window"
                    color: Theme.textPrimary
                    font.pixelSize: 18
                    font.bold: true
                    renderType: Text.NativeRendering
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 82

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Refresh EPG now"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: root.app.activeProfileId.length > 0
                                    ? "Last successful refresh: " + root.app.epgLastRefreshText
                                    : "Load a source first to refresh EPG data."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        Item {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            Layout.preferredWidth: 116
                            Layout.preferredHeight: 40

                            BusyIndicator {
                                anchors.centerIn: parent
                                width: 34
                                height: 34
                                running: root.app.epgRefreshInProgress
                                opacity: root.app.epgRefreshInProgress ? 1 : 0
                            }

                            AppButton {
                                anchors.fill: parent
                                visible: !root.app.epgRefreshInProgress
                                text: "Refresh Now"
                                accent: true
                                enabled: root.app.activeProfileId.length > 0
                                onClicked: root.app.refreshActiveEpg()
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 80

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Refresh interval"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Minutes between automatic source refreshes."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSpinBox {
                            id: refreshIntervalField

                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            Layout.preferredWidth: root.numericFieldWidth
                            from: 1
                            to: 1440
                            onValueChanged: root.settings.refreshIntervalMinutes = value

                            Binding {
                                target: refreshIntervalField
                                property: "value"
                                value: root.settings.refreshIntervalMinutes
                                when: !refreshIntervalField.editing
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 124

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingM

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4

                                Text {
                                    text: "EPG look-back"
                                    color: Theme.textPrimary
                                    font.pixelSize: 14
                                    font.bold: true
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: "Hours of older guide data to keep visible to the left of the current hour."
                                    color: Theme.textSecondary
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                            }

                            FormSpinBox {
                                id: guidePastField

                                Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                                Layout.preferredWidth: root.numericFieldWidth
                                from: 1
                                to: 48
                                onValueChanged: root.settings.guidePastHours = value

                                Binding {
                                    target: guidePastField
                                    property: "value"
                                    value: root.settings.guidePastHours
                                    when: !guidePastField.editing
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingM

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4

                                Text {
                                    text: "EPG look-ahead"
                                    color: Theme.textPrimary
                                    font.pixelSize: 14
                                    font.bold: true
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: "Hours of upcoming guide data to keep visible in the full EPG grid."
                                    color: Theme.textSecondary
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                            }

                            FormSpinBox {
                                id: epgLookAheadField

                                Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                                Layout.preferredWidth: root.numericFieldWidth
                                from: 1
                                to: 48
                                onValueChanged: root.settings.epgLookAheadHours = value

                                Binding {
                                    target: epgLookAheadField
                                    property: "value"
                                    value: root.settings.epgLookAheadHours
                                    when: !epgLookAheadField.editing
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 76

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Automatic EPG refresh"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Use scheduled updates for the active profiles."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSwitch {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            checked: root.settings.autoRefreshEpg
                            onToggled: root.settings.autoRefreshEpg = checked
                        }
                    }
                }
            }
        }
    }

    Component {
        id: playerSection

        ColumnLayout {
            width: parent ? parent.width : 0
            spacing: Theme.spacingM

            OverlaySectionPanel {
                Layout.fillWidth: true
                panelColor: root.sectionSurface
                panelSpacing: Theme.spacingS

                Text {
                    text: "Playback Tuning"
                    color: Theme.textPrimary
                    font.pixelSize: 18
                    font.bold: true
                    renderType: Text.NativeRendering
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 80

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Wait for data stream"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Seconds to wait for stream data before terminating playback."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormTextField {
                            id: waitForStreamField

                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            Layout.preferredWidth: root.numericFieldWidth
                            inputMethodHints: Qt.ImhFormattedNumbersOnly
                            validator: RegularExpressionValidator {
                                regularExpression: /^\d{0,3}([.,]\d?)?$/
                            }
                            onTextEdited: {
                                if (root.decimalInputInRange(text, 0.1, 120.0)) {
                                    const parsed = root.parseDecimalInput(text)
                                    root.settings.waitForDataStreamSeconds = parsed
                                }
                            }
                            onEditingFinished: {
                                if (root.decimalInputInRange(text, 0.1, 120.0)) {
                                    root.settings.waitForDataStreamSeconds = root.parseDecimalInput(text)
                                }
                                text = root.formatDecimalInput(root.settings.waitForDataStreamSeconds)
                            }

                            Binding {
                                target: waitForStreamField
                                property: "text"
                                value: root.formatDecimalInput(root.settings.waitForDataStreamSeconds)
                                when: !waitForStreamField.activeFocus
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 76

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Deinterlacing"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Enable deinterlacing filter for interlaced video sources."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSwitch {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            checked: root.settings.deinterlaceEnabled
                            onToggled: root.settings.deinterlaceEnabled = checked
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 80

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Buffer size"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Seconds of playback buffer used to absorb short source stutters."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormTextField {
                            id: bufferSizeField

                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            Layout.preferredWidth: root.numericFieldWidth
                            inputMethodHints: Qt.ImhFormattedNumbersOnly
                            validator: RegularExpressionValidator {
                                regularExpression: /^\d{0,2}([.,]\d?)?$/
                            }
                            onTextEdited: {
                                if (root.decimalInputInRange(text, 0.1, 60.0)) {
                                    const parsed = root.parseDecimalInput(text)
                                    root.settings.bufferSizeSeconds = parsed
                                }
                            }
                            onEditingFinished: {
                                if (root.decimalInputInRange(text, 0.1, 60.0)) {
                                    root.settings.bufferSizeSeconds = root.parseDecimalInput(text)
                                }
                                text = root.formatDecimalInput(root.settings.bufferSizeSeconds)
                            }

                            Binding {
                                target: bufferSizeField
                                property: "text"
                                value: root.formatDecimalInput(root.settings.bufferSizeSeconds)
                                when: !bufferSizeField.activeFocus
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 80

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "User-Agent"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Optional HTTP User-Agent header sent with stream requests."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormTextField {
                            id: playerUserAgentField

                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            Layout.preferredWidth: Math.max(260, root.numericFieldWidth + 140)
                            placeholderText: "Default mpv User-Agent"
                            onTextEdited: root.settings.playerUserAgent = text

                            Binding {
                                target: playerUserAgentField
                                property: "text"
                                value: root.settings.playerUserAgent
                                when: !playerUserAgentField.activeFocus
                            }
                        }
                    }
                }
            }

            OverlaySectionPanel {
                Layout.fillWidth: true
                panelColor: root.sectionSurface

                Text {
                    text: "Capture & Recording"
                    color: Theme.textPrimary
                    font.pixelSize: 18
                    font.bold: true
                    renderType: Text.NativeRendering
                }

                Text {
                    text: "Screenshots directory"
                    color: Theme.textSecondary
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }

                PathBrowseField {
                    id: screenshotsDirField

                    Layout.fillWidth: true
                    fillFieldWidth: true
                    folderMode: true
                    placeholderText: "Default: screenshots/ in app data"
                    onTextEdited: function(text) { root.settings.screenshotsDirectory = text }

                    Binding {
                        target: screenshotsDirField
                        property: "text"
                        value: root.settings.screenshotsDirectory
                        when: !screenshotsDirField.fieldActiveFocus
                    }
                }

                Text {
                    text: "Recordings directory"
                    color: Theme.textSecondary
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }

                PathBrowseField {
                    id: recordingsDirField

                    Layout.fillWidth: true
                    fillFieldWidth: true
                    folderMode: true
                    placeholderText: "Default: recordings/ in app data"
                    onTextEdited: function(text) { root.settings.recordingsDirectory = text }

                    Binding {
                        target: recordingsDirField
                        property: "text"
                        value: root.settings.recordingsDirectory
                        when: !recordingsDirField.fieldActiveFocus
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 76

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Remux to MKV"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: root.ffmpegToolsAvailable
                                    ? "After recording stops, remux the raw stream to an MKV container for better playback compatibility."
                                    : root.ffmpegToolsUnavailableText
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSwitch {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            enabled: root.ffmpegToolsAvailable
                            checked: root.settings.remuxRecordingsToMkv
                            onToggled: root.settings.remuxRecordingsToMkv = checked
                        }
                    }
                }
            }

            OverlaySectionPanel {
                Layout.fillWidth: true
                panelColor: root.sectionSurface

                Text {
                    text: "mpv Runtime"
                    color: Theme.textPrimary
                    font.pixelSize: 18
                    font.bold: true
                    renderType: Text.NativeRendering
                }

                PathBrowseField {
                    id: mpvPathField

                    Layout.fillWidth: true
                    fillFieldWidth: true
                    placeholderText: "Optional mpv DLL override"
                    nameFilters: ["Dynamic libraries (*.dll *.so *.dylib)", "All files (*)"]
                    onTextEdited: function(text) { root.settings.mpvDllPath = text }

                    Binding {
                        target: mpvPathField
                        property: "text"
                        value: root.settings.mpvDllPath
                        when: !mpvPathField.fieldActiveFocus
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: statusTextBlock.implicitHeight + 28

                    Text {
                        id: statusTextBlock
                        anchors.fill: parent
                        anchors.margins: 14
                        text: root.settings.mpvPathValidationStatus
                        color: Theme.textSecondary
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }

                RowLayout {
                    spacing: Theme.spacingS

                    AppButton {
                        text: "Use Bundled mpv"
                        onClicked: root.settings.useBundledMpv()
                    }

                    AppButton {
                        text: "Validate Path"
                        onClicked: root.settings.validateMpvPath()
                    }
                }
            }
        }
    }

    Component {
        id: timeshiftSection

        ColumnLayout {
            width: parent ? parent.width : 0
            spacing: Theme.spacingM

            OverlaySectionPanel {
                Layout.fillWidth: true
                panelColor: root.sectionSurface
                panelSpacing: Theme.spacingS

                Text {
                    text: "Live Timeshift"
                    color: Theme.textPrimary
                    font.pixelSize: 18
                    font.bold: true
                    renderType: Text.NativeRendering
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 80

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Enable live timeshift"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: root.ffmpegToolsAvailable
                                    ? "Pause, rewind, and scrub live playback inside a rolling local buffer."
                                    : root.ffmpegToolsUnavailableText
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSwitch {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            enabled: root.ffmpegToolsAvailable
                            checked: root.settings.timeshiftEnabled
                            onToggled: root.settings.timeshiftEnabled = checked
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 80

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Window (minutes)"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Rolling live buffer length. Changes apply on the next stream."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSpinBox {
                            id: timeshiftWindowField

                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            Layout.preferredWidth: root.numericFieldWidth
                            enabled: root.ffmpegToolsAvailable
                            from: 15
                            to: 360
                            onValueChanged: root.settings.timeshiftWindowMinutes = value

                            Binding {
                                target: timeshiftWindowField
                                property: "value"
                                value: root.settings.timeshiftWindowMinutes
                                when: !timeshiftWindowField.editing
                            }
                        }
                    }
                }

                Text {
                    text: "Storage directory"
                    color: Theme.textSecondary
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }

                PathBrowseField {
                    id: timeshiftStorageDirField

                    Layout.fillWidth: true
                    fillFieldWidth: true
                    enabled: root.ffmpegToolsAvailable
                    folderMode: true
                    placeholderText: "Default: timeshift/ in app data"
                    onTextEdited: function(text) { root.settings.timeshiftStorageDirectory = text }

                    Binding {
                        target: timeshiftStorageDirField
                        property: "text"
                        value: root.settings.timeshiftStorageDirectory
                        when: !timeshiftStorageDirField.fieldActiveFocus
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 80

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Max disk (GB)"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: root.ffmpegToolsAvailable
                                    ? "Admission quota for starting a live timeshift session. Changes apply on the next stream."
                                    : root.ffmpegToolsUnavailableText
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSpinBox {
                            id: timeshiftDiskField

                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            Layout.preferredWidth: root.numericFieldWidth
                            enabled: root.ffmpegToolsAvailable
                            from: 1
                            to: 128
                            onValueChanged: root.settings.timeshiftMaxDiskGb = value

                            Binding {
                                target: timeshiftDiskField
                                property: "value"
                                value: root.settings.timeshiftMaxDiskGb
                                when: !timeshiftDiskField.editing
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: multiviewSection

        ColumnLayout {
            width: parent ? parent.width : 0
            spacing: Theme.spacingM

            OverlaySectionPanel {
                Layout.fillWidth: true
                panelColor: root.sectionSurface
                panelSpacing: Theme.spacingS

                Text {
                    text: "Multiview"
                    color: Theme.textPrimary
                    font.pixelSize: 18
                    font.bold: true
                    renderType: Text.NativeRendering
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 80

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Enable multiview"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Allow PiP and grid live-channel layouts. PiP always keeps audio on the main screen."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSwitch {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            checked: root.settings.multiviewEnabled
                            onToggled: root.settings.multiviewEnabled = checked
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 80

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Maximum tiles"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Hard cap for simultaneously open tiles across PiP and grid layouts."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormComboBox {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            Layout.preferredWidth: Math.max(200, root.numericFieldWidth + 24)
                            enabled: root.settings.multiviewEnabled
                            model: ["2", "4", "6", "9", "12"]
                            currentIndex: {
                                switch (root.settings.multiviewMaxTiles) {
                                case 2:
                                    return 0
                                case 4:
                                    return 1
                                case 6:
                                    return 2
                                case 9:
                                    return 3
                                case 12:
                                    return 4
                                default:
                                    return 1
                                }
                            }
                            onActivated: root.settings.multiviewMaxTiles = Number(model[currentIndex])
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 80

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Prefer hardware decoding"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Keep hardware decode enabled for multiview tiles unless an explicit mpv override disables it."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSwitch {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            enabled: root.settings.multiviewEnabled
                            checked: root.settings.multiviewPreferHwdec
                            onToggled: root.settings.multiviewPreferHwdec = checked
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 80

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Retain multiview selection on channel promotion"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "When exiting grid with Ctrl+O, keep retained tile streams running in the background. Use Ctrl+Alt+O for explicit full cleanup."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSwitch {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            enabled: root.settings.multiviewEnabled
                            checked: root.settings.multiviewRetainSelectionOnPromotion
                            onToggled: root.settings.multiviewRetainSelectionOnPromotion = checked
                        }
                    }
                }
            }
        }
    }

    Component {
        id: dvrSection

        ColumnLayout {
            width: parent ? parent.width : 0
            spacing: Theme.spacingM

            OverlaySectionPanel {
                Layout.fillWidth: true
                panelColor: root.sectionSurface

                Text {
                    text: "Storage"
                    color: Theme.textPrimary
                    font.pixelSize: 18
                    font.bold: true
                    renderType: Text.NativeRendering
                }

                Text {
                    text: "DVR recordings directory"
                    color: Theme.textSecondary
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }

                PathBrowseField {
                    id: dvrRecordingsDirField

                    Layout.fillWidth: true
                    fillFieldWidth: true
                    folderMode: true
                    placeholderText: "Default: recordings/ in app data"
                    onTextEdited: function(text) { root.settings.dvrRecordingsDirectory = text }

                    Binding {
                        target: dvrRecordingsDirField
                        property: "text"
                        value: root.settings.dvrRecordingsDirectory
                        when: !dvrRecordingsDirField.fieldActiveFocus
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 76

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Remux to MKV"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: root.ffmpegToolsAvailable
                                    ? "After DVR capture stops, remux transport stream output to MKV."
                                    : root.ffmpegToolsUnavailableText
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSwitch {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            enabled: root.ffmpegToolsAvailable
                            checked: root.settings.dvrRemuxToMkv
                            onToggled: root.settings.dvrRemuxToMkv = checked
                        }
                    }
                }
            }

            OverlaySectionPanel {
                Layout.fillWidth: true
                panelColor: root.sectionSurface
                panelSpacing: Theme.spacingS

                Text {
                    text: "Recording Window"
                    color: Theme.textPrimary
                    font.pixelSize: 18
                    font.bold: true
                    renderType: Text.NativeRendering
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 80

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "Start offset (minutes)"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Positive starts early. Negative starts late."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSpinBox {
                            id: dvrStartOffsetField

                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            Layout.preferredWidth: root.numericFieldWidth
                            from: -120
                            to: 120
                            onValueChanged: root.settings.dvrStartOffsetMinutes = value

                            Binding {
                                target: dvrStartOffsetField
                                property: "value"
                                value: root.settings.dvrStartOffsetMinutes
                                when: !dvrStartOffsetField.editing
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusM
                    color: root.rowSurface
                    border.width: 0
                    implicitHeight: 80

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                text: "End offset (minutes)"
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Positive extends recording. Negative ends early."
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                        }

                        FormSpinBox {
                            id: dvrEndOffsetField

                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            Layout.preferredWidth: root.numericFieldWidth
                            from: -120
                            to: 120
                            onValueChanged: root.settings.dvrEndOffsetMinutes = value

                            Binding {
                                target: dvrEndOffsetField
                                property: "value"
                                value: root.settings.dvrEndOffsetMinutes
                                when: !dvrEndOffsetField.editing
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: diagnosticsSection

        ColumnLayout {
            width: parent ? parent.width : 0
            spacing: Theme.spacingM

            OverlaySectionPanel {
                Layout.fillWidth: true
                panelColor: root.sectionSurface

                ColumnLayout {
                    width: parent ? parent.width : 0
                    spacing: Theme.spacingM

                    RowLayout {
                        Layout.fillWidth: true

                        Text {
                            Layout.fillWidth: true
                            text: "Use this when a packaged build misbehaves on Windows or a profile loads unexpectedly."
                            color: Theme.textSecondary
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }

                        AppButton {
                            text: "Refresh"
                            onClicked: root.refreshDiagnostics()
                        }

                        AppButton {
                            text: "Dump To File"
                            onClicked: {
                                root.app.dumpDebugReport()
                                root.refreshDiagnostics()
                            }
                        }
                    }

                    OverlaySectionPanel {
                        Layout.fillWidth: true
                        panelColor: root.rowSurface
                        visible: root.portableRuntime.portableModeEnabled

                        Text {
                            text: "Portable Data Root"
                            color: Theme.textPrimary
                            font.pixelSize: 18
                            font.bold: true
                            renderType: Text.NativeRendering
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "Portable builds still default to %APPDATA%\\OKILTV. Set an absolute override here to move settings, cache, and the database after restart."
                            color: Theme.textSecondary
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "Effective path: " + root.portableRuntime.effectiveDataRoot
                            color: Theme.textPrimary
                            font.pixelSize: 12
                            wrapMode: Text.WrapAnywhere
                        }

                        PathBrowseField {
                            id: portableDataRootField

                            Layout.fillWidth: true
                            fillFieldWidth: true
                            folderMode: true
                            placeholderText: "Absolute directory path"
                            onTextEdited: function(text) { root.portableRuntime.customDataRoot = text }

                            Binding {
                                target: portableDataRootField
                                property: "text"
                                value: root.portableRuntime.customDataRoot
                                when: !portableDataRootField.fieldActiveFocus
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: root.portableRuntime.dataRootStatus
                            color: Theme.textSecondary
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }

                        RowLayout {
                            Layout.fillWidth: true

                            AppButton {
                                text: "Reset to APPDATA"
                                onClicked: root.portableRuntime.resetDataRootOverride()
                            }

                            AppButton {
                                text: "Apply and Restart"
                                onClicked: root.portableRuntime.applyCustomDataRootAndRestart()
                            }

                            Item {
                                Layout.fillWidth: true
                            }

                            Text {
                                text: root.portableRuntime.restartRequired ? "Restart required" : ""
                                color: Theme.textPrimary
                                font.pixelSize: 11
                                font.bold: true
                                visible: root.portableRuntime.restartRequired
                            }
                        }
                    }
                }

                TextArea {
                    Layout.fillWidth: true
                    Layout.preferredHeight: root.shell.layoutBand === "compact" ? 500 : 620
                    text: root.diagnosticsText
                    readOnly: true
                    wrapMode: TextEdit.WrapAnywhere
                    color: Theme.textPrimary
                    background: Rectangle {
                        radius: Theme.radiusM
                        color: root.rowSurface
                        border.width: 0
                    }
                }
            }
        }
    }

    Item {
        anchors.fill: parent
        visible: root.confirmDiscardVisible
        z: 40

        Rectangle {
            anchors.fill: parent
            color: "#52000000"
        }

        MouseArea {
            anchors.fill: parent
            onClicked: {}
        }

        Rectangle {
            width: Math.min(460, parent.width - Theme.spacingXL * 2)
            anchors.centerIn: parent
            color: root.sectionSurface
            radius: Theme.radiusL
            border.width: 0
            implicitHeight: dialogContent.implicitHeight + Theme.spacingL * 2

            ColumnLayout {
                id: dialogContent

                x: Theme.spacingL
                y: Theme.spacingL
                width: parent.width - Theme.spacingL * 2
                spacing: Theme.spacingM

                Text {
                    Layout.fillWidth: true
                    text: "Discard unsaved settings?"
                    color: Theme.textPrimary
                    font.pixelSize: 24
                    font.bold: true
                    wrapMode: Text.Wrap
                    renderType: Text.NativeRendering
                }

                Text {
                    Layout.fillWidth: true
                    text: "You have unsaved changes. Discard them and continue, or keep editing."
                    color: Theme.textSecondary
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacingS

                    Item {
                        Layout.fillWidth: true
                    }

                    AppButton {
                        text: "Keep Editing"
                        onClicked: {
                            root.confirmDiscardVisible = false
                            root.pendingSectionAfterDiscard = ""
                        }
                    }

                    AppButton {
                        text: "Discard"
                        accent: true
                        onClicked: root.discardChangesAndContinue()
                    }
                }
            }
        }
    }

    Item {
        anchors.fill: parent
        visible: root.confirmNoGroupsSelectedVisible
        z: 41

        Rectangle {
            anchors.fill: parent
            color: "#52000000"
        }

        MouseArea {
            anchors.fill: parent
            onClicked: {}
        }

        Rectangle {
            width: Math.min(500, parent.width - Theme.spacingXL * 2)
            anchors.centerIn: parent
            color: root.sectionSurface
            radius: Theme.radiusL
            border.width: 0
            implicitHeight: noGroupsDialogContent.implicitHeight + Theme.spacingL * 2

            ColumnLayout {
                id: noGroupsDialogContent

                x: Theme.spacingL
                y: Theme.spacingL
                width: parent.width - Theme.spacingL * 2
                spacing: Theme.spacingM

                Text {
                    Layout.fillWidth: true
                    text: "Leave with no groups selected?"
                    color: Theme.textPrimary
                    font.pixelSize: 24
                    font.bold: true
                    wrapMode: Text.Wrap
                    renderType: Text.NativeRendering
                }

                Text {
                    Layout.fillWidth: true
                    text: "This source has imported groups, but none are selected for use. Leave the Sources screen anyway, or keep editing and choose at least one group."
                    color: Theme.textSecondary
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacingS

                    Item {
                        Layout.fillWidth: true
                    }

                    AppButton {
                        text: "Keep Editing"
                        onClicked: {
                            root.pendingSectionAfterGroupWarning = ""
                            root.closeAfterGroupWarning = false
                            root.confirmNoGroupsSelectedVisible = false
                        }
                    }

                    AppButton {
                        text: "Leave Anyway"
                        accent: true
                        onClicked: root.continueAfterNoGroupsWarning()
                    }
                }
            }
        }
    }
}
