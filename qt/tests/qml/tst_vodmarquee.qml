import QtQuick
import QtTest
import "../../qml/components"
import "../../qml/theme/Theme.js" as Theme

TestCase {
    id: testCase
    name: "VodMarquee"
    width: 400; height: 200
    visible: true
    when: windowShown
    VodMarqueeTitle {
        id: title
        width: 120
        text: "A long title that overflows its caption"
        font.pixelSize: 16
    }
    SignalSpy { id: starts; target: title; signalName: "scrollingChanged" }
    function init() {
        title.indicated = false; title.visible = true; title.enabled = true
        title.width = 120; title.text = "A long title that overflows its caption"
        title.contentIdentity = "one"; title.font.pixelSize = 16
        starts.clear()
    }
    function test_shortTitleDoesNotScroll() {
        title.text = "Short"; title.indicated = true
        wait(1150)
        verify(!title.overflowing); verify(!title.scrolling); compare(title.offset, 0)
    }
    function test_fullSecondAndEarlyCancellation() {
        title.indicated = true
        wait(650); verify(!title.scrolling); compare(title.offset, 0)
        title.indicated = false
        wait(450); verify(!title.scrolling)
        title.indicated = true
        wait(650); verify(!title.scrolling)
        tryCompare(title, "scrolling", true, 650)
        const offset = title.offset
        wait(500)
        fuzzyCompare(title.offset - offset, 17.5, 5)
        title.indicated = false
        verify(!title.scrolling); compare(title.offset, 0)
    }
    function test_resets_data() {
        return [{tag: "width"}, {tag: "text"}, {tag: "identity"}, {tag: "font"}, {tag: "hidden"}, {tag: "disabled"}]
    }
    function test_resets(data) {
        title.indicated = true
        tryCompare(title, "scrolling", true, 1500)
        wait(100); verify(title.offset > 0)
        if (data.tag === "width") title.width = 100
        else if (data.tag === "text") title.text += " changed"
        else if (data.tag === "identity") title.contentIdentity = "two"
        else if (data.tag === "font") title.font.pixelSize = 18
        else if (data.tag === "hidden") title.visible = false
        else title.enabled = false
        verify(!title.scrolling); compare(title.offset, 0)
        wait(650); verify(!title.scrolling)
        if (title.visible && title.enabled) tryCompare(title, "scrolling", true, 650)
    }
    function test_continuousLoop() {
        title.text = "WWWW"; title.width = 20; title.indicated = true
        tryCompare(title, "scrolling", true, 1500)
        compare(starts.count, 1)
        const duration = title.cycleDistance / Theme.vodMarqueePixelsPerSecond * 1000
        wait(duration + 200)
        verify(title.scrolling); compare(starts.count, 1)
        const afterWrap = title.offset
        wait(200)
        verify(title.offset > afterWrap)
        verify(title.offset < title.cycleDistance)
    }
}
