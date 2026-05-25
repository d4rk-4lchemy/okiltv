pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OKILTV
import "../theme/Theme.js" as Theme

Item {
    id: root

    property int selectedIndex: -1
    // qmllint disable unqualified
    readonly property var profiles: profilesModel
    readonly property var groups: settingsSourceGroupsModel
    readonly property var liveGroups: liveSourceGroupsModel
    readonly property var app: appController
    readonly property var channelList: channelListModel
    readonly property var shell: shellController
    // qmllint enable unqualified
    property bool createMode: root.profiles.rowCount() === 0
    property int draftType: 0
    property bool confirmDeleteVisible: false
    property string pendingDeleteProfileId: ""
    property string pendingDeleteProfileName: ""
    property var pendingProfileDrafts: ({})
    property var pendingCreateDraftsByType: ({})
    property var pendingNewSourceProfileIds: []
    property var refreshProfileQueue: []
    property bool refreshQueueInFlight: false
    property bool groupDragActive: false
    property int dirtyRevision: 0
    readonly property int sidebarWidth: root.shell.layoutBand === "compact" ? 292 : 324
    readonly property color sidebarSurface: "#74070d12"
    readonly property color sectionSurface: "#7a0b1117"
    readonly property color rowSurface: "#5a0c141b"
    readonly property color selectedRowSurface: "#6e182127"
    readonly property bool groupReorderEnabled: root.groups.hasGroups
        && root.groups.searchText.trim().length === 0
    readonly property bool dirty: {
        const revision = root.dirtyRevision
        void revision
        return root.currentEditorHasUnsavedChanges()
            || Object.keys(root.pendingProfileDrafts).length > 0
            || root.createDraftMapHasMeaningfulValues(root.pendingCreateDraftsByType)
            || root.groups.dirty
    }

    function markDirtyStateChanged() {
        root.dirtyRevision += 1
    }

    function normalizedAutoRefreshIntervalHours(profileValue) {
        if (profileValue === undefined || profileValue === null) {
            return 24
        }
        const parsed = Number(profileValue)
        if (Number.isNaN(parsed)) {
            return 24
        }
        return Math.max(0, Math.round(parsed))
    }

    function normalizedDraftType(typeValue) {
        const parsed = Number(typeValue)
        if (parsed === 1 || parsed === 2) {
            return parsed
        }
        return 0
    }

    function normalizedText(value) {
        return (value || "").trim()
    }

    function buildDraftFromProfile(profile) {
        return {
            "type": normalizedDraftType(profile.type),
            "name": normalizedText(profile.name),
            "xtreamBaseUrl": normalizedText(profile.xtreamBaseUrl),
            "xtreamUsername": normalizedText(profile.xtreamUsername),
            "xtreamPassword": normalizedText(profile.xtreamPassword),
            "m3UUrl": normalizedText(profile.m3UUrl),
            "m3UFilePath": normalizedText(profile.m3UFilePath),
            "xmltvUrl": normalizedText(profile.xmltvUrl),
            "autoRefreshIntervalHours": normalizedAutoRefreshIntervalHours(profile.autoRefreshIntervalHours)
        }
    }

    function buildDraftFromForm() {
        return {
            "type": normalizedDraftType(root.draftType),
            "name": normalizedText(nameField.text),
            "xtreamBaseUrl": normalizedText(xtreamUrlField.text),
            "xtreamUsername": normalizedText(xtreamUserField.text),
            "xtreamPassword": normalizedText(xtreamPasswordField.text),
            "m3UUrl": normalizedText(m3uUrlField.text),
            "m3UFilePath": normalizedText(m3uFileField.text),
            "xmltvUrl": normalizedText(xmltvField.text),
            "autoRefreshIntervalHours": normalizedAutoRefreshIntervalHours(autoRefreshIntervalHoursField.value)
        }
    }

    function createDefaultDraftForType(typeValue) {
        return {
            "type": normalizedDraftType(typeValue),
            "name": "",
            "xtreamBaseUrl": "",
            "xtreamUsername": "",
            "xtreamPassword": "",
            "m3UUrl": "",
            "m3UFilePath": "",
            "xmltvUrl": "",
            "autoRefreshIntervalHours": 24
        }
    }

    function draftEquals(left, right) {
        if (!left || !right) {
            return false
        }

        return normalizedDraftType(left.type) === normalizedDraftType(right.type)
            && normalizedText(left.name) === normalizedText(right.name)
            && normalizedText(left.xtreamBaseUrl) === normalizedText(right.xtreamBaseUrl)
            && normalizedText(left.xtreamUsername) === normalizedText(right.xtreamUsername)
            && normalizedText(left.xtreamPassword) === normalizedText(right.xtreamPassword)
            && normalizedText(left.m3UUrl) === normalizedText(right.m3UUrl)
            && normalizedText(left.m3UFilePath) === normalizedText(right.m3UFilePath)
            && normalizedText(left.xmltvUrl) === normalizedText(right.xmltvUrl)
            && normalizedAutoRefreshIntervalHours(left.autoRefreshIntervalHours)
                === normalizedAutoRefreshIntervalHours(right.autoRefreshIntervalHours)
    }

    function sourceIdentityOrConnectionChanged(draft, persistedDraft) {
        return normalizedText(draft.name) !== normalizedText(persistedDraft.name)
            || normalizedText(draft.xtreamBaseUrl) !== normalizedText(persistedDraft.xtreamBaseUrl)
            || normalizedText(draft.xtreamUsername) !== normalizedText(persistedDraft.xtreamUsername)
            || normalizedText(draft.xtreamPassword) !== normalizedText(persistedDraft.xtreamPassword)
            || normalizedText(draft.m3UUrl) !== normalizedText(persistedDraft.m3UUrl)
            || normalizedText(draft.m3UFilePath) !== normalizedText(persistedDraft.m3UFilePath)
    }

    function createDraftHasMeaningfulValues(draft) {
        if (!draft) {
            return false
        }

        const normalizedType = normalizedDraftType(draft.type)
        const hasCommonText = normalizedText(draft.name).length > 0
            || normalizedText(draft.xmltvUrl).length > 0
        if (normalizedType === 0) {
            return hasCommonText
                || normalizedText(draft.xtreamBaseUrl).length > 0
                || normalizedText(draft.xtreamUsername).length > 0
                || normalizedText(draft.xtreamPassword).length > 0
                || normalizedAutoRefreshIntervalHours(draft.autoRefreshIntervalHours) !== 24
        }
        if (normalizedType === 1) {
            return hasCommonText
                || normalizedText(draft.m3UUrl).length > 0
                || normalizedAutoRefreshIntervalHours(draft.autoRefreshIntervalHours) !== 24
        }
        return hasCommonText || normalizedText(draft.m3UFilePath).length > 0
    }

    function createDraftMapHasMeaningfulValues(draftsByType) {
        const keys = Object.keys(draftsByType || {})
        for (let index = 0; index < keys.length; ++index) {
            if (createDraftHasMeaningfulValues(draftsByType[keys[index]])) {
                return true
            }
        }
        return false
    }

    function profileById(profileId) {
        for (let row = 0; row < root.profiles.rowCount(); ++row) {
            const profile = root.profiles.get(row)
            if (profile.id === profileId) {
                return profile
            }
        }
        return null
    }

    function setPendingProfileDraft(profileId, draftOrNull) {
        const nextDrafts = Object.assign({}, root.pendingProfileDrafts)
        if (draftOrNull) {
            nextDrafts[profileId] = Object.assign({}, draftOrNull)
        } else {
            delete nextDrafts[profileId]
        }
        root.pendingProfileDrafts = nextDrafts
    }

    function setPendingCreateDraftForType(typeValue, draftOrNull) {
        const typeKey = String(normalizedDraftType(typeValue))
        const nextDrafts = Object.assign({}, root.pendingCreateDraftsByType)
        if (draftOrNull) {
            nextDrafts[typeKey] = Object.assign({}, draftOrNull)
        } else {
            delete nextDrafts[typeKey]
        }
        root.pendingCreateDraftsByType = nextDrafts
    }

    function applyDraftToForm(draft) {
        const normalizedType = normalizedDraftType(draft.type)
        root.draftType = normalizedType
        nameField.text = draft.name || ""
        xtreamUrlField.text = draft.xtreamBaseUrl || ""
        xtreamUserField.text = draft.xtreamUsername || ""
        xtreamPasswordField.text = draft.xtreamPassword || ""
        m3uUrlField.text = draft.m3UUrl || ""
        m3uFileField.text = draft.m3UFilePath || ""
        xmltvField.text = draft.xmltvUrl || ""
        autoRefreshIntervalHoursField.value = normalizedAutoRefreshIntervalHours(draft.autoRefreshIntervalHours)
    }

    function saveCurrentEditorDraft() {
        const currentDraft = buildDraftFromForm()
        if (root.createMode) {
            if (createDraftHasMeaningfulValues(currentDraft)) {
                setPendingCreateDraftForType(currentDraft.type, currentDraft)
            } else {
                setPendingCreateDraftForType(currentDraft.type, null)
            }
            return
        }

        if (root.selectedIndex < 0) {
            return
        }

        const profile = root.profiles.get(root.selectedIndex)
        if (!profile.id) {
            return
        }

        const persistedDraft = buildDraftFromProfile(profile)
        if (draftEquals(currentDraft, persistedDraft)) {
            setPendingProfileDraft(profile.id, null)
            return
        }

        setPendingProfileDraft(profile.id, currentDraft)
    }

    function currentEditorHasUnsavedChanges() {
        const currentDraft = buildDraftFromForm()
        if (root.createMode) {
            return createDraftHasMeaningfulValues(currentDraft)
        }

        if (root.selectedIndex < 0) {
            return false
        }

        const profile = root.profiles.get(root.selectedIndex)
        if (!profile.id) {
            return false
        }

        return !draftEquals(currentDraft, buildDraftFromProfile(profile))
    }

    function loadProfileIntoForm(row) {
        const profile = root.profiles.get(row)
        if (!profile.id) {
            return
        }

        const persistedDraft = buildDraftFromProfile(profile)
        const pendingDraft = root.pendingProfileDrafts[profile.id]
        const draft = pendingDraft ? pendingDraft : persistedDraft

        createMode = false
        selectedIndex = row
        applyDraftToForm(draft)
        root.groups.searchText = ""
        root.groups.profileId = profile.id
        markDirtyStateChanged()
    }

    function enterCreateMode(typeValue) {
        saveCurrentEditorDraft()
        createMode = true
        selectedIndex = -1
        const normalizedType = normalizedDraftType(typeValue)
        const typeKey = String(normalizedType)
        const pendingDraft = root.pendingCreateDraftsByType[typeKey]
        applyDraftToForm(pendingDraft ? pendingDraft : createDefaultDraftForType(normalizedType))
        root.groups.searchText = ""
        root.groups.profileId = ""
        markDirtyStateChanged()
    }

    function resetForNew(typeValue) {
        enterCreateMode(typeValue)
    }

    function groupMatchesSearch(groupName) {
        const query = root.groups.searchText.trim().toLowerCase()
        if (!query.length) {
            return true
        }
        return (groupName || "").toLowerCase().indexOf(query) >= 0
    }

    function groupVisibleInList(groupName, groupSelected) {
        if (!root.groupMatchesSearch(groupName)) {
            return false
        }
        if (!root.groups.hideUnchecked) {
            return true
        }
        return !!groupSelected
    }

    function addPendingNewSourceProfile(profileId) {
        if (!profileId || root.pendingNewSourceProfileIds.indexOf(profileId) >= 0) {
            return
        }
        root.pendingNewSourceProfileIds = root.pendingNewSourceProfileIds.concat([profileId])
    }

    function removePendingNewSourceProfile(profileId) {
        const index = root.pendingNewSourceProfileIds.indexOf(profileId)
        if (index < 0) {
            return
        }
        const updated = root.pendingNewSourceProfileIds.slice()
        updated.splice(index, 1)
        root.pendingNewSourceProfileIds = updated
    }

    function centerGroupsPanelInView() {
        if (!groupsPanel.visible || !sourceScroll.contentItem) {
            return
        }

        const viewportHeight = sourceScroll.availableHeight
        if (viewportHeight <= 0) {
            return
        }

        const panelCenterY = groupsPanel.y + groupsPanel.height * 0.5
        const desiredY = panelCenterY - viewportHeight * 0.5
        const maxY = Math.max(0, sourceFormColumn.implicitHeight - viewportHeight)
        sourceScroll.contentItem.contentY = Math.max(0, Math.min(desiredY, maxY))
    }

    function syncActiveProfileGroupState() {
        if (root.groups.profileId.length > 0
            && root.groups.profileId === root.app.activeProfileId) {
            root.channelList.refreshFilter()
        }
        if (root.liveGroups.profileId.length > 0) {
            root.liveGroups.reload()
        }
    }

    function selectProfileById(profileId) {
        for (let i = 0; i < root.profiles.rowCount(); ++i) {
            const profile = root.profiles.get(i)
            if (profile.id === profileId) {
                loadProfileIntoForm(i)
                return
            }
        }
    }

    function initialSelectionRow() {
        if (root.profiles.rowCount() <= 0) {
            return -1
        }

        const activeProfileId = root.profiles.activeProfileId
        if (activeProfileId && activeProfileId.length > 0) {
            for (let row = 0; row < root.profiles.rowCount(); ++row) {
                const profile = root.profiles.get(row)
                if (profile.id === activeProfileId) {
                    return row
                }
            }
        }

        return 0
    }

    function selectProfileByRow(row) {
        if (row < 0) {
            return
        }

        if (!root.createMode && root.selectedIndex === row) {
            return
        }

        saveCurrentEditorDraft()
        loadProfileIntoForm(row)
    }

    function enqueueRefreshProfile(profileId) {
        if (!profileId || root.refreshProfileQueue.indexOf(profileId) >= 0) {
            return
        }
        root.refreshProfileQueue = root.refreshProfileQueue.concat([profileId])
        root.startRefreshQueueIfIdle()
    }

    function startRefreshQueueIfIdle() {
        if (root.refreshQueueInFlight || root.refreshProfileQueue.length === 0) {
            return
        }
        if (root.app.isBusy) {
            return
        }

        root.refreshQueueInFlight = true
        const profileId = root.refreshProfileQueue[0]
        Qt.callLater(function() {
            root.app.loadProfile(profileId)
        })
    }

    function completeRefreshQueueItem(profileId) {
        if (!root.refreshQueueInFlight || root.refreshProfileQueue.length === 0) {
            return
        }
        if (root.refreshProfileQueue[0] !== profileId) {
            return
        }

        root.refreshProfileQueue = root.refreshProfileQueue.slice(1)
        root.refreshQueueInFlight = false
        if (root.refreshProfileQueue.length > 0) {
            Qt.callLater(function() {
                root.startRefreshQueueIfIdle()
            })
        }
    }

    function saveAllChanges() {
        saveCurrentEditorDraft()

        const pendingRefreshIds = []
        const handledProfileIds = []
        const handledCreateTypeKeys = []

        if (!root.createMode && root.selectedIndex >= 0) {
            const selectedProfile = root.profiles.get(root.selectedIndex)
            if (selectedProfile.id) {
                const currentDraft = buildDraftFromForm()
                const persistedDraft = buildDraftFromProfile(selectedProfile)
                if (!draftEquals(currentDraft, persistedDraft)) {
                    root.profiles.replaceProfile(selectedProfile.id, {
                        "name": currentDraft.name,
                        "xtreamBaseUrl": currentDraft.xtreamBaseUrl,
                        "xtreamUsername": currentDraft.xtreamUsername,
                        "xtreamPassword": currentDraft.xtreamPassword,
                        "m3UUrl": currentDraft.m3UUrl,
                        "m3UFilePath": currentDraft.m3UFilePath,
                        "xmltvUrl": currentDraft.xmltvUrl,
                        "autoRefreshIntervalHours": currentDraft.autoRefreshIntervalHours
                    })
                    if (sourceIdentityOrConnectionChanged(currentDraft, persistedDraft)) {
                        pendingRefreshIds.push(selectedProfile.id)
                    }
                }
                handledProfileIds.push(selectedProfile.id)
                setPendingProfileDraft(selectedProfile.id, null)
            }
        } else if (root.createMode) {
            const currentCreateDraft = buildDraftFromForm()
            const currentTypeKey = String(normalizedDraftType(currentCreateDraft.type))
            handledCreateTypeKeys.push(currentTypeKey)
            if (createDraftHasMeaningfulValues(currentCreateDraft)) {
                let createdProfileId = ""
                if (normalizedDraftType(currentCreateDraft.type) === 0) {
                    createdProfileId = root.profiles.addXtreamProfile(
                                currentCreateDraft.name,
                                currentCreateDraft.xtreamBaseUrl,
                                currentCreateDraft.xtreamUsername,
                                currentCreateDraft.xtreamPassword,
                                currentCreateDraft.xmltvUrl,
                                currentCreateDraft.autoRefreshIntervalHours)
                } else if (normalizedDraftType(currentCreateDraft.type) === 1) {
                    createdProfileId = root.profiles.addM3uUrlProfile(
                                currentCreateDraft.name,
                                currentCreateDraft.m3UUrl,
                                currentCreateDraft.xmltvUrl,
                                currentCreateDraft.autoRefreshIntervalHours)
                } else {
                    createdProfileId = root.profiles.addM3uFileProfile(
                                currentCreateDraft.name,
                                currentCreateDraft.m3UFilePath,
                                currentCreateDraft.xmltvUrl)
                }

                if (createdProfileId.length > 0) {
                    addPendingNewSourceProfile(createdProfileId)
                    pendingRefreshIds.push(createdProfileId)
                    selectProfileById(createdProfileId)
                }
            }
            setPendingCreateDraftForType(currentCreateDraft.type, null)
        }

        const profileDraftIds = Object.keys(root.pendingProfileDrafts)
        for (let index = 0; index < profileDraftIds.length; ++index) {
            const profileId = profileDraftIds[index]
            if (handledProfileIds.indexOf(profileId) >= 0) {
                continue
            }
            const profile = profileById(profileId)
            const draft = root.pendingProfileDrafts[profileId]
            if (!profile || !draft) {
                continue
            }

            const persistedDraft = buildDraftFromProfile(profile)
            root.profiles.replaceProfile(profileId, {
                "name": draft.name,
                "xtreamBaseUrl": draft.xtreamBaseUrl,
                "xtreamUsername": draft.xtreamUsername,
                "xtreamPassword": draft.xtreamPassword,
                "m3UUrl": draft.m3UUrl,
                "m3UFilePath": draft.m3UFilePath,
                "xmltvUrl": draft.xmltvUrl,
                "autoRefreshIntervalHours": draft.autoRefreshIntervalHours
            })

            if (sourceIdentityOrConnectionChanged(draft, persistedDraft)
                && pendingRefreshIds.indexOf(profileId) < 0) {
                pendingRefreshIds.push(profileId)
            }
        }
        root.pendingProfileDrafts = ({})

        const createDraftKeys = Object.keys(root.pendingCreateDraftsByType).sort()
        const createdProfileIds = []
        for (let index = 0; index < createDraftKeys.length; ++index) {
            if (handledCreateTypeKeys.indexOf(createDraftKeys[index]) >= 0) {
                continue
            }
            const draft = root.pendingCreateDraftsByType[createDraftKeys[index]]
            if (!createDraftHasMeaningfulValues(draft)) {
                continue
            }

            let createdProfileId = ""
            if (normalizedDraftType(draft.type) === 0) {
                createdProfileId = root.profiles.addXtreamProfile(
                            draft.name,
                            draft.xtreamBaseUrl,
                            draft.xtreamUsername,
                            draft.xtreamPassword,
                            draft.xmltvUrl,
                            draft.autoRefreshIntervalHours)
            } else if (normalizedDraftType(draft.type) === 1) {
                createdProfileId = root.profiles.addM3uUrlProfile(
                            draft.name,
                            draft.m3UUrl,
                            draft.xmltvUrl,
                            draft.autoRefreshIntervalHours)
            } else {
                createdProfileId = root.profiles.addM3uFileProfile(
                            draft.name,
                            draft.m3UFilePath,
                            draft.xmltvUrl)
            }

            if (createdProfileId.length > 0) {
                createdProfileIds.push(createdProfileId)
                addPendingNewSourceProfile(createdProfileId)
                if (pendingRefreshIds.indexOf(createdProfileId) < 0) {
                    pendingRefreshIds.push(createdProfileId)
                }
            }
        }
        root.pendingCreateDraftsByType = ({})

        if (createdProfileIds.length > 0) {
            selectProfileById(createdProfileIds[createdProfileIds.length - 1])
        } else if (root.createMode && root.profiles.rowCount() === 0) {
            applyDraftToForm(createDefaultDraftForType(root.draftType))
        }

        if (root.groups.dirty) {
            root.groups.saveDraftChanges()
            root.syncActiveProfileGroupState()
        }

        for (let index = 0; index < pendingRefreshIds.length; ++index) {
            enqueueRefreshProfile(pendingRefreshIds[index])
        }

        if (!root.createMode && root.selectedIndex >= 0) {
            const selectedProfile = root.profiles.get(root.selectedIndex)
            if (selectedProfile.id) {
                setPendingProfileDraft(selectedProfile.id, null)
                applyDraftToForm(buildDraftFromProfile(selectedProfile))
            }
        } else if (root.createMode) {
            const currentCreateDraft = buildDraftFromForm()
            if (!createDraftHasMeaningfulValues(currentCreateDraft)) {
                setPendingCreateDraftForType(currentCreateDraft.type, null)
            }
        }

        markDirtyStateChanged()
    }

    function saveCurrent() {
        saveAllChanges()
    }

    function discardDraftChanges() {
        root.pendingProfileDrafts = ({})
        root.pendingCreateDraftsByType = ({})
        root.pendingNewSourceProfileIds = []
        root.refreshProfileQueue = []
        root.refreshQueueInFlight = false
        root.groups.discardDraftChanges()

        if (root.selectedIndex >= 0 && !root.createMode) {
            loadProfileIntoForm(root.selectedIndex)
            return
        }

        if (root.profiles.rowCount() > 0) {
            loadProfileIntoForm(0)
            return
        }

        enterCreateMode(0)
        markDirtyStateChanged()
    }

    function activateCurrent() {
        if (selectedIndex < 0) {
            return
        }

        saveCurrentEditorDraft()
        const profile = root.profiles.get(selectedIndex)
        root.profiles.selectProfile(profile.id)
    }

    function refreshCurrent() {
        if (selectedIndex < 0) {
            return
        }

        saveCurrentEditorDraft()
        const profile = root.profiles.get(selectedIndex)
        root.app.loadProfile(profile.id)
    }

    function requestDeleteCurrent() {
        if (selectedIndex < 0) {
            return
        }

        saveCurrentEditorDraft()
        const profile = root.profiles.get(selectedIndex)
        pendingDeleteProfileId = profile.id
        pendingDeleteProfileName = profile.name || "this source"
        confirmDeleteVisible = true
    }

    function deleteCurrent() {
        if (!pendingDeleteProfileId.length) {
            return
        }

        setPendingProfileDraft(pendingDeleteProfileId, null)
        root.profiles.removeProfile(pendingDeleteProfileId)
        pendingDeleteProfileId = ""
        pendingDeleteProfileName = ""
        confirmDeleteVisible = false
        const nextIndex = root.profiles.rowCount() > 0 ? 0 : -1
        if (nextIndex >= 0) {
            loadProfileIntoForm(nextIndex)
        } else {
            enterCreateMode(0)
        }
    }

    Component.onCompleted: {
        root.groups.autoPersist = false
        const initialRow = initialSelectionRow()
        if (initialRow >= 0) {
            loadProfileIntoForm(initialRow)
        } else {
            resetForNew(0)
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: Theme.spacingL

        Rectangle {
            Layout.preferredWidth: root.sidebarWidth
            Layout.minimumWidth: root.sidebarWidth
            Layout.fillHeight: true
            color: root.sidebarSurface
            radius: Theme.radiusM
            border.width: 0

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spacingM
                spacing: Theme.spacingS

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Text {
                        text: "Sources"
                        color: Theme.textPrimary
                        font.pixelSize: 24
                        font.bold: true
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.profiles.rowCount() > 0
                            ? root.profiles.rowCount() + " configured profiles"
                            : "Add your first playlist or Xtream source"
                        color: Theme.textSecondary
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: root.refreshQueueInFlight
                        ? "Refreshing source data..."
                        : (root.profiles.rowCount() > 0
                            ? "Select a profile to edit credentials, refresh channels, and manage groups."
                            : "Create a source profile to start importing channels and groups.")
                    color: Theme.textSecondary
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 8

                    AppButton {
                        text: "Xtream"
                        compact: true
                        accent: root.createMode && root.draftType === 0
                        onClicked: root.resetForNew(0)
                    }

                    AppButton {
                        text: "M3U URL"
                        compact: true
                        accent: root.createMode && root.draftType === 1
                        onClicked: root.resetForNew(1)
                    }

                    AppButton {
                        text: "M3U File"
                        compact: true
                        accent: root.createMode && root.draftType === 2
                        onClicked: root.resetForNew(2)
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#32ffffff"
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 8
                    model: root.profiles

                    delegate: Rectangle {
                        id: profileRow
                        required property int index
                        required property var model
                        property string profileName: model.name
                        property string profileTypeLabel: model.typeLabel
                        property int profileGroupCount: model.groupCount || 0
                        property bool profileIsActive: model.isActive
                        property string profileLastRefreshed: model.lastRefreshed

                        width: ListView.view.width
                        height: 88
                        radius: Theme.radiusM
                        color: root.selectedIndex === index ? root.selectedRowSurface : "transparent"
                        border.width: 0

                        Column {
                            anchors.fill: parent
                            anchors.margins: 14
                            spacing: 5

                            RowLayout {
                                width: parent.width

                                Text {
                                    Layout.fillWidth: true
                                    text: profileRow.profileName
                                    color: Theme.textPrimary
                                    font.pixelSize: 15
                                    font.bold: true
                                    elide: Text.ElideRight
                                }

                                Rectangle {
                                    visible: profileRow.profileIsActive
                                    implicitWidth: 58
                                    implicitHeight: 22
                                    radius: 11
                                    color: root.rowSurface
                                    border.width: 0

                                    Text {
                                        anchors.centerIn: parent
                                        text: "ACTIVE"
                                        color: Theme.textPrimary
                                        font.pixelSize: 10
                                        font.bold: true
                                    }
                                }
                            }

                            Text {
                                text: profileRow.profileTypeLabel + " • " + profileRow.profileGroupCount + " groups"
                                color: Theme.textSecondary
                                font.pixelSize: 12
                            }

                            Text {
                                width: parent.width
                                text: profileRow.profileLastRefreshed
                                    ? "Last refresh " + profileRow.profileLastRefreshed.replace("T", " ").replace("Z", " UTC")
                                    : "Never refreshed"
                                color: Theme.textMuted
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.selectProfileByRow(profileRow.index)
                        }
                    }
                }
            }
        }

        ScrollView {
            id: sourceScroll

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            clip: true

            ColumnLayout {
                id: sourceFormColumn
                width: sourceScroll.availableWidth
                spacing: Theme.spacingM

                OverlaySectionPanel {
                    Layout.fillWidth: true
                    panelColor: root.sectionSurface

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Text {
                                text: root.createMode ? "Create Source" : "Edit Source"
                                color: Theme.textPrimary
                                font.pixelSize: 30
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: root.createMode
                                    ? "Add a provider, playlist URL, or local M3U file."
                                    : "Manage credentials, activate the profile, or refresh content."
                                color: Theme.textSecondary
                                font.pixelSize: 13
                                wrapMode: Text.Wrap
                            }
                        }

                        Flow {
                            spacing: 10

                            AppButton {
                                text: "Activate"
                                enabled: !root.createMode && root.selectedIndex >= 0
                                onClicked: root.activateCurrent()
                            }

                            AppButton {
                                text: "Refresh"
                                enabled: !root.createMode && root.selectedIndex >= 0
                                onClicked: root.refreshCurrent()
                            }

                            AppButton {
                                text: "Delete"
                                enabled: !root.createMode && root.selectedIndex >= 0
                                onClicked: root.requestDeleteCurrent()
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacingM

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 78
                            radius: Theme.radiusM
                            color: root.rowSurface
                            border.width: 0

                            Column {
                                anchors.fill: parent
                                anchors.margins: 14
                                spacing: 4

                                Text {
                                    text: "Mode"
                                    color: Theme.textMuted
                                    font.pixelSize: 11
                                }

                                Text {
                                    text: root.createMode ? "New source draft" : "Saved profile"
                                    color: Theme.textPrimary
                                    font.pixelSize: 18
                                    font.bold: true
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 78
                            radius: Theme.radiusM
                            color: root.rowSurface
                            border.width: 0

                            Column {
                                anchors.fill: parent
                                anchors.margins: 14
                                spacing: 4

                                Text {
                                    text: "Playback"
                                    color: Theme.textMuted
                                    font.pixelSize: 11
                                }

                                Text {
                                    text: root.profiles.activeProfileId ? "One source is active" : "No active source"
                                    color: Theme.textPrimary
                                    font.pixelSize: 18
                                    font.bold: true
                                }
                            }
                        }
                    }
                }

                OverlaySectionPanel {
                    Layout.fillWidth: true
                    panelColor: root.sectionSurface

                    Text {
                        text: "Source Identity"
                        color: Theme.textPrimary
                        font.pixelSize: 20
                        font.bold: true
                    }

                    FormTextField {
                        id: nameField
                        Layout.fillWidth: true
                        placeholderText: "Profile name"
                    }

                    FormComboBox {
                        Layout.fillWidth: true
                        model: ["Xtream", "M3U URL", "M3U File"]
                        currentIndex: root.draftType
                        enabled: root.createMode
                        onActivated: root.draftType = currentIndex
                    }

                    Text {
                        visible: !root.createMode
                        text: "Profile type is fixed after creation."
                        color: Theme.textMuted
                        font.pixelSize: 12
                    }
                }

                OverlaySectionPanel {
                    Layout.fillWidth: true
                    panelColor: root.sectionSurface

                    Text {
                        text: "Connection"
                        color: Theme.textPrimary
                        font.pixelSize: 20
                        font.bold: true
                    }

                    FormTextField {
                        id: xtreamUrlField
                        Layout.fillWidth: true
                        visible: root.draftType === 0
                        placeholderText: "Xtream base URL"
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: root.draftType === 0
                        spacing: Theme.spacingM

                        FormTextField {
                            id: xtreamUserField
                            Layout.fillWidth: true
                            placeholderText: "Xtream username"
                        }

                        FormTextField {
                            id: xtreamPasswordField
                            Layout.fillWidth: true
                            placeholderText: "Xtream password"
                            echoMode: TextInput.Password
                        }
                    }

                    FormTextField {
                        id: m3uUrlField
                        Layout.fillWidth: true
                        visible: root.draftType === 1
                        placeholderText: "M3U URL"
                    }

                    PathBrowseField {
                        id: m3uFileField
                        Layout.fillWidth: true
                        visible: root.draftType === 2
                        fieldPreferredWidth: root.shell.layoutBand === "compact" ? 280 : 360
                        fillFieldWidth: false
                        placeholderText: "M3U file path"
                        nameFilters: ["M3U Playlists (*.m3u *.m3u8)", "All files (*)"]
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: root.draftType !== 2
                        spacing: Theme.spacingM

                        Text {
                            Layout.fillWidth: true
                            text: "Auto refresh interval (hours, 0 to disable)"
                            color: Theme.textSecondary
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }

                        FormSpinBox {
                            id: autoRefreshIntervalHoursField
                            Layout.preferredWidth: root.shell.layoutBand === "compact" ? 174 : 192
                            from: 0
                            to: 720
                            stepSize: 1
                            value: 24
                        }
                    }
                }

                OverlaySectionPanel {
                    id: groupsPanel
                    Layout.fillWidth: true
                    panelColor: root.sectionSurface

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacingM

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Text {
                                text: "Groups"
                                color: Theme.textPrimary
                                font.pixelSize: 20
                                font.bold: true
                            }

                            Text {
                                Layout.fillWidth: true
                                text: root.groups.hasGroups
                                    ? root.groups.selectedCount + " selected out of "
                                        + root.groups.totalCount + " imported groups."
                                    : (root.createMode
                                        ? "Save this source to import groups automatically."
                                        : "Save this source to refresh and import the latest groups.")
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: root.groups.hasGroups && !root.groupReorderEnabled
                                text: "Clear the group search filter to reorder groups."
                                color: Theme.textMuted
                                font.pixelSize: 11
                                wrapMode: Text.Wrap
                            }
                        }

                        RowLayout {
                            spacing: Theme.spacingS
                            visible: root.groups.hasGroups

                            FormTextField {
                                id: groupsSearchField
                                Layout.preferredWidth: root.shell.layoutBand === "compact" ? 220 : 280
                                placeholderText: "Search groups"
                                text: root.groups.searchText
                                enabled: !root.groups.loading
                                onTextEdited: root.groups.searchText = text
                            }

                            AppButton {
                                text: "Select Filtered"
                                compact: true
                                Layout.preferredWidth: root.shell.layoutBand === "compact" ? 154 : 170
                                enabled: !root.groups.loading && root.groups.visibleGroupIds.length > 0
                                onClicked: {
                                    root.groups.setGroupsSelected(root.groups.visibleGroupIds, true)
                                }
                            }

                            AppButton {
                                text: "Deselect Filtered"
                                compact: true
                                Layout.preferredWidth: root.shell.layoutBand === "compact" ? 168 : 186
                                enabled: !root.groups.loading && root.groups.visibleGroupIds.length > 0
                                onClicked: {
                                    root.groups.setGroupsSelected(root.groups.visibleGroupIds, false)
                                }
                            }
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                        implicitHeight: groupsContent.implicitHeight

                        ColumnLayout {
                            id: groupsContent
                            anchors.fill: parent
                            spacing: Theme.spacingS

                            Item {
                                Layout.fillWidth: true
                                implicitHeight: 84
                                visible: root.groups.loading

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 14
                                    spacing: 6

                                    BusyIndicator {
                                        Layout.alignment: Qt.AlignLeft
                                        running: root.groups.loading
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: "Loading imported groups"
                                        color: Theme.textPrimary
                                        font.pixelSize: 16
                                        font.bold: true
                                        wrapMode: Text.Wrap
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: "The previous group list stays visible until the refreshed data is ready."
                                        color: Theme.textSecondary
                                        font.pixelSize: 12
                                        wrapMode: Text.Wrap
                                    }
                                }
                            }

                            Item {
                                Layout.fillWidth: true
                                implicitHeight: 92
                                visible: !root.groups.loading && !root.groups.hasGroups

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 14
                                    spacing: 6

                                    Text {
                                        Layout.fillWidth: true
                                        text: "No imported groups yet"
                                        color: Theme.textPrimary
                                        font.pixelSize: 16
                                        font.bold: true
                                        wrapMode: Text.Wrap
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: root.createMode
                                            ? "Create the source first. Save will import its groups automatically."
                                            : "Save changes to refresh this source and import the latest group list."
                                        color: Theme.textSecondary
                                        font.pixelSize: 12
                                        wrapMode: Text.Wrap
                                    }
                                }
                            }

                            Item {
                                Layout.fillWidth: true
                                implicitHeight: 72
                                visible: !root.groups.loading
                                    && root.groups.hasGroups
                                    && root.groups.visibleGroupIds.length === 0

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 14
                                    spacing: 6

                                    Text {
                                        Layout.fillWidth: true
                                        text: root.groups.searchText.trim().length > 0
                                            ? "No groups match this search."
                                            : "No checked groups to show."
                                        color: Theme.textPrimary
                                        font.pixelSize: 16
                                        font.bold: true
                                        wrapMode: Text.Wrap
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: root.groups.searchText.trim().length > 0
                                            ? "Try a different group name or clear the filter."
                                            : "Turn off Hide Unchecked to show all groups."
                                        color: Theme.textSecondary
                                        font.pixelSize: 12
                                        wrapMode: Text.Wrap
                                    }
                                }
                            }

                            ListView {
                                id: groupsList
                                Layout.fillWidth: true
                                Layout.preferredHeight: (root.groups.hasGroups
                                        && root.groups.visibleGroupIds.length > 0)
                                    ? Math.min(contentHeight, 340)
                                    : 0
                                clip: true
                                spacing: 6
                                enabled: !root.groups.loading
                                interactive: !root.groupDragActive && !root.groupReorderEnabled
                                model: root.groups.visibleGroups
                                visible: root.groups.hasGroups
                                    && root.groups.visibleGroupIds.length > 0
                                boundsBehavior: Flickable.StopAtBounds

                                WheelHandler {
                                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                                    blocking: true
                                    onWheel: function(event) {
                                        const maxContentY = Math.max(0, groupsList.contentHeight - groupsList.height)
                                        if (maxContentY <= 0) {
                                            event.accepted = false
                                            return
                                        }

                                        let delta = 0
                                        if (Math.abs(event.pixelDelta.y) > 0) {
                                            delta = event.pixelDelta.y
                                        } else if (Math.abs(event.angleDelta.y) > 0) {
                                            delta = event.angleDelta.y * 0.5
                                        }

                                        if (Math.abs(delta) <= 0.001) {
                                            event.accepted = false
                                            return
                                        }

                                        const previousY = groupsList.contentY
                                        const nextY = Math.max(0, Math.min(previousY - delta, maxContentY))
                                        const moved = Math.abs(nextY - previousY) > 0.01
                                        if (moved) {
                                            groupsList.contentY = nextY
                                        }
                                        event.accepted = moved
                                    }
                                }

                                delegate: Rectangle {
                                    id: groupRow
                                    required property int index
                                    required property var modelData
                                    property string groupId: modelData.id
                                    property int rowIndex: index
                                    property bool dragActive: dragHandleArea.pressed && root.groupReorderEnabled
                                    property int dragLastTargetIndex: index

                                    width: ListView.view.width
                                    height: 54
                                    visible: true
                                    radius: Theme.radiusM
                                    color: "#5a0c141b"
                                    border.width: dragActive ? 1 : 0
                                    border.color: "#7fa3b5"
                                    opacity: dragActive ? 0.8 : 1.0

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 14
                                        anchors.rightMargin: 14
                                        spacing: Theme.spacingM

                                        Rectangle {
                                            Layout.preferredWidth: 20
                                            Layout.preferredHeight: 20
                                            radius: 4
                                            color: groupRow.modelData.selected ? Theme.accent : "transparent"
                                            border.width: 1
                                            border.color: groupRow.modelData.selected ? Theme.accent : "#7fa3b5"

                                            Text {
                                                anchors.centerIn: parent
                                                text: groupRow.modelData.selected ? "\u2713" : ""
                                                color: Theme.textPrimary
                                                font.pixelSize: 11
                                                font.bold: true
                                            }
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 1

                                            Text {
                                                Layout.fillWidth: true
                                                text: groupRow.modelData.name
                                                color: Theme.textPrimary
                                                font.pixelSize: 14
                                                font.bold: true
                                                elide: Text.ElideRight
                                            }

                                            Text {
                                                Layout.fillWidth: true
                                                text: groupRow.modelData.count + " channels"
                                                color: Theme.textSecondary
                                                font.pixelSize: 11
                                                elide: Text.ElideRight
                                            }
                                        }

                                        Rectangle {
                                            Layout.preferredWidth: 22
                                            Layout.preferredHeight: 22
                                            radius: 5
                                            color: root.groupReorderEnabled ? "#2affffff" : "transparent"
                                            border.width: root.groupReorderEnabled ? 1 : 0
                                            border.color: "#6ca0b8"
                                            visible: root.groupReorderEnabled

                                            Text {
                                                anchors.centerIn: parent
                                                text: ":::"
                                                color: Theme.textSecondary
                                                font.pixelSize: 11
                                                font.bold: true
                                            }

                                            MouseArea {
                                                id: dragHandleArea
                                                anchors.fill: parent
                                                enabled: root.groupReorderEnabled
                                                acceptedButtons: Qt.LeftButton
                                                preventStealing: true
                                                cursorShape: Qt.OpenHandCursor
                                                onPressed: {
                                                    groupRow.dragLastTargetIndex = groupRow.rowIndex
                                                    root.groupDragActive = true
                                                }
                                                onReleased: {
                                                    groupRow.dragLastTargetIndex = -1
                                                    root.groupDragActive = false
                                                }
                                                onCanceled: {
                                                    groupRow.dragLastTargetIndex = -1
                                                    root.groupDragActive = false
                                                }
                                                onPositionChanged: {
                                                    if (!pressed || !root.groupReorderEnabled) {
                                                        return
                                                    }

                                                    const mapped = dragHandleArea.mapToItem(
                                                                groupsList.contentItem, mouseX, mouseY)
                                                    let targetIndex = groupsList.indexAt(mapped.x, mapped.y)
                                                    if (targetIndex < 0) {
                                                        if (mapped.y <= 0) {
                                                            targetIndex = 0
                                                        } else if (mapped.y >= groupsList.contentItem.height - 1) {
                                                            targetIndex = root.groups.visibleGroupIds.length - 1
                                                        } else {
                                                            return
                                                        }
                                                    }

                                                    if (targetIndex === groupRow.rowIndex
                                                        || targetIndex === groupRow.dragLastTargetIndex) {
                                                        return
                                                    }

                                                    groupRow.dragLastTargetIndex = targetIndex
                                                    const orderedVisibleIds = root.groups.visibleGroupIds.slice()
                                                    orderedVisibleIds.splice(groupRow.rowIndex, 1)
                                                    orderedVisibleIds.splice(targetIndex, 0, groupRow.groupId)
                                                    root.groups.reorderVisibleGroups(orderedVisibleIds)
                                                }
                                            }
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        anchors.rightMargin: root.groupReorderEnabled ? 34 : 0
                                        onClicked: {
                                            if (root.groupDragActive) {
                                                return
                                            }
                                            root.groups.setGroupSelected(groupRow.modelData.id, !groupRow.modelData.selected)
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                visible: root.groups.hasGroups
                                spacing: Theme.spacingS

                                Text {
                                    Layout.fillWidth: true
                                    text: "Hide Unchecked"
                                    color: Theme.textSecondary
                                    font.pixelSize: 12
                                }

                                FormSwitch {
                                    checked: root.groups.hideUnchecked
                                    onToggled: root.groups.hideUnchecked = checked
                                }
                            }
                        }
                    }
                }

                OverlaySectionPanel {
                    Layout.fillWidth: true
                    panelColor: root.sectionSurface

                    Text {
                        text: "EPG"
                        color: Theme.textPrimary
                        font.pixelSize: 20
                        font.bold: true
                    }

                    FormTextField {
                        id: xmltvField
                        Layout.fillWidth: true
                        placeholderText: "Optional XMLTV URL"
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "Use XMLTV when the provider does not supply usable EPG data directly."
                        color: Theme.textSecondary
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }

    Item {
        anchors.fill: parent
        visible: root.confirmDeleteVisible
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
            width: Math.min(500, parent.width - Theme.spacingXL * 2)
            anchors.centerIn: parent
            color: root.sectionSurface
            radius: Theme.radiusL
            border.width: 0
            implicitHeight: deleteDialogContent.implicitHeight + Theme.spacingL * 2

            ColumnLayout {
                id: deleteDialogContent

                x: Theme.spacingL
                y: Theme.spacingL
                width: parent.width - Theme.spacingL * 2
                spacing: Theme.spacingM

                Text {
                    Layout.fillWidth: true
                    text: "Delete source?"
                    color: Theme.textPrimary
                    font.pixelSize: 24
                    font.bold: true
                    wrapMode: Text.Wrap
                    renderType: Text.NativeRendering
                }

                Text {
                    Layout.fillWidth: true
                    text: "Remove \"" + root.pendingDeleteProfileName + "\" from Sources? This action removes the source profile immediately."
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
                        text: "Cancel"
                        onClicked: {
                            root.pendingDeleteProfileId = ""
                            root.pendingDeleteProfileName = ""
                            root.confirmDeleteVisible = false
                        }
                    }

                    AppButton {
                        text: "Delete"
                        danger: true
                        onClicked: root.deleteCurrent()
                    }
                }
            }
        }
    }

    Connections {
        target: root.app

        function onIsBusyChanged() {
            if (!root.app.isBusy) {
                root.startRefreshQueueIfIdle()
            }
        }

        function onProfileLoadFinished(profileId, ok) {
            root.completeRefreshQueueItem(profileId)

            if (profileId === root.groups.profileId && !root.groups.dirty) {
                root.groups.reload()
                root.syncActiveProfileGroupState()
            }

            if (root.pendingNewSourceProfileIds.indexOf(profileId) >= 0) {
                root.removePendingNewSourceProfile(profileId)
                if (ok) {
                    Qt.callLater(function() {
                        Qt.callLater(function() {
                            root.centerGroupsPanelInView()
                        })
                    })
                }
            }

            root.startRefreshQueueIfIdle()
        }
    }

    Connections {
        target: root.profiles

        function onDataChanged(topLeft, bottomRight, roles) {
            root.markDirtyStateChanged()
        }

        function onModelReset() {
            root.markDirtyStateChanged()
        }

        function onRowsInserted(parent, first, last) {
            root.markDirtyStateChanged()
        }

        function onRowsRemoved(parent, first, last) {
            root.markDirtyStateChanged()
        }

        function onRowsMoved(sourceParent, sourceStart, sourceEnd, destinationParent, destinationRow) {
            root.markDirtyStateChanged()
        }
    }
}
