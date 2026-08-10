import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: root

    AppPalette {
        id: appPalette
    }

    width: 1100
    height: 720
    minimumWidth: 820
    minimumHeight: 560
    visible: !qmlSmoke
    title: appViewModel.meshVisible ? qsTr("TinyMesh Chat — mesh") : qsTr("TinyMesh Chat")
    color: appPalette.windowBackground

    palette {
        window: appPalette.windowBackground
        windowText: appPalette.textPrimary
        base: appPalette.surface
        alternateBase: appPalette.windowBackground
        text: appPalette.textPrimary
        button: appPalette.controlBackground
        buttonText: appPalette.textPrimary
        highlight: appPalette.accent
        highlightedText: appPalette.accentText
        placeholderText: appPalette.textSecondary
    }

    menuBar: MenuBar {
        Menu {
            title: qsTr("Настройки")

            Action {
                text: qsTr("Имя пользователя")
                enabled: !appViewModel.identityRequired
                onTriggered: identitySettings.open()
            }
            Action {
                text: qsTr("STUN-серверы")
                enabled: !appViewModel.identityRequired
                onTriggered: stunSettings.open()
            }
            Action {
                text: qsTr("Аудио")
                enabled: !appViewModel.identityRequired
                onTriggered: audioSettings.open()
            }
            Action {
                text: qsTr("Ссылки tinymesh://")
                onTriggered: appLinkSettings.open()
            }

            MenuSeparator {}

            Action {
                text: qsTr("Диагностика сети")
                enabled: !appViewModel.identityRequired
                onTriggered: diagnosticsDialog.open()
            }
            Action {
                text: qsTr("О программе")
                onTriggered: aboutDialog.open()
            }
        }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: {
            if (appViewModel.identityRequired) {
                return 0;
            }

            return appViewModel.meshVisible ? 2 : 1;
        }

        Item {
            id: identityPage

            readonly property string displayName: firstName.text.trim()
            readonly property bool canSubmit: displayName.length > 0

            function submitIdentity() {
                if (canSubmit) {
                    appViewModel.createIdentity(displayName);
                }
            }

            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(520, parent.width - 48)
                spacing: 18

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("TinyMesh Chat")
                    font.pixelSize: 32
                    font.bold: true
                    color: appPalette.textPrimary
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Выберите имя, которое увидят другие участники mesh")
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: appPalette.textSecondary
                }

                TextField {
                    id: firstName

                    Layout.fillWidth: true
                    focus: true
                    placeholderText: qsTr("Имя пользователя")
                    maximumLength: 128
                    onAccepted: identityPage.submitIdentity()
                }

                PrimaryButton {
                    Layout.fillWidth: true
                    text: qsTr("Продолжить")
                    enabled: identityPage.canSubmit
                    onClicked: identityPage.submitIdentity()
                }
            }
        }

        Item {
            id: startPage

            readonly property string signalingText: signalingInput.text.trim()
            readonly property bool canImportSignaling: !appViewModel.connecting && signalingText.length > 0

            function importSignaling() {
                if (canImportSignaling) {
                    appViewModel.importSignalingText(signalingText);
                }
            }

            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(620, parent.width - 48)
                spacing: 14

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("TinyMesh Chat")
                    font.pixelSize: 30
                    font.bold: true
                    color: appPalette.textPrimary
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Прямое P2P-общение без центрального сервера сообщений")
                    horizontalAlignment: Text.AlignHCenter
                    color: appPalette.textSecondary
                }

                PrimaryButton {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    text: qsTr("Создать новый mesh")
                    enabled: !appViewModel.connecting
                    onClicked: appViewModel.createMesh()
                }

                Frame {
                    Layout.fillWidth: true
                    enabled: !appViewModel.connecting

                    background: Rectangle {
                        color: appPalette.surface
                        radius: 10
                        border.color: appPalette.border
                    }

                    ColumnLayout {
                        anchors.fill: parent

                        Label {
                            text: qsTr("Подключиться по коду")
                            font.bold: true
                        }

                        ScrollView {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 115

                            TextArea {
                                id: signalingInput

                                placeholderText: qsTr("Вставьте код подключения или ссылку")
                                wrapMode: TextEdit.WrapAnywhere
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true

                            PrimaryButton {
                                Layout.fillWidth: true
                                text: qsTr("Подключиться")
                                enabled: startPage.canImportSignaling
                                onClicked: startPage.importSignaling()
                            }

                            Button {
                                text: qsTr("Открыть файл…")
                                onClicked: importDialog.open()
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: {
                        if (appViewModel.connecting) {
                            return qsTr("Подключение к mesh… Передайте созданный answer пригласившему участнику.");
                        }

                        if (appViewModel.status.length > 0) {
                            return appViewModel.status;
                        }

                        return qsTr("Создайте mesh или импортируйте приглашение.");
                    }
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: appPalette.textSecondary
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
            id: meshPage

            readonly property string messageText: messageInput.text.trim()
            readonly property bool canSendMessage: messageText.length > 0

            onVisibleChanged: {
                if (visible) {
                    Qt.callLater(() => messageInput.forceActiveFocus());
                }
            }

            function sendMessage() {
                if (canSendMessage && appViewModel.sendMessage(messageText)) {
                    messageInput.clear();
                }
            }

            function invitationStateText(state) {
                switch (state) {
                case "gathering":
                    return qsTr("подготовка кода");
                case "awaiting-answer":
                    return qsTr("ожидание ответного кода");
                case "awaiting-connection":
                case "connecting":
                    return qsTr("установка соединения");
                case "awaiting-hello":
                    return qsTr("проверка участника");
                default:
                    return qsTr("обработка");
                }
            }

            function rttColor(rttMs) {
                if (rttMs < 80) {
                    return appPalette.success;
                }
                if (rttMs < 150) {
                    return appPalette.warning;
                }
                return appPalette.danger;
            }

            function audioQualityColor(packetLossPercent, jitterMs) {
                if (packetLossPercent < 1 && jitterMs < 30) {
                    return appPalette.success;
                }
                if (packetLossPercent < 5 && jitterMs < 60) {
                    return appPalette.warning;
                }
                return appPalette.danger;
            }

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
                        color: appPalette.textPrimary
                    }

                    Label {
                        text: appViewModel.degraded ? qsTr("ограниченная связность") : qsTr("прямой P2P mesh")
                        color: appViewModel.degraded ? appPalette.warning : appPalette.textSecondary
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    PrimaryButton {
                        text: qsTr("Вставить код")
                        onClicked: codeImportDialog.open()
                    }

                    PrimaryButton {
                        text: appViewModel.invitationPending ? qsTr("Подготовка ICE…") : qsTr("Пригласить")
                        enabled: !appViewModel.invitationPending
                        onClicked: appViewModel.createInvitation()
                    }

                    Button {
                        text: qsTr("Выйти")
                        onClicked: appViewModel.leaveMesh()
                    }

                    Label {
                        text: qsTr("Прямые связи: %1/%2").arg(appViewModel.connectedPeerCount).arg(appViewModel.expectedPeerCount)
                        color: appPalette.accentMutedText
                        font.bold: true
                        padding: 9

                        background: Rectangle {
                            color: appPalette.accentMuted
                            radius: 9
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: appViewModel.invitationPending

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Приглашение: %1").arg(meshPage.invitationStateText(appViewModel.invitationState))
                        color: appPalette.textSecondary
                    }

                    Button {
                        text: qsTr("Отменить")
                        onClicked: appViewModel.cancelInvitation()
                    }

                    Button {
                        text: qsTr("Новое приглашение")
                        onClicked: appViewModel.recreateInvitation()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true

                    Item {
                        Layout.fillWidth: true
                    }

                    PrimaryButton {
                        text: appViewModel.callActive ? qsTr("Выйти из звонка") : qsTr("Начать звонок")
                        onClicked: appViewModel.toggleCall()
                    }

                    Button {
                        visible: appViewModel.callActive
                        text: appViewModel.muted ? qsTr("Включить микрофон") : qsTr("Выключить микрофон")
                        onClicked: appViewModel.toggleMute()
                    }

                    Button {
                        visible: appViewModel.callActive
                        text: appViewModel.deafened ? qsTr("Включить звук") : qsTr("Заглушить звук")
                        onClicked: appViewModel.toggleDeafen()
                    }

                    ProgressBar {
                        Layout.preferredWidth: 90
                        visible: appViewModel.callActive
                        from: 0
                        to: 1
                        value: appViewModel.microphoneLevel
                    }
                }

                SplitView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Frame {
                        SplitView.fillWidth: true
                        SplitView.minimumWidth: 460
                        padding: 0

                        background: Rectangle {
                            color: appPalette.conversationBackground
                            radius: 10
                            border.color: appPalette.border
                        }

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
                                required property date createdAt
                                required property bool local
                                required property int acknowledgedCount
                                required property int expectedCount

                                width: messageList.width
                                height: bubble.height + 6

                                TextMetrics {
                                    id: messageMetrics

                                    text: messageDelegate.text
                                    font: messageText.font
                                }

                                Rectangle {
                                    id: bubble

                                    x: messageDelegate.local ? parent.width - width - 6 : 6
                                    width: Math.min(parent.width * 0.78, Math.max(230, messageMetrics.advanceWidth + 20))
                                    height: body.implicitHeight + 20
                                    radius: 9
                                    color: messageDelegate.local ? appPalette.messageLocalBackground : appPalette.surface
                                    border.color: appPalette.border

                                    ColumnLayout {
                                        id: body

                                        anchors.fill: parent
                                        anchors.margins: 10
                                        spacing: 4

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 8

                                            Label {
                                                Layout.fillWidth: true
                                                text: messageDelegate.author
                                                color: appPalette.textPrimary
                                                font.pixelSize: 12
                                                font.weight: Font.DemiBold
                                                elide: Text.ElideRight
                                            }

                                            Label {
                                                text: Qt.formatTime(messageDelegate.createdAt, "HH:mm")
                                                color: appPalette.textSecondary
                                                font.pixelSize: 12
                                            }
                                        }

                                        Label {
                                            id: messageText

                                            Layout.fillWidth: true
                                            text: messageDelegate.text
                                            wrapMode: Text.Wrap
                                            color: appPalette.textPrimary
                                        }

                                        Label {
                                            Layout.alignment: Qt.AlignRight
                                            visible: messageDelegate.expectedCount > 0
                                            text: messageDelegate.acknowledgedCount === messageDelegate.expectedCount ? qsTr("Доставлено всем") : qsTr("Доставлено: %1 из %2").arg(messageDelegate.acknowledgedCount).arg(messageDelegate.expectedCount)
                                            color: appPalette.textSecondary
                                            font.pixelSize: 11
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Frame {
                        SplitView.preferredWidth: 340
                        SplitView.minimumWidth: 260

                        background: Rectangle {
                            color: appPalette.surface
                            radius: 10
                            border.color: appPalette.border
                        }

                        ColumnLayout {
                            anchors.fill: parent

                            Label {
                                text: qsTr("Участники")
                                font.pixelSize: 17
                                font.bold: true
                            }

                            ListView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                spacing: 3
                                model: appViewModel.peers

                                delegate: Rectangle {
                                    id: peerDelegate

                                    required property string displayName
                                    required property bool connected
                                    required property bool voiceJoined
                                    required property bool muted
                                    required property bool talking
                                    required property bool isSelf
                                    required property string peerId
                                    required property int volume
                                    required property int rttMs
                                    required property real packetLossPercent
                                    required property int audioJitterMs
                                    required property int audioBufferMs

                                    width: ListView.view.width
                                    height: peerContent.implicitHeight + 8
                                    radius: 7
                                    color: peerDelegate.isSelf ? appPalette.participantSelfBackground : "transparent"
                                    border.width: peerDelegate.talking ? 1 : 0
                                    border.color: appPalette.success

                                    ColumnLayout {
                                        id: peerContent

                                        anchors.fill: parent
                                        anchors.margins: 4
                                        spacing: 2

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 6

                                            Rectangle {
                                                Layout.preferredWidth: 8
                                                Layout.preferredHeight: 8
                                                radius: width / 2
                                                color: peerDelegate.connected ? appPalette.success : appPalette.inactive
                                            }

                                            Label {
                                                Layout.fillWidth: true
                                                text: peerDelegate.displayName
                                                elide: Text.ElideRight
                                            }

                                            Label {
                                                visible: peerDelegate.connected && !peerDelegate.isSelf && peerDelegate.rttMs >= 0
                                                text: qsTr("%1 мс").arg(peerDelegate.rttMs)
                                                color: meshPage.rttColor(peerDelegate.rttMs)
                                                font.pixelSize: 11
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 22
                                            spacing: 6

                                            Rectangle {
                                                Layout.preferredWidth: 8
                                                Layout.preferredHeight: 8
                                                visible: peerDelegate.voiceJoined
                                                radius: width / 2
                                                color: peerDelegate.talking ? appPalette.success : "transparent"
                                                border.width: peerDelegate.talking ? 0 : 1
                                                border.color: appPalette.inactive
                                            }

                                            Label {
                                                visible: peerDelegate.voiceJoined
                                                text: peerDelegate.talking ? qsTr("Говорит")
                                                                          : (peerDelegate.muted ? qsTr("Микрофон выкл.") : qsTr("В звонке"))
                                                color: peerDelegate.talking ? appPalette.success
                                                                            : (peerDelegate.muted ? appPalette.warning : appPalette.textSecondary)
                                                font.pixelSize: 10
                                            }

                                            Label {
                                                Layout.fillWidth: true
                                                visible: peerDelegate.voiceJoined && !peerDelegate.isSelf && peerDelegate.packetLossPercent >= 0
                                                text: qsTr("Потери %1% · джиттер %2 мс · буфер %3 мс").arg(peerDelegate.packetLossPercent.toFixed(1)).arg(peerDelegate.audioJitterMs).arg(peerDelegate.audioBufferMs)
                                                color: meshPage.audioQualityColor(peerDelegate.packetLossPercent, peerDelegate.audioJitterMs)
                                                font.pixelSize: 10
                                                elide: Text.ElideRight
                                            }

                                            Slider {
                                                id: peerVolumeSlider

                                                Layout.preferredWidth: 70
                                                Layout.preferredHeight: 22
                                                visible: peerDelegate.voiceJoined && !peerDelegate.isSelf
                                                from: 0
                                                to: 200
                                                stepSize: 5
                                                value: peerDelegate.volume
                                                onMoved: appViewModel.setPeerVolume(peerDelegate.peerId, Math.round(value))

                                                ToolTip.visible: hovered || pressed
                                                ToolTip.text: qsTr("Громкость: %1%").arg(Math.round(value))
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true

                    TextField {
                        id: messageInput

                        Layout.fillWidth: true
                        placeholderText: qsTr("Введите сообщение…")
                        maximumLength: 4096
                        onAccepted: meshPage.sendMessage()
                    }

                    PrimaryButton {
                        text: qsTr("Отправить")
                        enabled: meshPage.canSendMessage
                        onClicked: meshPage.sendMessage()
                    }
                }
            }
        }
    }

    footer: Label {
        height: 28
        visible: appViewModel.meshVisible && appViewModel.status.length > 0
        leftPadding: 12
        verticalAlignment: Text.AlignVCenter
        text: appViewModel.status
        color: appPalette.textSecondary
        elide: Text.ElideRight

        background: Rectangle {
            color: appPalette.surface
            border.color: appPalette.border
        }
    }

    Dialog {
        id: codeImportDialog
        title: qsTr("Вставить код подключения")
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
                placeholderText: qsTr("Вставьте код подключения или ссылку")
                wrapMode: TextEdit.WrapAnywhere
            }
        }
    }

    FileDialog {
        id: importDialog
        title: qsTr("Открыть файл подключения")
        nameFilters: [qsTr("TinyMesh signaling (*.tmcinvite *.tmcanswer)"), qsTr("JSON (*.json)"), qsTr("Все файлы (*)")]
        onAccepted: appViewModel.importSignalingFile(selectedFile)
    }

    FileDialog {
        id: saveDialog
        title: qsTr("Сохранить файл подключения")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("TinyMesh signaling (*.tmcinvite *.tmcanswer)"), qsTr("Все файлы (*)")]
        onAccepted: appViewModel.saveSignalingFile(selectedFile)
    }

    MessageDialog {
        id: errorDialog

        title: qsTr("TinyMesh Chat")
        buttons: MessageDialog.Ok
    }

    SignalingOutputDialog {
        id: signalingDialog

        viewModel: appViewModel
        onSaveRequested: saveDialog.open()
    }

    AppLinkSettingsDialog {
        id: appLinkSettings

        viewModel: appViewModel
    }

    AudioSettingsDialog {
        id: audioSettings

        viewModel: appViewModel
    }

    DiagnosticsDialog {
        id: diagnosticsDialog

        viewModel: appViewModel
    }

    Dialog {
        id: identitySettings
        title: qsTr("Имя пользователя")
        modal: true
        anchors.centerIn: parent
        width: Math.min(420, root.width - 48)
        standardButtons: Dialog.Save | Dialog.Cancel
        onOpened: identityEdit.text = appViewModel.displayName
        onAccepted: appViewModel.updateDisplayName(identityEdit.text)
        ColumnLayout {
            anchors.fill: parent
            TextField {
                id: identityEdit
                Layout.fillWidth: true
                maximumLength: 128
            }
        }
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
            Label {
                text: qsTr("Один stun: URI на строку")
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                TextArea {
                    id: stunEdit
                }
            }
        }
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
            text: qsTr("TinyMesh Chat\n\nДинамический прямой P2P-чат для небольшой компании. " + "Первый offer/answer передаётся вручную, остальные связи строятся автоматически.\n\n" + "TURN и relay не используются.")
            wrapMode: Text.WordWrap
        }
    }

    Connections {
        target: appViewModel
        function onErrorRequested(message) {
            errorDialog.text = message;
            errorDialog.open();
        }
        function onSignalingRequested(kind, text, link) {
            signalingDialog.showSignaling(kind, text, link);
        }
    }
}
