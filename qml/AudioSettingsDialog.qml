import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: control

    required property var viewModel
    readonly property string defaultDeviceName: "Системное устройство по умолчанию"

    function qualityIndex(qualityKbps) {
        if (qualityKbps === 24) {
            return 0;
        }
        if (qualityKbps === 48) {
            return 2;
        }
        return 1;
    }

    title: qsTr("Настройки аудио")
    modal: true
    anchors.centerIn: parent
    width: Math.min(640, parent.width - 60)
    height: Math.min(620, parent.height - 60)
    standardButtons: Dialog.Save | Dialog.Cancel

    onClosed: {
        if (control.viewModel.microphoneTest) {
            control.viewModel.toggleMicrophoneTest();
        }
    }

    onOpened: {
        control.viewModel.refreshAudioDevices();

        const captureDevice = control.viewModel.captureDevice.length ? control.viewModel.captureDevice : control.defaultDeviceName;
        const playbackDevice = control.viewModel.playbackDevice.length ? control.viewModel.playbackDevice : control.defaultDeviceName;

        captureCombo.currentIndex = Math.max(0, captureCombo.find(captureDevice));
        playbackCombo.currentIndex = Math.max(0, playbackCombo.find(playbackDevice));
        aecSwitch.checked = control.viewModel.echoCancellation;
        nsSwitch.checked = control.viewModel.noiseSuppression;
        agcSwitch.checked = control.viewModel.automaticGainControl;
        volumeSlider.value = control.viewModel.outputVolume;
        qualityCombo.currentIndex = control.qualityIndex(control.viewModel.qualityKbps);
    }

    onAccepted: control.viewModel.updateAudioPreferences(captureCombo.currentText, playbackCombo.currentText, aecSwitch.checked, nsSwitch.checked, agcSwitch.checked, Math.round(volumeSlider.value), qualityCombo.currentValue)

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            text: qsTr("Микрофон")
        }

        ComboBox {
            id: captureCombo

            Layout.fillWidth: true
            model: control.viewModel.captureDevices
        }

        Label {
            text: qsTr("Динамики")
        }

        ComboBox {
            id: playbackCombo

            Layout.fillWidth: true
            model: control.viewModel.playbackDevices
        }

        RowLayout {
            Layout.fillWidth: true

            Button {
                text: control.viewModel.microphoneTest ? qsTr("Остановить и прослушать") : qsTr("Проверить микрофон")
                onClicked: control.viewModel.toggleMicrophoneTest()
            }

            ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: 1
                value: control.viewModel.microphoneLevel
            }
        }

        Switch {
            id: aecSwitch

            text: qsTr("Подавление эха (AEC)")
        }

        Switch {
            id: nsSwitch

            text: qsTr("Подавление шума")
        }

        Switch {
            id: agcSwitch

            text: qsTr("Автоматическая громкость микрофона")
        }

        Label {
            text: qsTr("Общая громкость: %1%").arg(Math.round(volumeSlider.value))
        }

        Slider {
            id: volumeSlider

            Layout.fillWidth: true
            from: 0
            to: 200
            stepSize: 5
        }

        Label {
            text: qsTr("Качество Opus")
        }

        ComboBox {
            id: qualityCombo

            Layout.fillWidth: true
            textRole: "text"
            valueRole: "value"
            model: [
                {
                    text: qsTr("Экономное — 24 кбит/с"),
                    value: 24
                },
                {
                    text: qsTr("Сбалансированное — 32 кбит/с"),
                    value: 32
                },
                {
                    text: qsTr("Высокое — 48 кбит/с"),
                    value: 48
                }
            ]
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Микрофон передаётся постоянно, пока вы в звонке и он не выключен. " + "Opus DTX уменьшает трафик во время тишины.")
            wrapMode: Text.WordWrap
            color: control.palette.placeholderText
        }

        Item {
            Layout.fillHeight: true
        }
    }
}
