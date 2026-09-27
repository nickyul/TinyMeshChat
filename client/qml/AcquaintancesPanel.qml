import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: control
    required property var viewModel
    signal fileRequested(string peerId)
    padding: 16
    background: Rectangle { color: "#ffffff"; border.color: "#dce1e5" }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        Label { text: qsTr("Знакомые"); font.pixelSize: 22; font.bold: true }
        Label {
            Layout.fillWidth: true
            text: qsTr("%1").arg(control.viewModel.signalingStatus)
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            text: control.viewModel.meshVisible
                ? qsTr("Пригласите знакомого в беседу.")
                : qsTr("Выберите знакомого, чтобы начать беседу.")
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            visible: control.viewModel.acquaintances.length === 0
            text: qsTr("Здесь появятся люди, с которыми вы уже общались. Для первого знакомства обменяйтесь приглашением.")
            wrapMode: Text.WordWrap
        }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            model: control.viewModel.acquaintances
            ScrollBar.vertical: ScrollBar {}
            delegate: ColumnLayout {
                required property var modelData
                width: ListView.view.width
                spacing: 4
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
                    text: modelData.inMesh ? qsTr("В беседе") : modelData.inviting ? qsTr("Ожидаем ответ…") : qsTr("Пригласить")
                    enabled: modelData.presence === "online" && !modelData.inviting && !modelData.inMesh
                        && !control.viewModel.connecting && !control.viewModel.serverBusy
                        && !control.viewModel.invitationPending
                    onClicked: control.viewModel.inviteAcquaintance(modelData.peerId)
                }
                Button {
                    text: qsTr("Отправить файл…")
                    enabled: modelData.presence === "online" && control.viewModel.serverReady
                    onClicked: control.fileRequested(modelData.peerId)
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
