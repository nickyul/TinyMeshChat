import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: control
    required property var viewModel
    property var sources: []
    title: qsTr("Показать экран или окно")
    modal: true
    anchors.centerIn: parent
    width: Math.min(600, parent.width - 40)
    onOpened: {
        sources = viewModel.screenSources();
        sourcePicker.currentIndex = sources.length > 0 ? 0 : -1;
        systemSound.checked = false;
        qualityPicker.currentIndex = [720, 1080, 1440].indexOf(viewModel.screenQualityHeight);
        fpsPicker.currentIndex = viewModel.screenQualityFps === 60 ? 1 : 0;
    }
    ColumnLayout {
        width: parent.width
        spacing: 16
        Label {
            Layout.fillWidth: true
            text: qsTr("Участники смогут выбрать вашу трансляцию для просмотра. Вы можете смотреть их экраны во время показа.")
            wrapMode: Text.WordWrap
        }
        ComboBox {
            id: sourcePicker
            Layout.fillWidth: true
            model: control.sources
            textRole: "title"
            valueRole: "index"
            displayText: currentIndex < 0 ? qsTr("Выберите экран или окно") : currentText
        }
        Button {
            text: qsTr("Обновить список")
            onClicked: {
                control.sources = control.viewModel.screenSources();
                sourcePicker.currentIndex = -1;
            }
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Не видите нужное окно? Разверните его и обновите список.")
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Label {
                text: qsTr("Качество")
            }
            ComboBox {
                id: qualityPicker
                model: ["720p", "1080p", "1440p"]
                Layout.fillWidth: true
            }
            ComboBox {
                id: fpsPicker
                model: ["30 FPS", "60 FPS"]
            }
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Это верхний предел. Качество автоматически снижается при слабом соединении у каждого зрителя.")
            wrapMode: Text.WordWrap
        }
        Switch {
            id: systemSound
            text: qsTr("Передавать звук компьютера")
            enabled: control.viewModel.systemAudioSupported
        }
        Label {
            Layout.fillWidth: true
            visible: systemSound.checked
            text: qsTr("Будет передаваться системный звук, включая другие приложения.")
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            Button { text: qsTr("Отмена"); onClicked: control.close() }
            PrimaryButton {
                text: qsTr("Начать показ")
                enabled: sourcePicker.currentIndex >= 0
                onClicked: {
                    control.viewModel.startScreenShare(sourcePicker.currentValue,
                        systemSound.checked, [720, 1080, 1440][qualityPicker.currentIndex],
                        fpsPicker.currentIndex === 1 ? 60 : 30);
                    if (control.viewModel.sharingScreen) {
                        control.close();
                    }
                }
            }
        }
    }
}
