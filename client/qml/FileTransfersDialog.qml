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
            delegate: Pane {
                required property var modelData
                width: ListView.view.width
                padding: 12
                background: Rectangle { color: "#edf1f3"; radius: 8 }
                contentItem: ColumnLayout {
                    Label {
                        Layout.fillWidth: true
                        text: modelData.name
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        text: (modelData.outgoing ? qsTr("Кому: %1") : qsTr("От: %1")).arg(modelData.peerName)
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                    }
                    ProgressBar { Layout.fillWidth: true; value: modelData.progress }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("%1 · %2 / %3 МБ").arg(modelData.status)
                            .arg((modelData.transferred / 1048576).toFixed(1))
                            .arg((modelData.size / 1048576).toFixed(1))
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        Button {
                            visible: modelData.canAccept
                            text: qsTr("Сохранить…")
                            onClicked: {
                                control.acceptingId = modelData.id;
                                destinationDialog.open();
                            }
                        }
                        Button {
                            visible: !modelData.finished
                            text: modelData.canAccept ? qsTr("Отклонить") : qsTr("Отменить")
                            onClicked: control.viewModel.cancelFile(modelData.id)
                        }
                        Button {
                            visible: modelData.finished
                            text: qsTr("Убрать из списка")
                            onClicked: control.viewModel.dismissFile(modelData.id)
                        }
                    }
                }
            }
        }
    }
}
