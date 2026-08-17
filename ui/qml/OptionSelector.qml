import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Ds4Translator

// A ComboBox bound to a daemon setting, with Apply and Reset.
//
// Picking an entry only stages the change: nothing is sent until Apply, so a
// mis-click costs nothing and Reset puts the box back to what the daemon
// currently reports. The daemon stays the source of truth -- the box follows
// a status refresh whenever there is no staged change to lose.
RowLayout {
    id: root

    // Config strings the daemon accepts, as reported by ds4ipc. Never
    // hand-written here, so the UI cannot offer a value ds4-ctl rejects.
    required property var values
    // value -> display text. Looked up by value rather than by position, so
    // the shared list is free to change order without silently mislabelling
    // an entry. Values with no entry display as themselves.
    property var labelMap: ({})
    // Currently active value as reported by the daemon.
    property string current: ""

    // Staged value: what Apply would send.
    readonly property string selectedValue:
        box.currentIndex >= 0 && box.currentIndex < values.length ? values[box.currentIndex] : ""
    readonly property bool modified: selectedValue !== "" && selectedValue !== current

    signal selected(string value)

    spacing: 8

    function syncFromDaemon() {
        const index = values.indexOf(current);
        if (index < 0 || index === box.currentIndex) {
            return;
        }
        box.currentIndex = index;
    }

    onCurrentChanged: {
        // A staged change the user has not applied yet outranks a status
        // refresh; without this, a poll would silently undo their selection.
        if (!modified) {
            syncFromDaemon();
        }
    }
    Component.onCompleted: syncFromDaemon()

    ComboBox {
        id: box
        Layout.fillWidth: true
        model: root.values.map(value =>
            root.labelMap[value] !== undefined ? root.labelMap[value] : value)

        // Hold off background polling while the list is open: a refresh
        // landing mid-selection would move the highlighted entry under the
        // user.
        Connections {
            target: box.popup
            function onVisibleChanged() {
                if (box.popup.visible) {
                    DaemonController.beginInteraction();
                } else {
                    DaemonController.endInteraction();
                }
            }
        }
    }

    Button {
        text: qsTr("Apply")
        enabled: root.enabled && root.modified
        onClicked: root.selected(root.selectedValue)
    }

    Button {
        text: qsTr("Reset")
        // Discards the staged change; it never sends anything to the daemon.
        enabled: root.enabled && root.modified
        onClicked: root.syncFromDaemon()
    }
}
