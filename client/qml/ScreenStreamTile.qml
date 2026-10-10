import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia

Frame {
    id: tile
    required property var viewModel
    required property var streamData
    property bool expanded: false
    signal expandRequested(string streamId)
    padding: 8
    function statusText(value) {
        switch (value) {
        case "starting":
            return qsTr("Запуск захвата…");
        case "connecting":
            return qsTr("Подключение…");
        case "reconnecting":
            return qsTr("Восстановление соединения…");
        case "failed":
            return qsTr("Не удалось подключиться");
        case "playing":
            return streamData.qualityText || qsTr("Прямая трансляция");
        default:
            return qsTr("Доступно для просмотра");
        }
    }
    background: Rectangle {
        color: "#17232f"
        radius: 8
        border.color: "#38516a"
    }
    contentItem: ColumnLayout {
        spacing: 6
        RowLayout {
            Label {
                Layout.fillWidth: true
                color: "white"
                text: tile.viewModel.screenOwnerName(tile.streamData.peerId || "") + ": "
                    + (tile.streamData.title || "")
                textFormat: Text.PlainText
                elide: Text.ElideRight
            }
            CheckBox {
                visible: !tile.streamData.local
                text: qsTr("Смотреть")
                checked: !!tile.streamData.selected
                onClicked: tile.viewModel.subscribeScreen(tile.streamData.streamId, checked)
            }
            Button {
                visible: !tile.expanded && !!tile.streamData.selected
                text: "⤢"
                Accessible.name: qsTr("Развернуть трансляцию")
                onClicked: tile.expandRequested(tile.streamData.streamId)
            }
        }
        Loader {
            Layout.fillWidth: true
            Layout.fillHeight: tile.expanded
            Layout.preferredHeight: tile.expanded ? -1 : 160
            active: !!tile.streamData.selected
            visible: active
            sourceComponent: Rectangle {
                color: "#0c141c"
                VideoOutput {
                    id: video
                    property string attachedId: ""
                    anchors.fill: parent
                    fillMode: VideoOutput.PreserveAspectFit
                    Component.onCompleted: {
                        attachedId = tile.streamData.streamId;
                        tile.viewModel.setScreenVideoSink(attachedId, videoSink, true);
                    }
                    Component.onDestruction: tile.viewModel.setScreenVideoSink(attachedId, videoSink, false)
                }
                Label {
                    anchors.centerIn: parent
                    visible: tile.streamData.state !== "playing"
                    color: "white"
                    text: tile.statusText(tile.streamData.state)
                }
            }
        }
        Label {
            Layout.fillWidth: true
            text: tile.streamData.error || tile.statusText(tile.streamData.state)
            textFormat: Text.PlainText
            color: tile.streamData.error ? "#ffaaa0" : "#b5c5d5"
            wrapMode: Text.WordWrap
        }
        RowLayout {
            visible: !!tile.streamData.selected
            Label {
                text: qsTr("До")
                color: "#b5c5d5"
            }
            ComboBox {
                model: ["720p", "1080p", "1440p"]
                currentIndex: [720, 1080, 1440].indexOf(tile.streamData.requestedHeight)
                onActivated: tile.viewModel.setScreenQuality(tile.streamData.streamId,
                    [720, 1080, 1440][currentIndex], tile.streamData.requestedFps)
                Accessible.name: qsTr("Максимальное разрешение")
            }
            ComboBox {
                model: ["30 FPS", "60 FPS"]
                currentIndex: tile.streamData.requestedFps === 60 ? 1 : 0
                onActivated: tile.viewModel.setScreenQuality(tile.streamData.streamId,
                    tile.streamData.requestedHeight, currentIndex === 1 ? 60 : 30)
                Accessible.name: qsTr("Максимальная частота кадров")
            }
            Item {
                Layout.fillWidth: true
            }
            Button {
                visible: !tile.streamData.local && !!tile.streamData.hasAudio
                text: tile.streamData.audioSelected ? qsTr("Звук ✓") : qsTr("Звук")
                checkable: true
                checked: !!tile.streamData.audioSelected
                onClicked: tile.viewModel.selectScreenAudio(
                    tile.streamData.audioSelected ? "" : tile.streamData.streamId)
            }
        }
        RowLayout {
            visible: tile.streamData.state === "failed" || !!tile.streamData.local
            Button {
                visible: tile.streamData.state === "failed" && !tile.streamData.local
                text: qsTr("Повторить")
                onClicked: tile.viewModel.retryScreen(tile.streamData.streamId)
            }
            Item {
                Layout.fillWidth: true
            }
            Button {
                visible: !!tile.streamData.local
                text: qsTr("Остановить показ")
                onClicked: tile.viewModel.stopScreenShare()
            }
        }
    }
}
