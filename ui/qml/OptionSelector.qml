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

    // Config strings the daemon accepts, as reported by ds4ipc. Never
    // hand-written here, so the UI cannot offer a value ds4-ctl rejects.
    required property var values
    // value -> display text. Looked up by value rather than by position, so
    // the shared list is free to change order without silently mislabelling
    // an entry. Values with no entry display as themselves.
    property var labelMap: ({})
    // Currently active value as reported by the daemon.
    property string current: ""

    signal selected(string value)

    model: values.map(value => labelMap[value] !== undefined ? labelMap[value] : value)

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
