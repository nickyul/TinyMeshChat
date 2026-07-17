import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: root
    width: 1100
    height: 720
    minimumWidth: 820
    minimumHeight: 560
    visible: !qmlSmoke
    title: appViewModel.meshVisible ? "TinyMesh Chat — mesh" : "TinyMesh Chat"
    color: "#f3f5f7"

    property color accent: "#68879b"
    property color accentHover: "#7897aa"
    property color panel: "#ffffff"
    property color subtleText: "#75838d"

    palette.window: "#f3f5f7"
    palette.windowText: "#303b45"
    palette.base: "#ffffff"
    palette.alternateBase: "#f3f5f7"
    palette.text: "#303b45"
    palette.button: "#e4e9ed"
    palette.buttonText: "#303b45"
    palette.highlight: root.accent
    palette.highlightedText: "#ffffff"
    palette.placeholderText: root.subtleText

    menuBar: MenuBar {
        Menu {
            title: qsTr("Mesh")
            Action { text: qsTr("Создать новый mesh"); onTriggered: appViewModel.createMesh() }
            Action { text: qsTr("Создать приглашение"); enabled: appViewModel.meshVisible; onTriggered: appViewModel.createInvitation() }
            Action { text: qsTr("Выйти из mesh"); enabled: appViewModel.meshVisible || appViewModel.connecting; onTriggered: appViewModel.leaveMesh() }
            MenuSeparator {}
            Action { text: qsTr("Импортировать signaling-файл…"); onTriggered: importDialog.open() }
        }
        Menu {
            title: qsTr("Настройки")
            Action { text: qsTr("Имя пользователя"); enabled: !appViewModel.identityRequired; onTriggered: identitySettings.open() }
            Action { text: qsTr("STUN-серверы"); enabled: !appViewModel.identityRequired; onTriggered: stunSettings.open() }
            MenuSeparator {}
            Action { text: qsTr("Диагностика сети"); enabled: !appViewModel.identityRequired; onTriggered: diagnosticsDialog.open() }
            Action { text: qsTr("О программе"); onTriggered: aboutDialog.open() }
        }
    }

    component PrimaryButton: Button {
        palette.button: root.accent
        palette.buttonText: "white"
        font.weight: Font.DemiBold
        implicitHeight: 42
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: appViewModel.identityRequired ? 0 : (appViewModel.meshVisible ? 2 : 1)

        Item {
            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(520, parent.width - 48)
                spacing: 18
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("TinyMesh Chat")
                    font.pixelSize: 32
                    font.bold: true
                    color: "#303b45"
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Выберите имя, которое увидят другие участники mesh")
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: root.subtleText
                }
                TextField {
                    id: firstName
                    Layout.fillWidth: true
                    placeholderText: qsTr("Имя пользователя")
                    maximumLength: 128
                    onAccepted: appViewModel.createIdentity(text)
                }
                PrimaryButton {
                    Layout.fillWidth: true
                    text: qsTr("Продолжить")
                    enabled: firstName.text.trim().length > 0
                    onClicked: appViewModel.createIdentity(firstName.text)
                }
            }
        }

        Item {
            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(620, parent.width - 48)
                spacing: 14
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("TinyMesh Chat")
                    font.pixelSize: 30
                    font.bold: true
                    color: "#303b45"
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Прямое P2P-общение без центрального сервера")
                    horizontalAlignment: Text.AlignHCenter
                    color: root.subtleText
                }
                Item { Layout.preferredHeight: 8 }
                PrimaryButton {
                    Layout.fillWidth: true
                    text: qsTr("Создать новый mesh")
                    enabled: !appViewModel.connecting
                    onClicked: appViewModel.createMesh()
                }
                Frame {
                    Layout.fillWidth: true
                    background: Rectangle { color: root.panel; radius: 10; border.color: "#dce1e5" }
                    ColumnLayout {
                        anchors.fill: parent
                        Label { text: qsTr("Подключиться по коду"); font.bold: true }
                        ScrollView {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 115
                            TextArea {
                                id: signalingInput
                                placeholderText: qsTr("Вставьте строку tmc2:")
                                wrapMode: TextEdit.WrapAnywhere
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            PrimaryButton {
                                Layout.fillWidth: true
                                text: qsTr("Подключиться")
                                enabled: signalingInput.text.trim().length > 0
                                onClicked: appViewModel.importSignalingText(signalingInput.text)
                            }
                            Button { text: qsTr("Открыть файл…"); onClicked: importDialog.open() }
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: appViewModel.connecting
                          ? qsTr("Подключение к mesh… Передайте созданный answer пригласившему участнику.")
                          : (appViewModel.status.length ? appViewModel.status
                                                       : qsTr("Создайте mesh или импортируйте приглашение."))
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: root.subtleText
                }
                Button {
                    Layout.fillWidth: true
                    visible: appViewModel.connecting
                    text: qsTr("Отменить подключение")
                    onClicked: appViewModel.leaveMesh()
                }
            }
        }

        Item {
          ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: qsTr("TinyMesh Chat")
                    font.pixelSize: 23
                    font.bold: true
                    color: "#303b45"
                }
                Label {
                    text: appViewModel.degraded ? qsTr("ограниченная связность") : qsTr("прямой P2P mesh")
                    color: appViewModel.degraded ? "#a66b36" : root.subtleText
                }
                Item { Layout.fillWidth: true }
                PrimaryButton { text: qsTr("Вставить код"); onClicked: codeImportDialog.open() }
                PrimaryButton { text: qsTr("Пригласить"); onClicked: appViewModel.createInvitation() }
                Button { text: qsTr("Выйти"); onClicked: appViewModel.leaveMesh() }
                Label {
                    text: appViewModel.meshSummary
                    color: "#506e7f"
                    font.bold: true
                    padding: 9
                    background: Rectangle { color: "#e4ecef"; radius: 9 }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Голос передаётся напрямую каждому участнику"); color: root.subtleText }
                Item { Layout.fillWidth: true }
                PrimaryButton {
                    text: appViewModel.callActive ? qsTr("Выйти из звонка") : qsTr("Начать звонок")
                    onClicked: appViewModel.toggleCall()
                }
                Button {
                    visible: appViewModel.callActive
                    text: appViewModel.muted ? qsTr("Включить микрофон") : qsTr("Выключить микрофон")
                    onClicked: appViewModel.toggleMute()
                }
            }

            SplitView {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Frame {
                    SplitView.fillWidth: true
                    SplitView.minimumWidth: 460
                    padding: 0
                    background: Rectangle { color: "#e9edf0"; radius: 10; border.color: "#d9dfe4" }
                    ListView {
                        id: messageList
                        anchors.fill: parent
                        anchors.margins: 8
                        clip: true
                        spacing: 5
                        model: appViewModel.messages
                        onCountChanged: positionViewAtEnd()
                        delegate: Item {
                            id: messageDelegate
                            required property string author
                            required property string text
                            required property string timestamp
                            required property bool local
                            required property string delivery
                            width: messageList.width
                            height: bubble.implicitHeight + 6
                            Rectangle {
                                id: bubble
                                x: messageDelegate.local ? parent.width - width - 6 : 6
                                width: Math.min(parent.width * 0.78, Math.max(230, body.implicitWidth + 28))
                                implicitHeight: body.implicitHeight + 20
                                radius: 9
                                color: messageDelegate.local ? "#dceaf0" : "#ffffff"
                                border.color: "#dce1e5"
                                Column {
                                    id: body
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 4
                                    Label { text: messageDelegate.timestamp + "  " + messageDelegate.author; color: root.subtleText; font.pixelSize: 12 }
                                    Label { width: bubble.width - 20; text: messageDelegate.text; wrapMode: Text.Wrap; color: "#303b45" }
                                    Label { visible: messageDelegate.delivery.length > 0; text: qsTr("Доставлено: ") + messageDelegate.delivery; color: root.subtleText; font.pixelSize: 11 }
                                }
                            }
                        }
                    }
                }

                Frame {
                    SplitView.preferredWidth: 270
                    SplitView.minimumWidth: 220
                    background: Rectangle { color: root.panel; radius: 10; border.color: "#dce1e5" }
                    ColumnLayout {
                        anchors.fill: parent
                        Label { text: qsTr("Участники"); font.pixelSize: 17; font.bold: true }
                        ListView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            spacing: 5
                            model: appViewModel.peers
                            delegate: RowLayout {
                                id: peerDelegate
                                required property string displayName
                                required property bool connected
                                required property bool voiceJoined
                                required property bool muted
                                required property bool isSelf
                                width: ListView.view.width
                                Label { text: "●"; color: peerDelegate.connected ? "#729985" : "#a0a8ae" }
                                Label { Layout.fillWidth: true; text: peerDelegate.displayName + (peerDelegate.isSelf ? qsTr(" (вы)") : ""); elide: Text.ElideRight }
                                Label { visible: peerDelegate.voiceJoined || (peerDelegate.isSelf && appViewModel.callActive); text: peerDelegate.muted ? "🔇" : "🎙" }
                            }
                        }
                        Label { text: qsTr("TURN/relay отключён"); color: root.subtleText; font.pixelSize: 12 }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                TextField {
                    id: messageInput
                    Layout.fillWidth: true
                    placeholderText: qsTr("Сообщение участникам mesh…")
                    maximumLength: 4096
                    onAccepted: sendCurrentMessage()
                }
                PrimaryButton { text: qsTr("Отправить"); enabled: messageInput.text.trim().length > 0; onClicked: sendCurrentMessage() }
            }
          }
        }
    }

    footer: Label {
        height: 28
        leftPadding: 12
        verticalAlignment: Text.AlignVCenter
        text: appViewModel.status
        color: root.subtleText
        background: Rectangle { color: "#f8f9fa"; border.color: "#e1e5e9" }
        elide: Text.ElideRight
    }

    function sendCurrentMessage() {
        if (!messageInput.text.trim().length)
            return
        appViewModel.sendMessage(messageInput.text)
        messageInput.clear()
    }

    Dialog {
        id: codeImportDialog
        title: qsTr("Импорт signaling")
        modal: true
        anchors.centerIn: parent
        width: Math.min(680, root.width - 60)
        height: 360
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: codeImportText.clear()
        onAccepted: appViewModel.importSignalingText(codeImportText.text)
        ScrollView {
            anchors.fill: parent
            TextArea {
                id: codeImportText
                placeholderText: qsTr("Вставьте строку tmc2:")
                wrapMode: TextEdit.WrapAnywhere
            }
        }
    }

    FileDialog {
        id: importDialog
        title: qsTr("Открыть signaling-файл")
        nameFilters: [qsTr("TinyMesh signaling (*.tmcinvite *.tmcanswer)"), qsTr("JSON (*.json)"), qsTr("Все файлы (*)")]
        onAccepted: appViewModel.importSignalingFile(selectedFile)
    }

    FileDialog {
        id: saveDialog
        title: qsTr("Сохранить signaling-файл")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("TinyMesh signaling (*.tmcinvite *.tmcanswer)"), qsTr("Все файлы (*)")]
        onAccepted: appViewModel.saveSignalingFile(selectedFile)
    }

    Dialog {
        id: errorDialog
        property string message: ""
        title: qsTr("TinyMesh Chat")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok
        width: Math.min(520, root.width - 60)
        Label { width: parent.width; text: errorDialog.message; wrapMode: Text.WordWrap }
    }

    Dialog {
        id: signalingDialog
        property string signalingText: ""
        property string suggestedName: ""
        title: qsTr("Signaling готов")
        modal: true
        anchors.centerIn: parent
        width: Math.min(760, root.width - 60)
        height: Math.min(480, root.height - 60)
        standardButtons: Dialog.Close
        ColumnLayout {
            anchors.fill: parent
            Label { Layout.fillWidth: true; text: qsTr("Строка уже скопирована. Передайте её другому участнику:"); wrapMode: Text.WordWrap }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                TextArea { text: signalingDialog.signalingText; readOnly: true; wrapMode: TextEdit.WrapAnywhere; selectByMouse: true }
            }
            RowLayout {
                PrimaryButton { text: qsTr("Копировать"); onClicked: appViewModel.copyText(signalingDialog.signalingText) }
                Button { text: qsTr("Сохранить файл…"); onClicked: saveDialog.open() }
                Item { Layout.fillWidth: true }
            }
        }
    }

    Dialog {
        id: identitySettings
        title: qsTr("Имя пользователя")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Save | Dialog.Cancel
        onOpened: identityEdit.text = appViewModel.displayName
        onAccepted: appViewModel.updateDisplayName(identityEdit.text)
        TextField { id: identityEdit; width: 360; maximumLength: 128 }
    }

    Dialog {
        id: stunSettings
        title: qsTr("STUN-серверы")
        modal: true
        anchors.centerIn: parent
        width: Math.min(620, root.width - 60)
        height: 360
        standardButtons: Dialog.Save | Dialog.Cancel
        onOpened: stunEdit.text = appViewModel.stunServersText
        onAccepted: appViewModel.updateStunServers(stunEdit.text)
        ColumnLayout {
            anchors.fill: parent
            Label { text: qsTr("Один stun: URI на строку") }
            ScrollView { Layout.fillWidth: true; Layout.fillHeight: true; TextArea { id: stunEdit } }
        }
    }

    Dialog {
        id: diagnosticsDialog
        title: qsTr("Диагностика сети")
        modal: true
        anchors.centerIn: parent
        width: Math.min(680, root.width - 60)
        height: Math.min(500, root.height - 60)
        standardButtons: Dialog.Close
        ScrollView { anchors.fill: parent; TextArea { text: diagnosticsDialog.visible ? appViewModel.diagnostics() : ""; readOnly: true; wrapMode: TextEdit.Wrap } }
    }

    Dialog {
        id: aboutDialog
        title: qsTr("О программе")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Close
        width: Math.min(520, root.width - 60)
        Label {
            width: parent.width
            text: qsTr("TinyMesh Chat\n\nДинамический прямой P2P-чат для небольшой компании. " +
                       "Первый offer/answer передаётся вручную, остальные связи строятся автоматически.\n\n" +
                       "TURN и relay не используются.")
            wrapMode: Text.WordWrap
        }
    }

    Connections {
        target: appViewModel
        function onErrorRequested(message) {
            errorDialog.message = message
            errorDialog.open()
        }
        function onSignalingRequested(kind, text, suggestedName) {
            signalingDialog.signalingText = text
            signalingDialog.suggestedName = suggestedName
            signalingDialog.title = kind === "offer" ? qsTr("Приглашение готово") : qsTr("Answer готов")
            signalingDialog.open()
        }
    }
}
