import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: control

    required property var viewModel

    title: qsTr("Ссылки tinymesh://")
    modal: true
    anchors.centerIn: parent
    width: Math.min(520, parent.width - 60)
    standardButtons: Dialog.Close

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            Layout.fillWidth: true
            text: control.viewModel.appLinksRegistered ? qsTr("Ссылки зарегистрированы для текущего приложения.") : qsTr("Ссылки пока не зарегистрированы.")
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true

            Button {
                text: qsTr("Зарегистрировать")
                enabled: !control.viewModel.appLinksRegistered
                onClicked: control.viewModel.registerAppLinks()
            }

            Button {
                text: qsTr("Удалить регистрацию")
                enabled: control.viewModel.appLinksRegistered
                onClicked: control.viewModel.unregisterAppLinks()
            }

            Item {
                Layout.fillWidth: true
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Для portable-сборки регистрацию нужно обновить после перемещения executable.")
            color: control.palette.placeholderText
            wrapMode: Text.WordWrap
        }
    }
}
