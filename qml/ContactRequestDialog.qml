import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog

    required property var viewModel

    title: qsTr("Запрос подключения")
    modal: false
    closePolicy: Popup.NoAutoClose
    width: 430

    contentItem: ColumnLayout {
        spacing: 16

        Label {
            Layout.fillWidth: true
            text: qsTr("%1 хочет подключиться").arg(dialog.viewModel.incomingContactName)
            wrapMode: Text.WordWrap
            font.pixelSize: 17
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight

            Button {
                text: qsTr("Отклонить")
                onClicked: dialog.viewModel.declineIncomingContact()
            }

            Button {
                text: qsTr("Принять")
                highlighted: true
                onClicked: dialog.viewModel.acceptIncomingContact()
            }
        }
    }
}
