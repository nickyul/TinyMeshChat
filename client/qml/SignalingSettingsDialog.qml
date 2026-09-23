import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: control
    required property var viewModel
    readonly property bool editable: !viewModel.meshVisible && !viewModel.connecting && !viewModel.serverBusy
    title: qsTr("Сервер сигналинга")
    modal: true
    anchors.centerIn: parent
    width: Math.min(620, parent.width - 48)
    onOpened: {
        serverEnabled.checked = viewModel.signalingServerUrl.length > 0;
        serverAddress.text = serverEnabled.checked ? viewModel.signalingServerUrl : "ws://127.0.0.1:8080";
    }

    ColumnLayout {
        width: parent.width
        spacing: 12
        Switch {
            id: serverEnabled
            text: qsTr("Подключаться к серверу при запуске")
            enabled: control.editable
        }
        TextField {
            id: serverAddress
            Layout.fillWidth: true
            enabled: serverEnabled.checked && control.editable
            placeholderText: "ws://127.0.0.1:8080"
            maximumLength: 2048
            selectByMouse: true
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Состояние: %1").arg(control.viewModel.signalingStatus)
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            text: control.editable
                ? qsTr("Сервер помогает установить соединение. Сообщения и голос передаются между участниками по WebRTC. Ручные приглашения доступны без сервера.")
                : qsTr("При обрыве подключение восстанавливается автоматически. Для изменения адреса выйдите из mesh. Ручные приглашения доступны и без сервера.")
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            Button {
                text: qsTr("Применить")
                enabled: control.editable && (!serverEnabled.checked || serverAddress.text.trim().length > 0)
                onClicked: {
                    if (control.viewModel.updateSignalingServer(serverEnabled.checked ? serverAddress.text.trim() : ""))
                        control.close();
                }
            }
            Button {
                text: qsTr("Закрыть")
                onClicked: control.close()
            }
        }
    }
}
