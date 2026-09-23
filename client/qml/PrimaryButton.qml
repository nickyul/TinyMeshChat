import QtQuick
import QtQuick.Controls

Button {
    id: control

    palette.button: control.palette.highlight
    palette.buttonText: control.palette.highlightedText
    font.weight: Font.DemiBold
    implicitHeight: 42
}
