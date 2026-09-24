import QtQuick
import QtQuick.Controls

Dialog {
    id: control

    required property var viewModel

    title: qsTr("Диагностика сети")
    modal: true
    anchors.centerIn: parent
    width: Math.min(680, parent.width - 60)
    height: Math.min(500, parent.height - 60)
    standardButtons: Dialog.Close

    ScrollView {
        anchors.fill: parent

        TextArea {
            text: control.visible ? control.viewModel.diagnostics() : ""
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
        }
    }
}
