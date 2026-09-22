import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: control
    required property var viewModel
    title: qsTr("Знакомые")
    modal: true
    anchors.centerIn: parent
    width: Math.min(620, parent.width - 48)
    height: Math.min(540, parent.height - 48)
    standardButtons: Dialog.Close

    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        Label {
            Layout.fillWidth: true
            text: qsTr("Сервер: %1").arg(control.viewModel.signalingStatus)
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            text: control.viewModel.meshVisible
                ? qsTr("Пригласите онлайн-знакомого в текущий mesh. Получатель должен подтвердить подключение.")
                : qsTr("Для отправки приглашения создайте mesh. Получать приглашения можно уже сейчас.")
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            visible: control.viewModel.acquaintances.length === 0
            text: qsTr("Знакомые появятся после первого P2P-подключения через ссылку или ручное приглашение.")
            wrapMode: Text.WordWrap
        }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            model: control.viewModel.acquaintances
            ScrollBar.vertical: ScrollBar {}
            delegate: RowLayout {
                required property var modelData
                width: ListView.view.width
                spacing: 12
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        Layout.fillWidth: true
                        text: modelData.displayName
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.bold: true
                    }
                    Label {
                        text: modelData.presence === "online" ? qsTr("Онлайн")
                            : modelData.presence === "offline" ? qsTr("Офлайн")
                            : modelData.presence === "legacy" ? qsTr("Старая запись — требуется новое знакомство") : qsTr("Статус неизвестен")
                    }
                }
                Button {
                    text: modelData.inMesh ? qsTr("В mesh") : modelData.inviting ? qsTr("Ожидаем ответ…") : qsTr("Пригласить")
                    enabled: modelData.presence === "online" && !modelData.inviting && !modelData.inMesh
                        && control.viewModel.meshVisible && !control.viewModel.serverBusy
                        && !control.viewModel.invitationPending
                    onClicked: control.viewModel.inviteAcquaintance(modelData.peerId)
                }
            }
        }
        Label {
            Layout.fillWidth: true
            text: control.viewModel.status
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
        }
    }
}
