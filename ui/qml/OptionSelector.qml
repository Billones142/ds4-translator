import QtQuick
import QtQuick.Controls
import Ds4Translator

// A ComboBox bound to a daemon setting.
//
// The daemon is the source of truth, so the box follows `current` whenever a
// status refresh changes it, and only reports a selection the user made --
// otherwise a poll landing mid-interaction would echo back as a command.
ComboBox {
    id: root

    // Config strings sent to the daemon, parallel to `labels`.
    required property var values
    // Human-readable text, parallel to `values`.
    required property var labels
    // Currently active value as reported by the daemon.
    property string current: ""

    signal selected(string value)

    model: labels

    // Guards the currentIndex writes made to follow the daemon, so
    // onActivated stays "the user picked this".
    property bool syncing: false

    function syncFromDaemon() {
        const index = values.indexOf(current);
        if (index < 0 || index === currentIndex) {
            return;
        }
        syncing = true;
        currentIndex = index;
        syncing = false;
    }

    onCurrentChanged: syncFromDaemon()
    Component.onCompleted: syncFromDaemon()

    // Hold off background polling while the list is open: a refresh landing
    // mid-selection would move the highlighted entry under the user.
    Connections {
        target: root.popup
        function onVisibleChanged() {
            if (root.popup.visible) {
                DaemonController.beginInteraction();
            } else {
                DaemonController.endInteraction();
            }
        }
    }

    onActivated: (index) => {
        if (syncing || index < 0 || index >= values.length) {
            return;
        }
        if (values[index] === current) {
            return;
        }
        root.selected(values[index]);
    }
}
