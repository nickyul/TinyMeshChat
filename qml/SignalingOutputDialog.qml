import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: control

    required property var viewModel
    property string signalingText: ""
    property string signalingLink: ""

    signal saveRequested

    function showSignaling(kind, text, link) {
        signalingText = text;
        signalingLink = link;
        title = kind === "offer" ? qsTr("Приглашение готово") : qsTr("Ответ готов");
        open();
    }

    title: qsTr("Код подключения готов")
    modal: true
    anchors.centerIn: parent
    width: Math.min(760, parent.width - 60)
    height: Math.min(480, parent.height - 60)
    standardButtons: Dialog.Close

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            Layout.fillWidth: true
            text: qsTr("Код уже скопирован. Передайте его другому участнику:")
            wrapMode: Text.WordWrap
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            TextArea {
                text: control.signalingText
                readOnly: true
                wrapMode: TextEdit.WrapAnywhere
                selectByMouse: true
            }
        }

        RowLayout {
            Layout.fillWidth: true

            PrimaryButton {
                text: qsTr("Копировать код")
                onClicked: control.viewModel.copyText(control.signalingText)
            }

            Button {
                text: qsTr("Копировать ссылку")
                enabled: control.signalingLink.length > 0
                onClicked: control.viewModel.copyText(control.signalingLink)
            }

            Button {
                text: qsTr("Сохранить файл…")
                onClicked: control.saveRequested()
            }

            Item {
                Layout.fillWidth: true
            }
        }
    }
}
