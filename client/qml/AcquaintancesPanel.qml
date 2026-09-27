import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml.Models

Pane {
    id: control
    required property var viewModel
    signal fileRequested(string peerId)
    signal filesDropped(var urls, string peerId)
    AppPalette { id: colors }
    padding: 12
    background: Rectangle { color: colors.surface; border.color: colors.border }

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
            delegate: ItemDelegate {
                id: contact
                required property var modelData
                width: ListView.view.width
                implicitHeight: 64
                padding: 8
                Accessible.name: modelData.displayName
                onClicked: actions.open()
                DropArea {
                    anchors.fill: parent
                    z: 2
                    enabled: contact.modelData.presence === "online" && control.viewModel.serverReady
                    onEntered: function(drag) { drag.accepted = drag.hasUrls; }
                    onDropped: function(drop) {
                        if (drop.hasUrls) {
                            control.filesDropped(drop.urls, contact.modelData.peerId);
                            drop.acceptProposedAction();
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        visible: parent.containsDrag
                        color: "transparent"
                        border.color: colors.accent
                        border.width: 2
                        radius: 10
                    }
                }
                background: Rectangle {
                    radius: 10
                    color: contact.hovered || contact.down ? colors.accentMuted : "transparent"
                }
                contentItem: RowLayout {
                    spacing: 10
                    Rectangle {
                        Layout.preferredWidth: 34
                        Layout.preferredHeight: 34
                        radius: 12
                        color: colors.accentMuted
                        Label {
                            anchors.centerIn: parent
                            text: contact.modelData.displayName.trim().slice(0, 1).toUpperCase()
                            color: colors.accentMutedText
                            font.bold: true
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        Label {
                            Layout.fillWidth: true
                            text: contact.modelData.displayName
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            font.weight: Font.DemiBold
                            color: colors.textPrimary
                        }
                        Label {
                            Layout.fillWidth: true
                            text: contact.modelData.inviting || contact.modelData.requestingEntry ? qsTr("Ожидаем ответ…")
                                : contact.modelData.inMesh ? qsTr("В беседе")
                                : contact.modelData.presence === "online" ? qsTr("Онлайн")
                                : contact.modelData.presence === "offline" ? qsTr("Офлайн") : qsTr("Недоступен")
                            color: contact.modelData.presence === "online" ? colors.success : colors.textSecondary
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }
                    ToolButton {
                        text: "⋯"
                        implicitWidth: 32
                        Accessible.name: qsTr("Действия со знакомым")
                        ToolTip.visible: hovered
                        ToolTip.text: Accessible.name
                        onClicked: actions.open()

                    }
                }
                Menu {
                    id: actions
                    x: Math.max(0, contact.width - width)
                    y: contact.height
                    MenuItem {
                        text: qsTr("Пригласить в беседу")
                        enabled: contact.modelData.presence === "online" && !contact.modelData.inviting
                            && !contact.modelData.inMesh && !control.viewModel.connecting
                            && !control.viewModel.serverBusy && !control.viewModel.invitationPending
                        onTriggered: control.viewModel.inviteAcquaintance(contact.modelData.peerId)
                    }
                    Instantiator {
                        model: contact.modelData.presence === "online" && contact.modelData.inConversation ? 1 : 0
                        delegate: MenuItem {
                            text: contact.modelData.requestingEntry ? qsTr("Ожидаем входа…") : qsTr("Попроситься в беседу")
                            enabled: !control.viewModel.meshVisible && !control.viewModel.connecting
                                && !control.viewModel.serverBusy && !contact.modelData.requestingEntry
                            onTriggered: control.viewModel.requestConversationEntry(contact.modelData.peerId)
                        }
                        onObjectAdded: function(index, object) { actions.insertItem(1, object); }
                        onObjectRemoved: function(index, object) { actions.removeItem(object); }
                    }
                    MenuItem {
                        text: qsTr("Отправить файл…")
                        enabled: contact.modelData.presence === "online" && control.viewModel.serverReady
                        onTriggered: control.fileRequested(contact.modelData.peerId)
                    }
                }
            }
        }
    }
}
