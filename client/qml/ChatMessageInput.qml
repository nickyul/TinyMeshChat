import QtQuick
import QtQuick.Controls

// Kept separate so the multiline input experiment can be reverted independently.
FocusScope {
    id: control
    property alias text: editor.text
    property alias placeholderText: editor.placeholderText
    signal submitted()
    signal pasteRequested()

    AppPalette { id: colors }
    FontMetrics { id: editorMetrics; font: editor.font }
    implicitHeight: Math.max(40, Math.min(editor.implicitHeight,
        Math.ceil(editorMetrics.lineSpacing * 6) + editor.topPadding + editor.bottomPadding) + 4)
    HoverHandler { id: inputHover }
    ToolTip.visible: inputHover.hovered
    ToolTip.text: qsTr("Enter — отправить, Shift+Enter — новая строка")
    function clear() { editor.clear(); }
    function paste() { editor.paste(); }

    Rectangle {
        anchors.fill: parent
        radius: 8
        color: colors.surface
        border.width: editor.activeFocus ? 2 : 1
        border.color: editor.activeFocus ? colors.accent : colors.inactive
    }

    ScrollView {
        id: scroll
        anchors.fill: parent
        anchors.margins: 2
        contentWidth: availableWidth
        contentHeight: Math.max(editor.implicitHeight, availableHeight)
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        TextArea {
            id: editor
            width: scroll.availableWidth
            height: scroll.contentHeight
            focus: true
            wrapMode: TextEdit.Wrap
            textFormat: TextEdit.PlainText
            selectByMouse: true
            persistentSelection: true
            background: null
            leftPadding: 12
            rightPadding: 12
            topPadding: 10
            bottomPadding: 10
            color: colors.textPrimary
            placeholderTextColor: colors.textSecondary
            selectionColor: colors.accent
            selectedTextColor: colors.accentText
            Accessible.name: placeholderText
            // TextArea has no maximumLength property. Reject an oversized edit
            // rather than truncating an existing draft when inserting in the middle.
            property string acceptedText: ""
            property int acceptedCursor: 0
            onCursorPositionChanged: {
                if (length <= 4096) acceptedCursor = cursorPosition;
            }
            onTextChanged: {
                if (length > 4096) {
                    const previousCursor = acceptedCursor;
                    text = acceptedText;
                    cursorPosition = Math.min(previousCursor, length);
                } else {
                    acceptedText = text;
                }
            }
            Keys.onPressed: function(event) {
                if (event.matches(StandardKey.Paste)) {
                    control.pasteRequested();
                    event.accepted = true;
                } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                           && !(event.modifiers & Qt.ShiftModifier) && !inputMethodComposing) {
                    if (!event.isAutoRepeat) control.submitted();
                    event.accepted = true;
                }
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: function(point) { editMenu.popup(point.position.x, point.position.y); }
            }
            Menu {
                id: editMenu
                MenuItem {
                    text: qsTr("Копировать выделенное")
                    enabled: editor.selectedText.length > 0
                    onTriggered: editor.copy()
                }
                MenuItem {
                    text: qsTr("Вставить")
                    onTriggered: {
                        control.pasteRequested();
                        control.forceActiveFocus();
                    }
                }
            }
        }
    }
}
