import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: control
    required property var viewModel
    readonly property bool editable: !viewModel.meshVisible && !viewModel.connecting && !viewModel.serverBusy
    title: qsTr("Доступ к серверу")
    modal: true
    anchors.centerIn: parent
    width: Math.min(680, parent.width - 48)
    height: Math.min(720, parent.height - 48)

    function showImport(text) {
        accessText.text = text;
        open();
    }
    function showInvitation(link) {
        issuedLink.text = link;
        open();
    }

    contentItem: ScrollView {
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width
            spacing: 12
            Label {
                Layout.fillWidth: true
                text: qsTr("Состояние: %1").arg(control.viewModel.signalingStatus)
                wrapMode: Text.WordWrap
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Ваш публичный ключ — передайте владельцу сервера для первого разрешения:")
                wrapMode: Text.WordWrap
            }
            TextField {
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                text: control.viewModel.identityPublicKey
            }
            Button {
                text: qsTr("Скопировать публичный ключ")
                onClicked: control.viewModel.copyText(control.viewModel.identityPublicKey)
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Вставьте ссылку доступа от допущенного пользователя или разрешение от владельца. Для разрешения сначала укажите адрес в настройках сервера.")
                wrapMode: Text.WordWrap
            }
            TextArea {
                id: accessText
                Layout.fillWidth: true
                Layout.preferredHeight: 90
                wrapMode: TextEdit.WrapAnywhere
                selectByMouse: true
                enabled: control.editable
                placeholderText: "tinymesh://access/1#… / tmc-access1:…"
            }
            Label {
                id: preview
                Layout.fillWidth: true
                textFormat: Text.PlainText
                text: {
                    // Re-evaluate when settings or the local identity change too.
                    const server = control.viewModel.signalingServerUrl;
                    const key = control.viewModel.identityPublicKey;
                    return control.viewModel.accessPreview(accessText.text);
                }
                visible: text.length > 0
                wrapMode: Text.WrapAnywhere
            }
            Label {
                Layout.fillWidth: true
                visible: !control.editable || (accessText.text.trim().length > 0 && preview.text.length === 0)
                text: !control.editable ? qsTr("Выйдите из mesh для изменения доступа.")
                    : qsTr("Проверьте ссылку, адрес сервера и соответствие разрешения вашему публичному ключу.")
                wrapMode: Text.WordWrap
            }
            Button {
                text: qsTr("Получить / импортировать доступ")
                enabled: control.editable && preview.text.length > 0
                onClicked: {
                    if (control.viewModel.importServerAccess(accessText.text))
                        accessText.clear();
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: control.palette.mid }
            Label {
                Layout.fillWidth: true
                text: qsTr("Выдать другому пользователю постоянный доступ к этому серверу, включая право приглашать других. Это не добавит его в знакомые или в mesh. Ссылка одноразовая, действует 10 минут и пока вы подключены к серверу.")
                wrapMode: Text.WordWrap
            }
            Button {
                text: qsTr("Создать ссылку доступа")
                enabled: control.viewModel.serverReady
                onClicked: control.viewModel.createAccessInvitation()
            }
            TextArea {
                id: issuedLink
                Layout.fillWidth: true
                Layout.preferredHeight: 90
                visible: text.length > 0
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
            }
            Button {
                text: qsTr("Скопировать ссылку доступа")
                visible: issuedLink.text.length > 0
                onClicked: control.viewModel.copyText(issuedLink.text)
            }
        }
    }
    footer: DialogButtonBox {
        alignment: Qt.AlignRight
        standardButtons: DialogButtonBox.Close
    }
    Connections {
        target: control.viewModel
        function onSignalingServerChanged() {
            if (!control.viewModel.serverReady)
                issuedLink.clear();
        }
    }
}
