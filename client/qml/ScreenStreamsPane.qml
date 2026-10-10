import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Frame {
    id: pane
    required property var viewModel
    visible: tiles.count > 0
    implicitHeight: visible ? Math.min(420, grid.implicitHeight + footer.implicitHeight + 28) : 0
    padding: 6
    ColumnLayout {
        anchors.fill: parent
        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            GridLayout {
                id: grid
                width: scroll.availableWidth
                columns: Math.max(1, Math.min(3, Math.floor(width / 360)))
                columnSpacing: 8
                rowSpacing: 8
                Repeater {
                    id: tiles
                    model: pane.viewModel.screenStreams
                    delegate: ScreenStreamTile {
                        required property var model
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        viewModel: pane.viewModel
                        streamData: ({
                            streamId: model.streamId,
                            peerId: model.peerId,
                            title: model.title,
                            local: model.local,
                            hasAudio: model.hasAudio,
                            selected: model.selected,
                            audioSelected: model.audioSelected,
                            state: model.state,
                            error: model.error,
                            requestedHeight: model.requestedHeight,
                            requestedFps: model.requestedFps,
                            qualityText: model.qualityText
                        })
                        onExpandRequested: function(id) {
                            pane.expand(id);
                        }
                    }
                }
            }
        }
        RowLayout {
            id: footer
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Звук только выбранной трансляции")
                wrapMode: Text.WordWrap
            }
            Slider {
                visible: pane.viewModel.viewingScreen
                from: 0
                to: 100
                stepSize: 1
                value: pane.viewModel.streamVolume
                onMoved: pane.viewModel.streamVolume = Math.round(value)
                Accessible.name: qsTr("Громкость трансляции")
            }
        }
    }
    function expand(id) {
        expandedDialog.streamId = id;
        expandedDialog.snapshot = viewModel.screenStream(id);
        expandedDialog.open();
    }

    Dialog {
        id: expandedDialog
        property string streamId: ""
        property var snapshot: ({})
        parent: Overlay.overlay
        x: 8
        y: 8
        width: parent.width - 16
        height: parent.height - 16
        modal: true
        title: qsTr("Демонстрация экрана")
        standardButtons: Dialog.Close
        onClosed: {
            streamId = "";
            snapshot = ({});
        }
        contentItem: ScreenStreamTile {
            viewModel: pane.viewModel
            streamData: expandedDialog.snapshot
            expanded: true
        }
        Connections {
            target: pane.viewModel
            function onScreenShareChanged() {
                if (!expandedDialog.streamId) {
                    return;
                }
                expandedDialog.snapshot = pane.viewModel.screenStream(expandedDialog.streamId);
                if (!expandedDialog.snapshot.selected) {
                    expandedDialog.close();
                }
            }
        }
    }
}
