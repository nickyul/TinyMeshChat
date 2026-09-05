pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog

    required property var viewModel

    title: qsTr("Контакты")
    modal: false
    width: 520
    height: 430
    standardButtons: Dialog.Close

    contentItem: Item {
        Label {
            anchors.centerIn: parent
            visible: contactsList.count === 0
            text: qsTr("Контакты появятся после первого прямого подключения")
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: palette.placeholderText
        }

        ListView {
            id: contactsList

            anchors.fill: parent
            clip: true
            spacing: 6
            model: dialog.viewModel.contacts

            delegate: Rectangle {
                id: contactRow

                required property string peerId
                required property string displayName
                required property string status
                required property date lastSeen
                required property bool requestPending
                required property bool canConnect

                width: contactsList.width
                height: 64
                radius: 8
                color: dialog.palette.alternateBase

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 10

                    Rectangle {
                        Layout.preferredWidth: 10
                        Layout.preferredHeight: 10
                        radius: 5
                        color: contactRow.status === qsTr("В сети") || contactRow.status === qsTr("Подключён")
                               ? "#43a047"
                               : contactRow.status === qsTr("Проверка")
                                 ? "#f9a825"
                                 : contactRow.status === qsTr("Занят")
                                   ? "#ef6c00"
                                   : "#8c8c8c"
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            Layout.fillWidth: true
                            text: contactRow.displayName
                            font.bold: true
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: contactRow.status
                            color: palette.placeholderText
                        }
                    }

                    Button {
                        visible: contactRow.status === qsTr("В сети") || contactRow.requestPending
                        enabled: contactRow.canConnect
                        text: contactRow.requestPending ? qsTr("Ожидание…") : qsTr("Подключиться")
                        onClicked: dialog.viewModel.connectContact(contactRow.peerId)
                    }
                }
            }
        }
    }
}
