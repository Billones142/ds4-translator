import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Ds4Translator

// Preferences of this application itself, not of the daemon: they stay usable
// while the daemon is unreachable, which is why they are not on the emulation
// page.
ScrollView {
    id: root

    contentWidth: availableWidth

    ColumnLayout {
        width: parent.width
        spacing: 16

        GroupBox {
            title: qsTr("Application")
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.bottomMargin: 12

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                Switch {
                    text: qsTr("Show a tray icon")
                    checked: AppController.trayEnabled
                    enabled: AppController.trayAvailable
                    // Toggling writes `checked` imperatively, which drops
                    // the binding above; restoring it keeps the stored
                    // preference the source of truth, so a change that
                    // could not be saved snaps back.
                    onToggled: {
                        AppController.trayEnabled = checked;
                        checked = Qt.binding(() => AppController.trayEnabled);
                    }
                }

                Label {
                    text: AppController.trayAvailable
                        ? qsTr("With the tray icon, closing this window leaves the applet running and the emulated controller type can be switched from the tray menu.")
                        : qsTr("This session has no system tray, so the tray icon is unavailable.")
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                // What the close button does. Only meaningful with an icon
                // in the tray: without one, closing has to end the program or
                // it would keep running with no way to reach it.
                Label {
                    text: qsTr("When the window is closed:")
                    Layout.topMargin: 8
                    enabled: AppController.trayEnabled
                }

                RadioButton {
                    text: qsTr("Keep the applet running in the tray")
                    checked: AppController.closeToTray
                    enabled: AppController.trayEnabled
                    onToggled: {
                        AppController.closeToTray = checked;
                        checked = Qt.binding(() => AppController.closeToTray);
                    }
                }

                RadioButton {
                    text: qsTr("Quit the program")
                    checked: !AppController.closeToTray
                    enabled: AppController.trayEnabled
                    onToggled: {
                        AppController.closeToTray = !checked;
                        checked = Qt.binding(() => !AppController.closeToTray);
                    }
                }

                Label {
                    text: AppController.trayEnabled
                        ? qsTr("Quitting also removes the tray icon; the daemon and your controller are unaffected either way.")
                        : qsTr("Without the tray icon, closing the window always quits.")
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.bottomMargin: 8
                }

                Switch {
                    text: qsTr("Start the applet on login")
                    checked: AppController.autostartEnabled
                    // Autostart runs the applet with --background, which
                    // needs somewhere to sit.
                    enabled: AppController.trayEnabled
                    onToggled: {
                        AppController.autostartEnabled = checked;
                        checked = Qt.binding(() => AppController.autostartEnabled);
                    }
                }

                Label {
                    text: AppController.trayEnabled
                        ? qsTr("Adds an autostart entry for this user only. It starts the applet in the tray, without opening this window.")
                        : qsTr("Requires the tray icon: turning the tray icon off also removes the autostart entry.")
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                Label {
                    text: AppController.settingsError
                    visible: text.length > 0
                    color: "#c0392b"
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }
        }
    }
}
