pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Dialogs
import QtCore
import QtQml

RowLayout {
    id: control

    property alias text: pathField.text
    property alias placeholderText: pathField.placeholderText
    readonly property bool fieldActiveFocus: pathField.activeFocus
    property bool folderMode: false
    property var nameFilters: []
    property int fieldPreferredWidth: 320
    property bool fillFieldWidth: false
    property string browseText: "Browse"
    property FileDialog fileDialogInstance: null
    property FolderDialog folderDialogInstance: null

    signal textEdited(string text)
    signal editingFinished()

    function normalizePath(pathValue) {
        return (pathValue || "").toString().trim()
    }

    function urlToLocalPath(urlValue) {
        const asString = (urlValue || "").toString()
        if (!asString.startsWith("file:")) {
            return asString
        }

        let local = decodeURIComponent(asString.replace("file://", ""))
        if (Qt.platform.os === "windows" && local.startsWith("/")) {
            local = local.substring(1)
        }
        return local
    }

    function pathToFileUrl(pathValue) {
        const normalized = normalizePath(pathValue).replace(/\\/g, "/")
        if (!normalized.length) {
            return pathToFileUrl(StandardPaths.writableLocation(StandardPaths.HomeLocation))
        }
        if (normalized.startsWith("file:")) {
            return normalized
        }
        if (Qt.platform.os === "windows") {
            return "file:///" + encodeURI(normalized)
        }
        if (normalized.startsWith("/")) {
            return "file://" + encodeURI(normalized)
        }
        return "file:///" + encodeURI(normalized)
    }

    function parentDirectoryPath(pathValue, forFolder) {
        const normalized = normalizePath(pathValue).replace(/\\/g, "/")
        if (!normalized.length) {
            return StandardPaths.writableLocation(StandardPaths.HomeLocation)
        }

        if (forFolder) {
            return normalized
        }

        const separator = normalized.lastIndexOf("/")
        if (separator < 0) {
            return StandardPaths.writableLocation(StandardPaths.HomeLocation)
        }

        let parent = normalized.substring(0, separator)
        if (/^[A-Za-z]:$/.test(parent)) {
            parent += "/"
        }
        return parent.length > 0 ? parent : "/"
    }

    function ensureFileDialog() {
        if (control.fileDialogInstance === null) {
            control.fileDialogInstance = fileDialogComponent.createObject(control)
        }
        return control.fileDialogInstance
    }

    function ensureFolderDialog() {
        if (control.folderDialogInstance === null) {
            control.folderDialogInstance = folderDialogComponent.createObject(control)
        }
        return control.folderDialogInstance
    }

    function bindDialogToWindow(dialog) {
        if (dialog === null || control.Window.window === null) {
            return
        }
        if (dialog.parentWindow !== control.Window.window) {
            dialog.parentWindow = control.Window.window
        }
    }

    FormTextField {
        id: pathField

        Layout.preferredWidth: control.fieldPreferredWidth
        Layout.fillWidth: control.fillFieldWidth
        onTextEdited: control.textEdited(text)
        onEditingFinished: control.editingFinished()
    }

    AppButton {
        text: control.browseText
        compact: true
        onClicked: {
            const folder = control.parentDirectoryPath(pathField.text, control.folderMode)
            if (control.folderMode) {
                const dialog = control.ensureFolderDialog()
                if (dialog === null) {
                    return
                }
                control.bindDialogToWindow(dialog)
                dialog.currentFolder = control.pathToFileUrl(folder)
                dialog.open()
            } else {
                const dialog = control.ensureFileDialog()
                if (dialog === null) {
                    return
                }
                control.bindDialogToWindow(dialog)
                dialog.currentFolder = control.pathToFileUrl(folder)
                dialog.open()
            }
        }
    }

    Component {
        id: fileDialogComponent

        FileDialog {
            fileMode: FileDialog.OpenFile
            nameFilters: control.nameFilters
            onAccepted: {
                pathField.text = control.urlToLocalPath(selectedFile)
                control.textEdited(pathField.text)
                control.editingFinished()
            }
        }
    }

    Component {
        id: folderDialogComponent

        FolderDialog {
            onAccepted: {
                pathField.text = control.urlToLocalPath(selectedFolder)
                control.textEdited(pathField.text)
                control.editingFinished()
            }
        }
    }
}
