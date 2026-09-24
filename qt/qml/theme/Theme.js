.pragma library

var window = "#08131d"
var surface = "#0d1a27"
var surfaceRaised = "#142334"
var surfaceMuted = "#1a2b3d"
var surfaceInteractive = "#20364d"
var border = "#294763"
var borderStrong = "#4e88b8"
var accent = "#59aaf7"
var accentMuted = "#2c5b87"
var textPrimary = "#f4f7fb"
var textSecondary = "#aac0d3"
var textMuted = "#8096ab"
var channelProgressTrack = "#424242"
var channelProgressFill = "#929292"
var success = "#5ac88f"
var warning = "#d4aa56"
var danger = "#e17474"
var shadow = "#66000000"
var glass = "#b3121d2a"
var glassStrong = "#d2192434"

// The percentage scales the original transparency; 100 preserves the design.
// Pass original colors here, exactly once at the rendered background.
function uiBackground(baseColor, transparency, baseOpacity) {
    var color = Qt.tint(baseColor, "transparent")
    var alpha = color.a * (baseOpacity === undefined ? 1 : baseOpacity)
    if (alpha === 0)
        return Qt.rgba(color.r, color.g, color.b, 0)
    var fraction = Math.max(0, Math.min(100, transparency)) / 100
    return Qt.rgba(color.r, color.g, color.b, 1 - (1 - alpha) * fraction)
}

var radiusS = 8
var radiusM = 10
var radiusL = 12
var spacingXs = 6
var spacingS = 10
var spacingM = 16
var spacingL = 24
var spacingXL = 36
var railWidth = 104
var transitionMs = 180
