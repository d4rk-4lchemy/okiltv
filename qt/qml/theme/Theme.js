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
// Shared neutral palette for video overlays. Keep color in small semantic accents.
var overlayBackground = "#f5101010"
var overlaySidebar = "#800d0d0d"
var overlaySurface = "#1a1a1a"
var overlaySurfaceRaised = "#242424"
var overlaySurfaceMuted = "#2b2b2b"
var overlaySurfaceInteractive = "#363636"
var overlayBorder = "#474747"
var overlayTextPrimary = "#f4f4f4"
var overlayTextSecondary = "#bdbdbd"
var overlayTextMuted = "#909090"
var vodLibraryBackground = overlayBackground
var vodLibrarySidebar = overlaySidebar
// Shared VOD transport and poster progress colors.
var vodTimelineTrack = "#36454f"
var vodTimelineFill = "#a9d8ff"
var vodScrollBarThumb = "#929292"
var vodScrollBarThumbHover = "#a0a0a0"
var vodScrollBarThumbPressed = "#ababab"
function vodScrollBarColor(pressed, hovered) {
    return pressed ? vodScrollBarThumbPressed : hovered ? vodScrollBarThumbHover : vodScrollBarThumb
}
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
var vodSidebarMinimumWidth = 208
var vodSidebarDefaultWidth = 268
var vodSidebarResizeHandleWidth = 8
var vodScrollBarGap = 12
var vodLibraryScrollBarGap = 24
var vodMarqueeDelayMs = 1000
var vodMarqueePixelsPerSecond = 35
var vodMarqueeGap = 32
var railWidth = 104
var transitionMs = 180
var titleBarHeight = 33
var titleBarFontSize = 13
var titleBarIconSize = 18
var titleBarButtonSize = 30
var mediaModeChromeHeight = 50
var mediaModeWidth = 300
var mediaModeMinimumWidth = 216
var mediaModeHeight = 34
var mediaModeInset = 3
var mediaModeFontSize = 13
// Live media navigation shares the rail palette; VOD uses overlay tokens.
var playbackSearchFontSize = 14
var playbackSearchBackground = "#960d1822"
var liveRailBackground = "#82070d12"
var liveRailSelection = "#96182431"
var liveRailHover = "#6d111a24"
var liveRailPressed = "#ad1f2d3a"
var mediaModeBackground = liveRailBackground
var mediaModeSelection = liveRailSelection

// Opaque, compact labels remain legible on bright movie artwork.
var vodResolutionText = "#ffffff"
function vodResolutionColor(label) {
    switch (label) {
    case "480p": return "#475569"
    case "720p": return "#1d4ed8"
    case "1080p": return "#15803d"
    case "1440p": return "#7e22ce"
    case "4K": return "#a16207"
    default: return "#475569"
    }
}
