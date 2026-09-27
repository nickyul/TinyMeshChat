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
    }
    ColumnLayout {
        width: parent.width
        spacing: 16
        Label {
            Layout.fillWidth: true
            text: qsTr("Участники беседы увидят выбранный источник. Одновременно доступна одна трансляция.")
            wrapMode: Text.WordWrap
        }
        ComboBox {
            id: sourcePicker
            Layout.fillWidth: true
            model: control.sources
            textRole: "title"
            valueRole: "index"
        }
        Button {
            text: qsTr("Обновить список")
            onClicked: control.sources = control.viewModel.screenSources()
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
                enabled: sourcePicker.currentIndex >= 0 && !control.viewModel.viewingScreen
                onClicked: {
                    control.viewModel.startScreenShare(sourcePicker.currentValue, systemSound.checked);
                    if (control.viewModel.sharingScreen) control.close();
                }
            }
        }
    }
}
