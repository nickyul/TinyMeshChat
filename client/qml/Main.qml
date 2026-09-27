import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtMultimedia

ApplicationWindow {
    id: root

    function confirmFiles(urls, peerId) {
        const files = Array.from(urls).filter(url => url.toString().startsWith("file:"));
        if (!files.length) return;
        let recipients = appViewModel.fileRecipients();
        if (peerId) {
            const known = appViewModel.acquaintances.find(peer => peer.peerId === peerId);
            recipients = [{ peerId: peerId, displayName: known ? known.displayName : peerId }];
        }
        if (!recipients.length) return;
        fileConfirmation.showTransfers = peerId.length > 0 &&
            !appViewModel.fileRecipients().some(peer => peer.peerId === peerId);
        fileConfirmation.files = files;
        fileConfirmation.recipients = recipients;
        fileConfirmation.open();
    }


    AppPalette {
        id: appPalette
    }

    width: 1100
    height: 720
    minimumWidth: 820
    minimumHeight: 560
    visible: !qmlSmoke
    title: appViewModel.meshVisible ? qsTr("TinyMesh Chat — беседа") : qsTr("TinyMesh Chat")
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
            title: qsTr("Файлы")
            Action {
                text: qsTr("Передачи файлов")
                onTriggered: fileTransfersDialog.open()
            }
        }
        Menu {
            title: qsTr("Настройки")

            Action {
                text: qsTr("Имя пользователя")
                enabled: !appViewModel.identityRequired
                onTriggered: identitySettings.open()
            }
            Action {
                text: qsTr("Сервер подключения")
                enabled: !appViewModel.identityRequired
                onTriggered: signalingSettings.open()
            }
            Action {
                text: qsTr("Доступ к серверу")
                enabled: !appViewModel.identityRequired
                onTriggered: serverAccess.open()
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

            MenuSeparator {}

            Action {
                text: appViewModel.updateState === "checking"
                      ? qsTr("Проверка обновлений…")
                      : qsTr("Проверить обновления")
                enabled: appViewModel.updateState !== "checking"
                         && appViewModel.updateState !== "downloading"
                         && appViewModel.updateState !== "applying"
                onTriggered: appViewModel.checkForUpdates()
            }
        }
    }

    AcquaintancesPanel {
        id: acquaintancesPanel
        viewModel: appViewModel
        onFilesDropped: function(urls, peerId) { root.confirmFiles(urls, peerId); }
        onFileRequested: function(peerId) {
            outgoingFileDialog.peerId = peerId;
            outgoingFileDialog.open();
        }
        visible: !appViewModel.identityRequired && appViewModel.signalingServerUrl.length > 0
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Math.min(270, root.width * 0.27)
    }

    StackLayout {
        anchors.fill: parent
        anchors.leftMargin: acquaintancesPanel.visible ? acquaintancesPanel.width : 0
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
                    text: qsTr("Выберите имя, которое увидят другие участники беседы")
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
                    text: qsTr("Начать беседу")
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
                            return appViewModel.serverMesh
                                ? qsTr("Подключаемся к беседе…")
                                : qsTr("Передайте ответ участнику, который вас пригласил.");
                        }

                        if (appViewModel.status.length > 0) {
                            return appViewModel.status;
                        }

                        return qsTr("Начните беседу или примите приглашение.");
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
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            text: qsTr("Беседа")
                            font.pixelSize: 23
                            font.bold: true
                            color: appPalette.textPrimary
                        }
                        Label {
                            Layout.fillWidth: true
                            text: appViewModel.degraded ? qsTr("Восстанавливаем связь…")
                                : qsTr("На связи: %1 из %2").arg(appViewModel.connectedPeerCount).arg(appViewModel.expectedPeerCount)
                            color: appViewModel.degraded ? appPalette.warning : appPalette.textSecondary
                            elide: Text.ElideRight
                        }
                    }
                    PrimaryButton {
                        id: inviteButton
                        text: qsTr("Пригласить")
                        onClicked: invitationMenu.open()
                        Menu {
                            id: invitationMenu
                            x: inviteButton.width - width
                            y: inviteButton.height
                            MenuItem {
                                text: appViewModel.serverBusy ? qsTr("Через сервер — ожидание…") : qsTr("Через сервер")
                                enabled: appViewModel.serverReady && !appViewModel.serverBusy && !appViewModel.invitationPending
                                onTriggered: appViewModel.createServerInvitation()
                            }
                            MenuItem {
                                text: qsTr("Создать ручное приглашение")
                                enabled: !appViewModel.invitationPending
                                onTriggered: appViewModel.createInvitation()
                            }
                            MenuSeparator {}
                            MenuItem {
                                text: qsTr("Вставить код приглашения или ответа…")
                                onTriggered: codeImportDialog.open()
                            }
                        }
                    }
                    Button {
                        text: qsTr("Выйти")
                        Accessible.name: qsTr("Выйти из беседы")
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Покинуть беседу и отключиться от её участников")
                        onClicked: appViewModel.leaveMesh()
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

                Pane {
                    Layout.fillWidth: true
                    padding: 8
                    background: Rectangle {
                        color: appPalette.surface
                        radius: 10
                        border.color: appPalette.border
                    }
                    contentItem: Flow {
                        spacing: 6
                        PrimaryButton {
                            text: appViewModel.callActive ? qsTr("Покинуть звонок") : qsTr("Начать звонок")
                            enabled: appViewModel.callActive || appViewModel.connectedPeerCount > 0
                            onClicked: appViewModel.toggleCall()
                        }
                        Button {
                            visible: appViewModel.callActive
                            text: appViewModel.muted ? qsTr("Микрофон выключен") : qsTr("Микрофон включён")
                            checkable: true
                            checked: !appViewModel.muted
                            onClicked: appViewModel.toggleMute()
                        }
                        Button {
                            visible: appViewModel.callActive || appViewModel.viewingScreen
                            text: appViewModel.deafened ? qsTr("Звук выключен") : qsTr("Звук включён")
                            checkable: true
                            checked: !appViewModel.deafened
                            onClicked: appViewModel.toggleDeafen()
                        }
                        Button {
                            text: appViewModel.sharingScreen ? qsTr("Остановить показ") : qsTr("Показать экран…")
                            enabled: appViewModel.sharingScreen || (appViewModel.connectedPeerCount > 0 && !appViewModel.viewingScreen)
                            onClicked: appViewModel.sharingScreen ? appViewModel.stopScreenShare() : screenShareDialog.open()
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: appViewModel.sharingScreen || appViewModel.viewingScreen
                    text: visible ? appViewModel.screenShareTitle : ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: appPalette.textSecondary
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? Math.min(300, root.height * 0.35) : 0
                    visible: appViewModel.sharingScreen || appViewModel.viewingScreen
                    color: "#17232f"
                    radius: 10
                    VideoOutput {
                        id: previewVideo
                        anchors.fill: parent
                        fillMode: VideoOutput.PreserveAspectFit
                        Component.onCompleted: appViewModel.setScreenVideoSink(videoSink)
                        Component.onDestruction: appViewModel.setScreenVideoSink(null)
                    }
                    Button {
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 8
                        text: qsTr("Развернуть")
                        onClicked: expandedStream.open()
                    }
                }

                RowLayout {
                    visible: appViewModel.viewingScreen
                    Layout.fillWidth: true
                    Label { text: qsTr("Звук трансляции") }
                    Slider {
                        from: 0; to: 100; stepSize: 1
                        value: appViewModel.streamVolume
                        onMoved: appViewModel.streamVolume = Math.round(value)
                        Accessible.name: qsTr("Громкость трансляции")
                    }
                    Label { text: appViewModel.streamVolume + "%" }
                }

                SplitView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Frame {
                        SplitView.fillWidth: true
                        SplitView.minimumWidth: 280
                        padding: 0

                        background: Rectangle {
                            color: appPalette.conversationBackground
                            radius: 10
                            border.color: appPalette.border
                        }

                        DropArea {
                            anchors.fill: parent
                            z: 2
                            enabled: appViewModel.connectedPeerCount > 0
                            onEntered: function(drag) { drag.accepted = drag.hasUrls; }
                            onDropped: function(drop) {
                                if (drop.hasUrls) { root.confirmFiles(drop.urls, ""); drop.acceptProposedAction(); }
                            }
                            Rectangle {
                                anchors.fill: parent
                                visible: parent.containsDrag
                                color: "transparent"
                                border.color: appPalette.accent
                                border.width: 3
                                radius: 10
                                z: 2
                            }
                        }
                        ListView {
                            id: messageList

                            anchors.fill: parent
                            anchors.margins: 8
                            clip: true
                            spacing: 5
                            model: appViewModel.messages
                            onCountChanged: positionViewAtEnd()
                            footer: Column {
                                width: messageList.width
                                spacing: 8
                                Repeater {
                                    model: appViewModel.fileTransfers.filter(item => item.id.startsWith("mesh:") && item.currentSession)
                                    delegate: TransferCard {
                                        required property var modelData
                                        width: messageList.width
                                        transfer: modelData
                                        viewModel: appViewModel
                                        onSaveRequested: function(id) { fileTransfersDialog.saveTransfer(id); }
                                    }
                                }
                            }

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
                                                textFormat: Text.PlainText
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
                                            textFormat: Text.PlainText
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
                        SplitView.preferredWidth: 250
                        SplitView.minimumWidth: 200

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
                                                textFormat: Text.PlainText
                                                elide: Text.ElideRight
                                            }

                                            ToolButton {
                                                id: peerActionsButton
                                                text: "⋯"
                                                implicitWidth: 30
                                                visible: !peerDelegate.isSelf
                                                Accessible.name: qsTr("Действия с участником")
                                                ToolTip.visible: hovered
                                                ToolTip.text: Accessible.name
                                                onClicked: peerActions.open()
                                                Menu {
                                                    id: peerActions
                                                    x: peerActionsButton.width - width
                                                    y: peerActionsButton.height
                                                    MenuItem {
                                                        text: qsTr("Отправить файл…")
                                                        enabled: peerDelegate.connected
                                                        onTriggered: { outgoingFileDialog.peerId = peerDelegate.peerId; outgoingFileDialog.open(); }
                                                    }
                                                }
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
                                            visible: peerDelegate.voiceJoined
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

                                            Item { Layout.fillWidth: true }

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
                        persistentSelection: true
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: function(eventPoint) {
                                messageEditMenu.popup(eventPoint.position.x, eventPoint.position.y);
                            }
                        }
                        Menu {
                            id: messageEditMenu
                            MenuItem {
                                text: qsTr("Копировать выделенное")
                                enabled: messageInput.selectedText.length > 0
                                onTriggered: messageInput.copy()
                            }
                            MenuItem {
                                text: qsTr("Вставить")
                                enabled: messageInput.canPaste
                                onTriggered: { messageInput.paste(); messageInput.forceActiveFocus(); }
                            }
                        }
                    }

                    ToolButton {
                        implicitWidth: 40
                        implicitHeight: 40
                        contentItem: Item {
                            implicitWidth: 24
                            implicitHeight: 24
                            Canvas {
                                anchors.centerIn: parent
                                width: 24
                                height: 24
                                opacity: parent.enabled ? 1 : 0.4
                                onPaint: {
                                    const ctx = getContext("2d");
                                    ctx.clearRect(0, 0, width, height);
                                    ctx.strokeStyle = appPalette.textPrimary;
                                    ctx.lineWidth = 1.8;
                                    ctx.lineCap = "round";
                                    ctx.lineJoin = "round";
                                    ctx.beginPath();
                                    ctx.moveTo(8, 12);
                                    ctx.lineTo(15, 5);
                                    ctx.bezierCurveTo(19, 1, 24, 6, 20, 10);
                                    ctx.lineTo(11, 19);
                                    ctx.bezierCurveTo(5, 25, -2, 18, 4, 12);
                                    ctx.lineTo(13, 3);
                                    ctx.stroke();
                                    ctx.beginPath();
                                    ctx.moveTo(7, 15);
                                    ctx.lineTo(16, 6);
                                    ctx.stroke();
                                }
                            }
                        }
                        Accessible.name: qsTr("Отправить файл в беседу")
                        enabled: appViewModel.connectedPeerCount > 0
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Предложить файл всем участникам беседы")
                        onClicked: { outgoingFileDialog.peerId = ""; outgoingFileDialog.open(); }
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
        textFormat: Text.PlainText
        color: appPalette.textSecondary
        HoverHandler { id: statusHover }
        ToolTip.visible: statusHover.hovered
        ToolTip.text: appViewModel.status
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

    Dialog {
        id: fileConfirmation
        property var files: []
        property var recipients: []
        property bool showTransfers: false
        title: qsTr("Отправить файлы?")
        anchors.centerIn: parent
        width: Math.min(480, root.width - 40)
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: ScrollView {
            id: confirmationScroll
            implicitHeight: Math.min(320, confirmationText.implicitHeight)
            contentWidth: availableWidth
            clip: true
            Label {
                id: confirmationText
                width: confirmationScroll.availableWidth
                text: qsTr("Файлы:\n%1\n\nПолучатели:\n%2")
                    .arg(fileConfirmation.files.map(url => decodeURIComponent(url.toString().split("/").pop())).join("\n"))
                    .arg(fileConfirmation.recipients.map(peer => peer.displayName).join("\n"))
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
        }
        onAccepted: {
            for (const file of files)
                for (const peer of recipients) appViewModel.sendFile(file, peer.peerId);
            if (showTransfers) fileTransfersDialog.open();
            else Qt.callLater(function() { messageList.positionViewAtEnd(); });
        }
    }

    Dialog {
        id: expandedStream
        parent: Overlay.overlay
        x: 8; y: 8
        width: parent.width - 16
        height: parent.height - 16
        modal: true
        title: appViewModel.screenShareTitle
        onOpened: appViewModel.setScreenVideoSink(expandedVideo.videoSink)
        onClosed: appViewModel.setScreenVideoSink(previewVideo.videoSink)
        contentItem: ColumnLayout {
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#17232f"
                VideoOutput {
                    id: expandedVideo
                    anchors.fill: parent
                    fillMode: VideoOutput.PreserveAspectFit
                }
            }
            RowLayout {
                Label { visible: appViewModel.viewingScreen; text: qsTr("Звук трансляции") }
                Slider {
                    visible: appViewModel.viewingScreen
                    from: 0; to: 100; stepSize: 1
                    value: appViewModel.streamVolume
                    onMoved: appViewModel.streamVolume = Math.round(value)
                    Accessible.name: qsTr("Громкость трансляции")
                }
                Label { visible: appViewModel.viewingScreen; text: appViewModel.streamVolume + "%" }
                Item { Layout.fillWidth: true }
                Button {
                    visible: appViewModel.sharingScreen
                    text: qsTr("Остановить показ")
                    onClicked: appViewModel.stopScreenShare()
                }
                Button { text: qsTr("Свернуть"); onClicked: expandedStream.close() }
            }
        }
    }

    FileDialog {
        id: outgoingFileDialog
        property string peerId: ""
        title: qsTr("Выбрать файл для отправки")
        fileMode: FileDialog.OpenFile
        onAccepted: {
            root.confirmFiles([selectedFile], peerId);
        }
    }

    FileTransfersDialog {
        id: fileTransfersDialog
        viewModel: appViewModel
    }

    ScreenShareDialog {
        id: screenShareDialog
        viewModel: appViewModel
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

    Pane {
        id: onlineInvitationDialog
        property string invitationId: ""
        property string senderName: ""
        property string serverAddress: ""
        property bool accepting: false
        visible: false
        z: 100
        x: acquaintancesPanel.visible ? acquaintancesPanel.width + 16 : 16
        y: 16
        width: Math.min(460, root.width - x - 16)
        padding: 18
        function open() { accepting = false; visible = true; }
        function close() { visible = false; }
        background: Rectangle { color: "#ffffff"; radius: 12; border.color: "#58788c"; border.width: 2 }
        contentItem: ColumnLayout {
            spacing: 10
            Label { text: qsTr("Приглашение в беседу"); font.bold: true; font.pixelSize: 18 }
            Label {
                Layout.fillWidth: true
                text: qsTr("%1 хочет пообщаться с вами. Микрофон останется выключенным.").arg(onlineInvitationDialog.senderName)
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
            }
            RowLayout {
                PrimaryButton {
                    text: onlineInvitationDialog.accepting ? qsTr("Подключаемся…") : qsTr("Принять")
                    enabled: !onlineInvitationDialog.accepting
                    onClicked: {
                        onlineInvitationDialog.accepting = true;
                        appViewModel.respondToOnlineInvitation(onlineInvitationDialog.invitationId, true);
                    }
                }
                Button {
                    text: qsTr("Отклонить")
                    enabled: !onlineInvitationDialog.accepting
                    onClicked: appViewModel.respondToOnlineInvitation(onlineInvitationDialog.invitationId, false)
                }
            }
        }
    }

    SignalingSettingsDialog {
        id: signalingSettings
        viewModel: appViewModel
    }

    ServerAccessDialog {
        id: serverAccess
        viewModel: appViewModel
    }

    Dialog {
        id: serverJoinDialog
        property string serverAddress: ""
        title: qsTr("Присоединиться к беседе?")
        modal: true
        anchors.centerIn: parent
        width: Math.min(600, root.width - 48)
        onAccepted: appViewModel.acceptServerInvitation()
        onRejected: appViewModel.declineServerInvitation()
        footer: DialogButtonBox {
            alignment: Qt.AlignRight
            spacing: 8
            padding: 12

            Button {
                text: qsTr("Присоединиться")
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                text: qsTr("Отмена")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }
        Label {
            width: parent.width
            textFormat: Text.PlainText
            text: qsTr("Подключиться к серверу %1 и принять приглашение? Микрофон останется выключенным.").arg(serverJoinDialog.serverAddress)
            wrapMode: Text.WrapAnywhere
        }
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
            text: qsTr("TinyMesh Chat\n\nДинамический P2P-чат для небольшой компании. " + "Приглашения передаются вручную или через настроенный сервер сигналинга.\n\n" + "При недоступности прямого соединения поддерживается передача через TURN.")
            wrapMode: Text.WordWrap
        }
    }

    Dialog {
        id: updateDialog

        title: qsTr("Обновление TinyMesh Chat")
        modal: true
        anchors.centerIn: parent
        width: Math.min(560, root.width - 60)
        height: 360
        closePolicy: appViewModel.updateState === "applying"
                     ? Popup.NoAutoClose
                     : Popup.CloseOnEscape

        ColumnLayout {
            anchors.fill: parent
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: {
                    if (appViewModel.updateState === "downloading") {
                        return qsTr("Загрузка версии %1…").arg(appViewModel.updateVersion);
                    }
                    if (appViewModel.updateState === "ready" && appViewModel.meshVisible) {
                        return qsTr("Версия %1 загружена. Выйдите из беседы, чтобы установить её.")
                            .arg(appViewModel.updateVersion);
                    }
                    if (appViewModel.updateState === "ready") {
                        return qsTr("Версия %1 готова к установке.")
                            .arg(appViewModel.updateVersion);
                    }
                    if (appViewModel.updaterPortable) {
                        return qsTr("Доступна версия %1. Portable-сборка обновляется вручную.")
                            .arg(appViewModel.updateVersion);
                    }
                    return qsTr("Доступна новая версия %1.").arg(appViewModel.updateVersion);
                }
                wrapMode: Text.WordWrap
            }

            ProgressBar {
                Layout.fillWidth: true
                visible: appViewModel.updateState === "downloading"
                from: 0
                to: 100
                value: appViewModel.updateProgress
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 180
                visible: appViewModel.updateReleaseNotes.length > 0

                TextArea {
                    text: appViewModel.updateReleaseNotes
                    readOnly: true
                    wrapMode: TextEdit.Wrap
                }
            }

            RowLayout {
                Layout.fillWidth: true

                Item {
                    Layout.fillWidth: true
                }

                Button {
                    visible: appViewModel.updateState === "available"
                             || appViewModel.updateState === "ready"
                    enabled: appViewModel.updateState === "available"
                             || (appViewModel.updateState === "ready"
                                 && !appViewModel.meshVisible)
                    text: {
                        if (appViewModel.updateState === "ready") {
                            return appViewModel.meshVisible
                                ? qsTr("Ожидание выхода из беседы")
                                : qsTr("Установить и перезапустить");
                        }
                        return appViewModel.updaterPortable
                            ? qsTr("Открыть Releases")
                            : qsTr("Скачать");
                    }
                    onClicked: {
                        if (appViewModel.updateState === "ready") {
                            appViewModel.installUpdate();
                        } else {
                            appViewModel.downloadUpdate();
                        }
                    }
                }

                Button {
                    text: qsTr("Закрыть")
                    enabled: appViewModel.updateState !== "applying"
                    onClicked: updateDialog.close()
                }
            }
        }
    }

    Connections {
        target: appViewModel
        function onFileOffered() {
            if (appViewModel.fileTransfers.some(item => item.id.startsWith("personal:") && item.canAccept))
                fileTransfersDialog.open();
            else Qt.callLater(function() { messageList.positionViewAtEnd(); });
        }
        function onScreenShareChanged() {
            if (!appViewModel.sharingScreen && !appViewModel.viewingScreen) expandedStream.close();
        }
        function onErrorRequested(message) {
            if (errorDialog.visible && errorDialog.text === message)
                return;
            errorDialog.text = message;
            errorDialog.open();
        }
        function onSignalingRequested(kind, text) {
            signalingDialog.showSignaling(kind, text);
        }
        function onServerJoinRequested(server) {
            serverJoinDialog.serverAddress = server;
            serverJoinDialog.open();
        }
        function onAccessImportRequested(text) {
            serverAccess.showImport(text);
        }
        function onAccessInvitationReady(link) {
            serverAccess.showInvitation(link);
        }
        function onOnlineInvitationReceived(invitationId, displayName, server) {
            onlineInvitationDialog.invitationId = invitationId;
            onlineInvitationDialog.senderName = displayName;
            onlineInvitationDialog.serverAddress = server;
            onlineInvitationDialog.open();
        }
        function onOnlineInvitationClosed(invitationId) {
            if (onlineInvitationDialog.invitationId === invitationId) {
                onlineInvitationDialog.close();
                onlineInvitationDialog.invitationId = "";
            }
        }
        function onUpdatePromptRequested() {
            updateDialog.open();
        }
    }
}
