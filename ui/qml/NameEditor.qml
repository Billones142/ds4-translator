import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Ds4Translator

// One row of the name settings: label, editable field, Apply, Reset (discard
// the draft) and Default (ask the daemon for its built-in name).
//
// Unlike the combo boxes, this is not applied on every keystroke -- the field
// holds a draft until Apply, and status refreshes only overwrite the draft
// while the user is not editing it.
ColumnLayout {
    id: root

    required property string controller  // "ds4" | "dualsense"
    required property string label
    property string currentName: ""
    property bool isDefault: true

    readonly property bool edited: field.text !== currentName
    readonly property string problem: DaemonController.validateName(field.text)

    spacing: 2

    onCurrentNameChanged: {
        if (!field.activeFocus) {
            field.text = currentName;
        }
    }
    Component.onCompleted: field.text = currentName

    RowLayout {
        Layout.fillWidth: true

        Label {
            text: root.label
            Layout.preferredWidth: 110
        }

        TextField {
            id: field
            Layout.fillWidth: true
            placeholderText: qsTr("Controller name")
            onAccepted: if (root.edited && root.problem.length === 0) applyButton.clicked()
        }

        Button {
            id: applyButton
            text: qsTr("Apply")
            enabled: root.enabled && root.edited && root.problem.length === 0
            onClicked: DaemonController.setName(root.controller, field.text)
        }

        Button {
            text: qsTr("Reset")
            // Same meaning as next to the combo boxes: discard the edit, do
            // not touch the daemon.
            enabled: root.enabled && root.edited
            onClicked: field.text = root.currentName
        }

        Button {
            text: qsTr("Default")
            // Sends set-name --reset. Nothing to do when the daemon is
            // already reporting its built-in default name.
            enabled: root.enabled && !root.isDefault
            onClicked: DaemonController.resetName(root.controller)
        }
    }

    Label {
        Layout.fillWidth: true
        visible: root.edited && root.problem.length > 0
        text: root.problem
        color: "#c0392b"
        wrapMode: Text.Wrap
    }

    Label {
        Layout.fillWidth: true
        visible: root.isDefault && !root.edited
        text: qsTr("Using the built-in default name.")
        opacity: 0.7
        wrapMode: Text.Wrap
    }
}
