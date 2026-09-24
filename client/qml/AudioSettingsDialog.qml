import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: control

    required property var viewModel
    readonly property string defaultDeviceName: qsTr("Системное устройство по умолчанию")

    function qualityIndex(value) {
        return value === 24 ? 0 : (value === 48 ? 2 : 1);
    }

    title: qsTr("Настройки аудио")
    modal: true
    anchors.centerIn: parent
    width: Math.min(680, parent.width - 60)
    height: Math.min(720, parent.height - 60)
    standardButtons: Dialog.Save | Dialog.Cancel

    onOpened: {
        control.viewModel.prepareAudioSettings();
        control.viewModel.refreshAudioDevices();

        const capture = control.viewModel.captureDevice.length
            ? control.viewModel.captureDevice : control.defaultDeviceName;
        const playback = control.viewModel.playbackDevice.length
            ? control.viewModel.playbackDevice : control.defaultDeviceName;
        captureCombo.currentIndex = Math.max(0, captureCombo.find(capture));
        playbackCombo.currentIndex = Math.max(0, playbackCombo.find(playback));
        inputModeCombo.currentIndex = control.viewModel.inputMode;
        vadThresholdSlider.value = control.viewModel.vadThreshold;
        vadHangoverSpin.value = control.viewModel.vadHangoverMs;
        aecSwitch.checked = control.viewModel.echoCancellation;
        highPassSwitch.checked = control.viewModel.highPassFilter;
        nsSwitch.checked = control.viewModel.noiseSuppression;
        nsLevelCombo.currentIndex = control.viewModel.noiseSuppressionLevel;
        agcSwitch.checked = control.viewModel.automaticGainControl;
        agcTargetSpin.value = control.viewModel.agcTargetLevelDbfs;
        agcGainSpin.value = control.viewModel.agcCompressionGainDb;
        agcLimiterSwitch.checked = control.viewModel.agcLimiter;
        volumeSlider.value = control.viewModel.outputVolume;
        qualityCombo.currentIndex = control.qualityIndex(control.viewModel.qualityKbps);
        advancedToggle.checked = false;
    }

    onAccepted: control.viewModel.updateAudioPreferences({
        captureDevice: captureCombo.currentText,
        playbackDevice: playbackCombo.currentText,
        echoCancellation: aecSwitch.checked,
        highPassFilter: highPassSwitch.checked,
        noiseSuppression: nsSwitch.checked,
        noiseSuppressionLevel: nsLevelCombo.currentIndex,
        automaticGainControl: agcSwitch.checked,
        agcTargetLevelDbfs: agcTargetSpin.value,
        agcCompressionGainDb: agcGainSpin.value,
        agcLimiter: agcLimiterSwitch.checked,
        outputVolume: Math.round(volumeSlider.value),
        qualityKbps: qualityCombo.currentValue,
        inputMode: inputModeCombo.currentIndex,
        vadThreshold: vadThresholdSlider.value,
        vadHangoverMs: vadHangoverSpin.value
    })

    onClosed: {
        if (control.viewModel.microphoneTest) {
            control.viewModel.toggleMicrophoneTest();
        }
        control.viewModel.cancelAudioSettingsEdit();
    }

    ScrollView {
        id: settingsScroll

        anchors.fill: parent
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: settingsScroll.availableWidth
            spacing: 10

            Label { text: qsTr("Микрофон") }
            ComboBox {
                id: captureCombo
                Layout.fillWidth: true
                model: control.viewModel.captureDevices
            }

            Label { text: qsTr("Динамики") }
            ComboBox {
                id: playbackCombo
                Layout.fillWidth: true
                model: control.viewModel.playbackDevices
            }

            RowLayout {
                Layout.fillWidth: true

                Button {
                    text: control.viewModel.microphoneTest
                        ? qsTr("Остановить мониторинг") : qsTr("Слушать микрофон")
                    onClicked: control.viewModel.toggleMicrophoneTest()
                }
                ProgressBar {
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: control.viewModel.microphoneLevel
                }
            }

            Label {
                Layout.fillWidth: true
                visible: control.viewModel.callActive && control.viewModel.microphoneTest
                text: qsTr("Мониторинг добавляет обработанный микрофон к звуку собеседников. Передача микрофона в звонок временно приостановлена.")
                wrapMode: Text.WordWrap
                color: control.palette.accent
            }

            Label { text: qsTr("Режим передачи") }
            ComboBox {
                id: inputModeCombo
                Layout.fillWidth: true
                model: [qsTr("По голосовой активности"), qsTr("Push-to-Talk")]
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: inputModeCombo.currentIndex === 0

                Label {
                    text: qsTr("Порог голоса: %1").arg(vadThresholdSlider.value.toFixed(2))
                }
                Slider {
                    id: vadThresholdSlider
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    stepSize: 0.01
                }
                RowLayout {
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Задержка отключения VAD, мс")
                    }
                    SpinBox {
                        id: vadHangoverSpin
                        from: 0
                        to: 2000
                        stepSize: 10
                        editable: true
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: inputModeCombo.currentIndex === 1

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Системная кнопка: %1").arg(control.viewModel.pttBindingName)
                    }
                    Button {
                        text: control.viewModel.pttBindingCapturing
                            ? qsTr("Нажмите кнопку…") : qsTr("Назначить")
                        enabled: !control.viewModel.pttBindingCapturing
                        onClicked: control.viewModel.beginPttBindingCapture()
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Можно назначить одну клавишу, среднюю или боковую кнопку мыши. PTT работает и когда окно не в фокусе.")
                    wrapMode: Text.WordWrap
                    color: control.palette.placeholderText
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
            ComboBox {
                id: nsLevelCombo
                Layout.fillWidth: true
                enabled: nsSwitch.checked
                model: [qsTr("Низкое"), qsTr("Умеренное"), qsTr("Высокое"), qsTr("Очень высокое")]
            }
            Switch {
                id: agcSwitch
                text: qsTr("Автоматическая громкость микрофона (AGC)")
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

            Label { text: qsTr("Качество Opus") }
            ComboBox {
                id: qualityCombo
                Layout.fillWidth: true
                textRole: "text"
                valueRole: "value"
                model: [
                    { text: qsTr("Экономное — 24 кбит/с"), value: 24 },
                    { text: qsTr("Сбалансированное — 32 кбит/с"), value: 32 },
                    { text: qsTr("Высокое — 48 кбит/с"), value: 48 }
                ]
            }

            Button {
                id: advancedToggle
                Layout.fillWidth: true
                checkable: true
                text: checked ? qsTr("Скрыть дополнительные параметры")
                              : qsTr("Дополнительно")
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: advancedToggle.checked

                Switch {
                    id: highPassSwitch
                    text: qsTr("Фильтр низких частот (high-pass)")
                }

                RowLayout {
                    Layout.fillWidth: true
                    enabled: agcSwitch.checked
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Целевой уровень AGC, dBFS")
                    }
                    SpinBox {
                        id: agcTargetSpin
                        from: 0
                        to: 31
                        editable: true
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    enabled: agcSwitch.checked
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Усиление компрессора, dB")
                    }
                    SpinBox {
                        id: agcGainSpin
                        from: 0
                        to: 90
                        editable: true
                    }
                }
                Switch {
                    id: agcLimiterSwitch
                    enabled: agcSwitch.checked
                    text: qsTr("Лимитер AGC")
                }
            }
        }
    }
}
