import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../theme/Theme.js" as Theme

Control {
    id: control

    property int from: 0
    property int to: 99
    property int value: 0
    property int stepSize: 1
    readonly property bool editing: editor.activeFocus

    implicitWidth: 176
    implicitHeight: 44
    hoverEnabled: true
    padding: 0

    function lowerBound() {
        return Math.min(control.from, control.to)
    }

    function upperBound() {
        return Math.max(control.from, control.to)
    }

    function clampValue(candidate) {
        return Math.max(control.lowerBound(), Math.min(control.upperBound(), candidate))
    }

    function applyCandidate(candidate) {
        const parsed = Number(candidate)
        if (Number.isNaN(parsed)) {
            return
        }

        const nextValue = control.clampValue(Math.round(parsed))
        if (control.value !== nextValue) {
            control.value = nextValue
        }
    }

    function syncEditorText() {
        const displayValue = String(control.value)
        if (!editor.activeFocus && editor.text !== displayValue) {
            editor.text = displayValue
        }
    }

    onFromChanged: {
        const nextValue = control.clampValue(control.value)
        if (nextValue !== control.value) {
            control.value = nextValue
            return
        }
        syncEditorText()
    }

    onToChanged: {
        const nextValue = control.clampValue(control.value)
        if (nextValue !== control.value) {
            control.value = nextValue
            return
        }
        syncEditorText()
    }

    onValueChanged: {
        const nextValue = control.clampValue(control.value)
        if (nextValue !== control.value) {
            control.value = nextValue
            return
        }
        syncEditorText()
    }

    Component.onCompleted: syncEditorText()

    contentItem: Rectangle {
        radius: Theme.radiusM
        color: {
            if (!control.enabled)
                return "#53232c34"
            if (editor.activeFocus || control.visualFocus)
                return "#8a2b343d"
            if (control.hovered)
                return "#7d273039"
            return "#71242d35"
        }
        border.width: editor.activeFocus || control.visualFocus ? 1 : 0
        border.color: editor.activeFocus || control.visualFocus ? "#96acbc" : "transparent"

        RowLayout {
            anchors.fill: parent
            anchors.margins: 4
            spacing: 4

            Button {
                id: decrementButton

                Layout.preferredWidth: 34
                Layout.fillHeight: true
                enabled: control.enabled
                text: "-"
                autoRepeat: true
                onClicked: control.applyCandidate(control.value - control.stepSize)

                contentItem: Text {
                    text: decrementButton.text
                    color: decrementButton.enabled ? Theme.textPrimary : Theme.textMuted
                    font.pixelSize: 18
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    renderType: Text.NativeRendering
                }

                background: Rectangle {
                    radius: Theme.radiusS
                    color: decrementButton.down ? "#7c2f3943" : (decrementButton.hovered ? "#632c3640" : "#4d28323b")
                    border.width: 0
                }
            }

            TextField {
                id: editor

                Layout.fillWidth: true
                Layout.fillHeight: true
                text: String(control.value)
                color: Theme.textPrimary
                placeholderTextColor: Theme.textMuted
                selectionColor: Theme.accentMuted
                selectedTextColor: Theme.textPrimary
                font.pixelSize: 14
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                topPadding: 10
                bottomPadding: 10
                leftPadding: 10
                rightPadding: 10
                selectByMouse: true
                inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator {
                    bottom: control.lowerBound()
                    top: control.upperBound()
                }

                background: Item {}

                onTextEdited: {
                    if (text.length === 0) {
                        return
                    }
                    control.applyCandidate(text)
                }

                onEditingFinished: {
                    if (text.length === 0) {
                        text = String(control.value)
                        return
                    }
                    control.applyCandidate(text)
                    text = String(control.value)
                }
            }

            Button {
                id: incrementButton

                Layout.preferredWidth: 34
                Layout.fillHeight: true
                enabled: control.enabled
                text: "+"
                autoRepeat: true
                onClicked: control.applyCandidate(control.value + control.stepSize)

                contentItem: Text {
                    text: incrementButton.text
                    color: incrementButton.enabled ? Theme.textPrimary : Theme.textMuted
                    font.pixelSize: 18
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    renderType: Text.NativeRendering
                }

                background: Rectangle {
                    radius: Theme.radiusS
                    color: incrementButton.down ? "#7c2f3943" : (incrementButton.hovered ? "#632c3640" : "#4d28323b")
                    border.width: 0
                }
            }
        }
    }
}
