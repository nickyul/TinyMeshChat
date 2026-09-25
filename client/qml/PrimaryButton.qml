import QtQuick
import QtQuick.Controls

Button {
    id: control

    AppPalette {
        id: colors
    }

    palette.button: colors.accent
    palette.buttonText: colors.accentText

    font.weight: Font.DemiBold
    implicitHeight: 42
}
