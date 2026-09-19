import QtQuick
import QtTest
import "../../qml/components"
import "../../qml/theme/Theme.js" as Theme

TestCase {
    id: testCase
    name: "UiTransparency"
    width: 400
    height: 300
    visible: true
    when: windowShown
    property int preview: 100

    GlassPanel {
        id: glass
        width: 100
        height: 100
        uiTransparency: testCase.preview
        fillColor: "#82070d12"
    }
    OverlaySectionPanel {
        id: section
        x: 110
        width: 100
        uiTransparency: testCase.preview
        panelColor: "#7a0b1117"
    }
    FormTextField {
        id: field
        y: 120
        uiTransparency: testCase.preview
        text: "Readable text"
    }

    function init() {
        preview = 100
        glass.fillOpacity = 1
        field.enabled = true
    }

    function test_alpha_data() {
        return [
            { tag: "live", base: "#82070d12", alpha: 130 / 255 },
            { tag: "glass", base: Theme.glass, alpha: 179 / 255 },
            { tag: "strong", base: Theme.glassStrong, alpha: 210 / 255 },
            { tag: "opaque", base: Theme.surface, alpha: 1 }
        ]
    }

    function test_alpha(data) {
        const original = Qt.tint(data.base, "transparent")
        for (const value of [100, 50, 0]) {
            const result = Theme.uiBackground(data.base, value)
            fuzzyCompare(result.a, 1 - (1 - data.alpha) * value / 100, 0.0001)
            fuzzyCompare(result.r, original.r, 0.0001)
            fuzzyCompare(result.g, original.g, 0.0001)
            fuzzyCompare(result.b, original.b, 0.0001)
        }
        compare(Theme.uiBackground(data.base, 100), original)
    }

    function test_existing_surfaces_follow_preview_and_revert() {
        const originalGlass = glass.color
        const originalSection = section.color
        const originalText = field.color
        preview = 50
        // Verify custom fills are transformed once, not once per wrapper.
        fuzzyCompare(glass.color.a, 1 - (1 - 130 / 255) * 0.5, 0.0001)
        fuzzyCompare(section.color.a, 1 - (1 - 122 / 255) * 0.5, 0.0001)
        preview = 0
        compare(glass.color.a, 1)
        compare(section.color.a, 1)
        compare(field.background.color.a, 1)
        compare(field.color, originalText)
        compare(glass.opacity, 1)
        field.enabled = false
        compare(field.background.color.a, 1)
        preview = 100
        compare(glass.color, originalGlass)
        compare(section.color, originalSection)
    }

    function test_background_opacity_is_included() {
        glass.fillOpacity = 0.5
        fuzzyCompare(glass.color.a, 130 / 255 * 0.5, 0.0001)
        preview = 0
        compare(glass.color.a, 1)
        glass.fillOpacity = 0
        compare(glass.color.a, 0)
        preview = 100
        compare(glass.color.a, 0)
    }

    function test_empty_areas_and_range() {
        compare(Theme.uiBackground("transparent", 0).a, 0)
        compare(Theme.uiBackground("transparent", 100).a, 0)
        compare(Theme.uiBackground(Theme.glass, -10).a, 1)
        compare(Theme.uiBackground(Theme.glass, 110), Qt.tint(Theme.glass, "transparent"))
    }
}
