import QtQuick
import QtQuick.Controls
import "../theme/Theme.js" as Theme

Button {
    id: control

    property string iconName: ""
    property url iconSource: ""
    property string glyph: ""
    property string caption: ""
    property bool accent: false
    property bool compact: false
    property bool borderless: false
    property bool barMode: false
    property bool glassMode: false
    property real iconInset: control.compact ? 10 : 9
    property color iconColor: Theme.textPrimary

    implicitWidth: compact ? 42 : 46
    implicitHeight: compact ? 42 : 46
    hoverEnabled: true
    scale: control.enabled && control.hovered ? 1.08 : 1.0
    transformOrigin: Item.Center

    Behavior on scale {
        NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
    }

    contentItem: Item {
        Image {
            id: iconImage

            anchors.fill: parent
            anchors.margins: Math.max(0, control.iconInset)
            visible: control.iconSource.toString().length > 0
            source: control.iconSource
            fillMode: Image.PreserveAspectFit
            smooth: true
            mipmap: true
            opacity: control.enabled ? 1.0 : 0.36
        }

        Canvas {
            id: iconCanvas

            anchors.fill: parent
            anchors.margins: Math.max(0, control.iconInset + 1)
            visible: !iconImage.visible && control.iconName.length > 0
            antialiasing: true

            function strokePath(callback) {
                const ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                ctx.lineWidth = 2
                ctx.lineCap = "round"
                ctx.lineJoin = "round"
                ctx.strokeStyle = control.iconColor
                ctx.fillStyle = control.iconColor
                callback(ctx)
            }

            onPaint: {
                strokePath(function(ctx) {
                    const w = width
                    const h = height

                    if (control.iconName === "play") {
                        ctx.beginPath()
                        ctx.moveTo(w * 0.3, h * 0.2)
                        ctx.lineTo(w * 0.76, h * 0.5)
                        ctx.lineTo(w * 0.3, h * 0.8)
                        ctx.closePath()
                        ctx.fill()
                    } else if (control.iconName === "pause") {
                        ctx.fillRect(w * 0.28, h * 0.2, w * 0.14, h * 0.6)
                        ctx.fillRect(w * 0.58, h * 0.2, w * 0.14, h * 0.6)
                    } else if (control.iconName === "stop") {
                        ctx.fillRect(w * 0.26, h * 0.26, w * 0.48, h * 0.48)
                    } else if (control.iconName === "previous") {
                        ctx.fillRect(w * 0.2, h * 0.22, w * 0.09, h * 0.56)
                        ctx.beginPath()
                        ctx.moveTo(w * 0.72, h * 0.2)
                        ctx.lineTo(w * 0.34, h * 0.5)
                        ctx.lineTo(w * 0.72, h * 0.8)
                        ctx.closePath()
                        ctx.fill()
                    } else if (control.iconName === "next") {
                        ctx.fillRect(w * 0.71, h * 0.22, w * 0.09, h * 0.56)
                        ctx.beginPath()
                        ctx.moveTo(w * 0.28, h * 0.2)
                        ctx.lineTo(w * 0.66, h * 0.5)
                        ctx.lineTo(w * 0.28, h * 0.8)
                        ctx.closePath()
                        ctx.fill()
                    } else if (control.iconName === "guide") {
                        const cell = w * 0.22
                        const gap = w * 0.08
                        const startX = w * 0.18
                        const startY = h * 0.18
                        for (let row = 0; row < 2; ++row) {
                            for (let column = 0; column < 2; ++column) {
                                ctx.strokeRect(startX + column * (cell + gap), startY + row * (cell + gap), cell, cell)
                            }
                        }
                    } else if (control.iconName === "search") {
                        ctx.beginPath()
                        ctx.arc(w * 0.45, h * 0.43, w * 0.21, 0, Math.PI * 2)
                        ctx.stroke()
                        ctx.beginPath()
                        ctx.moveTo(w * 0.59, h * 0.58)
                        ctx.lineTo(w * 0.78, h * 0.77)
                        ctx.stroke()
                    } else if (control.iconName === "fullscreen") {
                        ctx.beginPath()
                        ctx.moveTo(w * 0.36, h * 0.16)
                        ctx.lineTo(w * 0.18, h * 0.16)
                        ctx.lineTo(w * 0.18, h * 0.34)
                        ctx.moveTo(w * 0.64, h * 0.16)
                        ctx.lineTo(w * 0.82, h * 0.16)
                        ctx.lineTo(w * 0.82, h * 0.34)
                        ctx.moveTo(w * 0.18, h * 0.66)
                        ctx.lineTo(w * 0.18, h * 0.84)
                        ctx.lineTo(w * 0.36, h * 0.84)
                        ctx.moveTo(w * 0.82, h * 0.66)
                        ctx.lineTo(w * 0.82, h * 0.84)
                        ctx.lineTo(w * 0.64, h * 0.84)
                        ctx.stroke()
                    } else if (control.iconName === "fullscreenExit") {
                        ctx.beginPath()
                        ctx.moveTo(w * 0.26, h * 0.16)
                        ctx.lineTo(w * 0.26, h * 0.34)
                        ctx.lineTo(w * 0.44, h * 0.34)
                        ctx.moveTo(w * 0.74, h * 0.16)
                        ctx.lineTo(w * 0.74, h * 0.34)
                        ctx.lineTo(w * 0.56, h * 0.34)
                        ctx.moveTo(w * 0.26, h * 0.84)
                        ctx.lineTo(w * 0.26, h * 0.66)
                        ctx.lineTo(w * 0.44, h * 0.66)
                        ctx.moveTo(w * 0.74, h * 0.84)
                        ctx.lineTo(w * 0.74, h * 0.66)
                        ctx.lineTo(w * 0.56, h * 0.66)
                        ctx.stroke()
                    } else if (control.iconName === "windowMinimize") {
                        ctx.fillRect(w * 0.26, h * 0.64, w * 0.48, 2)
                    } else if (control.iconName === "windowMaximize") {
                        ctx.strokeRect(w * 0.28, h * 0.28, w * 0.44, h * 0.44)
                    } else if (control.iconName === "windowRestore") {
                        ctx.strokeRect(w * 0.34, h * 0.24, w * 0.38, h * 0.38)
                        ctx.strokeRect(w * 0.22, h * 0.36, w * 0.38, h * 0.38)
                    } else if (control.iconName === "windowClose") {
                        ctx.beginPath()
                        ctx.moveTo(w * 0.28, h * 0.28)
                        ctx.lineTo(w * 0.72, h * 0.72)
                        ctx.moveTo(w * 0.72, h * 0.28)
                        ctx.lineTo(w * 0.28, h * 0.72)
                        ctx.stroke()
                    } else if (control.iconName === "list") {
                        for (let line = 0; line < 3; ++line) {
                            const y = h * (0.28 + line * 0.22)
                            ctx.fillRect(w * 0.18, y - 2, w * 0.1, 4)
                            ctx.fillRect(w * 0.36, y - 2, w * 0.46, 4)
                        }
                    } else if (control.iconName === "back") {
                        ctx.beginPath()
                        ctx.moveTo(w * 0.68, h * 0.2)
                        ctx.lineTo(w * 0.34, h * 0.5)
                        ctx.lineTo(w * 0.68, h * 0.8)
                        ctx.stroke()
                    } else {
                        ctx.clearRect(0, 0, width, height)
                    }
                })
            }

            Connections {
                target: control

                function onIconNameChanged() { iconCanvas.requestPaint() }
                function onIconColorChanged() { iconCanvas.requestPaint() }
                function onEnabledChanged() { iconCanvas.requestPaint() }
            }
        }

        Text {
            anchors.fill: parent
            visible: !iconImage.visible && !iconCanvas.visible
            text: control.glyph
            color: control.iconColor
            font.pixelSize: 20
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            renderType: Text.NativeRendering
        }
    }

    background: Rectangle {
        radius: Theme.radiusS
        color: {
            if (!control.enabled)
                return control.barMode ? "transparent" : Theme.surface
            if (control.glassMode) {
                if (control.down)
                    return "#7a1a2d40"
                if (control.hovered)
                    return "#98162131"
                return "#8f0a131d"
            }
            if (control.barMode) {
                if (control.down)
                    return "#5e2a3e57"
                if (control.hovered)
                    return "#4b203246"
                return "transparent"
            }
            if (control.accent)
                return control.down ? "#4c93d4" : Theme.accent
            if (control.down)
                return Theme.accentMuted
            if (control.hovered)
                return Theme.surfaceInteractive
            return Theme.surfaceRaised
        }
        border.width: (control.borderless || control.barMode) ? 0 : 1
        border.color: control.visualFocus || control.accent ? Theme.borderStrong : Theme.border
    }

    ToolTip.visible: hovered && caption.length > 0
    ToolTip.text: caption
}
