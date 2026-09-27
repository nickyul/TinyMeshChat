import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: card
    required property var transfer
    required property var viewModel
    signal saveRequested(string transferId)
    padding: 12
    background: Rectangle { color: "#edf1f3"; radius: 10 }
    contentItem: ColumnLayout {
        Label {
            Layout.fillWidth: true
            text: card.transfer.name
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            font.bold: true
        }
        Label {
            Layout.fillWidth: true
            text: (card.transfer.outgoing ? qsTr("Кому: %1") : qsTr("От: %1")).arg(card.transfer.peerName)
            textFormat: Text.PlainText
            elide: Text.ElideRight
        }
        ProgressBar { Layout.fillWidth: true; value: card.transfer.progress }
        Label {
            Layout.fillWidth: true
            text: qsTr("%1 · %2 / %3 МБ").arg(card.transfer.status)
                .arg((card.transfer.transferred / 1048576).toFixed(1))
                .arg((card.transfer.size / 1048576).toFixed(1))
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Button {
                visible: card.transfer.canAccept
                text: qsTr("Сохранить…")
                onClicked: {
                    card.saveRequested(card.transfer.id);
                }
            }
            Button {
                visible: !card.transfer.finished
                text: card.transfer.canAccept ? qsTr("Отклонить") : qsTr("Отменить")
                onClicked: card.viewModel.cancelFile(card.transfer.id)
            }
            Button {
                visible: card.transfer.finished
                text: qsTr("Убрать из списка")
                onClicked: card.viewModel.dismissFile(card.transfer.id)
            }
        }
    }
}
