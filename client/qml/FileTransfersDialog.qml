import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Dialog {
    id: control
    required property var viewModel
    title: qsTr("Передача файлов")
    anchors.centerIn: parent
    width: Math.min(640, parent.width - 40)
    height: Math.min(540, parent.height - 40)
    modal: false
    standardButtons: Dialog.Close
    property string acceptingId: ""
    function saveTransfer(id) {
        const suggestion = viewModel.fileSaveSuggestion(id, destinationDialog.currentFolder);
        if (!suggestion.url) return;
        acceptingId = id;
        destinationDialog.selectedFile = suggestion.url;
        destinationDialog.defaultSuffix = suggestion.suffix;
        destinationDialog.open();
    }

    FileDialog {
        id: destinationDialog
        title: qsTr("Сохранить полученный файл")
        fileMode: FileDialog.SaveFile
        onAccepted: control.viewModel.acceptFile(control.acceptingId, selectedFile)
    }

    ColumnLayout {
        anchors.fill: parent
        Label {
            Layout.fillWidth: true
            visible: control.viewModel.fileTransfers.length === 0
            text: qsTr("Здесь появятся отправленные и полученные файлы.")
            wrapMode: Text.WordWrap
        }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 12
            model: control.viewModel.fileTransfers
            ScrollBar.vertical: ScrollBar {}
            delegate: TransferCard {
                required property var modelData
                width: ListView.view.width
                transfer: modelData
                viewModel: control.viewModel
                onSaveRequested: function(id) { control.saveTransfer(id); }
            }
        }
    }
}
