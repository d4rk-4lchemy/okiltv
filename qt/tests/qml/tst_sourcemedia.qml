import QtQuick
import QtTest
import "../../qml/components"

TestCase {
    id: testCase
    name: "SourceMediaSettings"
    width: 1100
    height: 900
    visible: true
    when: windowShown

    component Groups: QtObject {
        property string profileId: ""
        property bool hasGroups: true
        property int selectedCount: 2
        property int totalCount: 2
        property bool loading: false
        property bool dirty: false
        property bool autoPersist: false
        property bool hideUnchecked: false
        property string searchText: ""
        property string errorText: ""
        property bool hasEmptyDraftSelection: dirty && selectedCount === 0
        property var visibleGroups: [{id: "1", name: "One", count: 0, selected: true}, {id: "2", name: "Two", count: 0, selected: true}]
        property var visibleGroupIds: ["1", "2"]
        property int saves: 0
        property int discards: 0
        property bool fail: false
        function saveDraftChanges() { if (fail) { errorText = "Local write failed"; return false } dirty = false; ++saves; return true }
        function discardDraftChanges() { dirty = false; hideUnchecked = false; selectedCount = 2; ++discards }
        function reload() {}
        function setGroupsSelected(ids, selected) { dirty = true; selectedCount = selected ? 2 : 0 }
        function reorderVisibleGroups(ids) { dirty = true }
        function setGroupSelected(id, selected) { dirty = true }
    }
    Groups { id: live }
    Groups { id: movies }
    Groups { id: series }
    ListModel {
        id: profiles
        property string activeProfileId: "one"
        property bool failCreate: false
        property bool failFileCreate: false
        property int created: 0
        signal profileMutationFailed(string message)
        function rowCount() { return count }
        function replaceProfile(id, changes) {
            for (let row = 0; row < count; ++row) if (get(row).id === id) {
                for (const key of Object.keys(changes)) setProperty(row, key, changes[key])
                return true
            }
            return false
        }
        function addM3uUrlProfile(name, url, xmltv, interval, margin) {
            if (failCreate) { profileMutationFailed("Local create failed"); return "" }
            ++created
            return "created-" + created
        }
        function addM3uFileProfile(name, file, xmltv, margin) {
            if (failFileCreate) { profileMutationFailed("Local create failed"); return "" }
            return addM3uUrlProfile(name, file, xmltv, 0, margin)
        }
    }
    QtObject {
        id: app
        property bool isBusy: false
        property string activeProfileId: "one"
        property var groupAutoEnableNoticeProfileIds: []
        signal statusTextChanged()
        signal profileLoadFinished(string profileId, bool ok)
        function loadProfile(id) {}
    }
    SourceManagerPane {
        id: pane
        anchors.fill: parent
        profiles: profiles
        settingsGroups: live
        movieGroups: movies
        seriesGroups: series
        liveGroups: live
        app: app
        shell: ({layoutBand: testCase.width < 1000 ? "compact" : "standard"})
        dateTime: ({dateTimePattern: "yyyy-MM-dd"})
        channelList: ({refreshFilter: function() {}})
    }
    function init() {
        width = 1100
        pane.selectedIndex = -1
        profiles.clear()
        profiles.failCreate = false; profiles.failFileCreate = false; profiles.created = 0
        pane.refreshProfileQueue = []; pane.refreshQueueInFlight = false
        for (const id of ["one", "two", "m3u"]) profiles.append({id: id, type: id === "m3u" ? 1 : 0,
            name: id, typeLabel: id === "m3u" ? "M3U" : "Xtream", isActive: id === "one", lastRefreshed: "", groupCount: 2,
            xtreamBaseUrl: "http://localhost", xtreamUsername: "synthetic", xtreamPassword: "synthetic",
            m3UUrl: "http://localhost/local.m3u", m3UFilePath: "", xmltvUrl: "", catchupSafetyMinutes: 3,
            autoRefreshIntervalHours: 24, vodEnabled: true})
        for (const model of [live, movies, series]) {
            model.dirty = false; model.fail = false; model.errorText = ""; model.selectedCount = 2
            model.searchText = ""; model.hideUnchecked = false; model.saves = 0; model.discards = 0
        }
        pane.pendingProfileDrafts = ({})
        pane.pendingCreateDraftsByType = ({})
        pane.loadProfileIntoForm(0)
        wait(20)
    }
    function test_controls_and_media_state() {
        const toggle = findChild(pane, "ui.sources.enableVod")
        const margin = findChild(pane, "ui.sources.archiveMargin")
        verify(toggle.enabled); verify(toggle.checked)
        verify(toggle.mapToItem(pane, 0, 0).y > margin.mapToItem(pane, 0, 0).y)
        compare(pane.groupMediaType, 0)
        pane.groupMediaType = 1; movies.searchText = "movie"; movies.hideUnchecked = true; movies.dirty = true
        pane.groupMediaType = 2; series.searchText = "series"; series.dirty = true
        pane.groupMediaType = 0; live.searchText = "live"
        pane.groupMediaType = 1
        compare(pane.groups.searchText, "movie"); verify(pane.groups.hideUnchecked)
        compare(series.searchText, "series"); compare(live.searchText, "live")
        pane.draftVodEnabled = false
        compare(pane.groupMediaType, 0); verify(!pane.mediaTypesVisible)
        pane.loadProfileIntoForm(2)
        verify(!toggle.enabled); verify(!pane.mediaTypesVisible)
    }
    function test_save_discard_and_failed_groups() {
        pane.draftVodEnabled = false; pane.saveCurrentEditorDraft(); pane.loadProfileIntoForm(1)
        pane.draftVodEnabled = false
        movies.dirty = true; series.dirty = true; series.fail = true
        pane.saveAllChanges()
        verify(!profiles.get(0).vodEnabled); verify(!profiles.get(1).vodEnabled)
        compare(movies.saves, 1); verify(series.dirty); compare(pane.saveError, "Local write failed")
        series.fail = false; pane.saveAllChanges(); verify(!series.dirty); compare(series.saves, 1)
        pane.draftVodEnabled = true; movies.dirty = true; series.dirty = true
        pane.discardDraftChanges()
        verify(!pane.draftVodEnabled); verify(!movies.dirty); verify(!series.dirty)
        compare(movies.discards, 1); compare(series.discards, 1)
    }
    function test_empty_warning_for_hidden_media_and_narrow_layout() {
        series.selectedCount = 0; series.dirty = true
        pane.groupMediaType = 0; verify(pane.emptyGroupSelection)
        width = 860; wait(30)
        for (let index = 0; index < 3; ++index) {
            const button = findChild(pane, "ui.sources.media." + index)
            verify(button.width > 0)
            verify(button.mapToItem(pane, button.width, 0).x <= pane.width)
        }
        const search = findChild(pane, "ui.sources.groupSearch")
        verify(search.width >= 80)
        verify(search.mapToItem(pane, search.width, 0).x <= pane.width)
    }
    function test_partial_save_does_not_repeat_created_source() {
        const url = pane.createDefaultDraftForType(1); url.name = "URL"; url.m3UUrl = "http://localhost/fixture.m3u"
        const file = pane.createDefaultDraftForType(2); file.name = "File"; file.m3UFilePath = "/local/fixture.m3u"
        pane.pendingCreateDraftsByType = ({"1": url, "2": file})
        // Fail the second creation after the first source has committed.
        profiles.failFileCreate = true
        pane.saveAllChanges()
        compare(profiles.created, 1)
        verify(pane.pendingCreateDraftsByType["1"] === undefined)
        verify(pane.pendingCreateDraftsByType["2"] !== undefined)
        compare(pane.saveError, "Local create failed")
        profiles.failFileCreate = false
        pane.saveAllChanges()
        compare(profiles.created, 2)
        compare(Object.keys(pane.pendingCreateDraftsByType).length, 0)
    }
}
