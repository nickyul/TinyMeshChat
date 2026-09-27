import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: card
    required property var transfer
    required property var viewModel
    signal saveRequested(string id)
    signal imageRequested(string source, string name)
    padding: 12
    AppPalette { id: colors }
    background: Rectangle {
        color: card.transfer.outgoing ? colors.messageLocalBackground : colors.surface
        border.color: colors.border
        radius: 10
    }
    contentItem: ColumnLayout {
        spacing: 8
        Label {
            Layout.fillWidth: true
            text: card.transfer.name || ""
            textFormat: Text.PlainText
            font.bold: true
            elide: Text.ElideMiddle
        }
        Label {
            Layout.fillWidth: true
            visible: (card.transfer.previewUrl || "").length > 0 && !card.transfer.outgoing && !card.transfer.finished
            text: qsTr("Предпросмотр · оригинал ещё не загружен")
            font.pixelSize: 11
            color: colors.textSecondary
            wrapMode: Text.WordWrap
        }
        Image {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 160 : 0
            visible: (card.transfer.previewUrl || "").length > 0
            source: card.transfer.previewUrl || ""
            sourceSize.width: 256
            sourceSize.height: 256
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Посмотреть изображение")
            activeFocusOnTab: true
            Keys.onReturnPressed: card.imageRequested(card.transfer.imageUrl, card.transfer.name)
            TapHandler { onTapped: card.imageRequested(card.transfer.imageUrl, card.transfer.name) }
        }
        Repeater {
            model: card.transfer.transfers || []
            delegate: ColumnLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 4
                Label {
                    Layout.fillWidth: true
                    text: (modelData.outgoing ? qsTr("Кому: %1") : qsTr("От: %1")).arg(modelData.peerName)
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: colors.textSecondary
                }
                ProgressBar { Layout.fillWidth: true; value: modelData.progress }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("%1 · %2 / %3 МБ").arg(modelData.status)
                        .arg((modelData.transferred / 1048576).toFixed(1))
                        .arg((modelData.size / 1048576).toFixed(1))
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                }
                Flow {
                    Layout.fillWidth: true
                    visible: modelData.saved === true
                    spacing: 4
                    Button {
                        text: qsTr("Открыть")
                        onClicked: card.viewModel.openReceivedFile(modelData.id, false)
                    }
                    Button {
                        text: qsTr("Показать в папке")
                        onClicked: card.viewModel.openReceivedFile(modelData.id, true)
                    }
                }
                RowLayout {
                    Button {
                        visible: modelData.canAccept
                        text: qsTr("Сохранить…")
                        onClicked: card.saveRequested(modelData.id)
                    }
                    Button {
                        visible: !modelData.finished
                        text: modelData.canAccept ? qsTr("Отклонить") : qsTr("Отменить")
                        onClicked: card.viewModel.cancelFile(modelData.id)
                    }
                    ToolButton {
                        visible: modelData.finished
                        text: "×"
                        Accessible.name: qsTr("Убрать передачу")
                        onClicked: card.viewModel.dismissFile(modelData.id)
                    }
                }
            }
        }
    }
}
