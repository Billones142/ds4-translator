import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Ds4Translator

ApplicationWindow {
    id: window
    title: qsTr("Settings")
    width: 640
    height: 720
    minimumWidth: 480
    minimumHeight: 560
    // Started with --background: the applet is running, so the window waits
    // in the tray until the user asks for it.
    visible: !AppController.startHidden

    // With a tray, closing the window only puts it away -- the applet keeps
    // running. Without one, closing must really quit, or the process would
    // linger with no way to reach it.
    onClosing: (close) => {
        if (AppController.trayActive) {
            close.accepted = false;
            window.hide();
        }
    }

    Connections {
        target: AppController
        function onShowWindowRequested() {
            window.show();
            window.raise();
            window.requestActivate();
        }
    }

    // Every control is disabled while the daemon is unreachable: sending a
    // command that cannot arrive would only produce an error banner. It is
    // also disabled while a command the user triggered is still running, so
    // a second one cannot be queued on top of it. Background status polls do
    // not count as busy -- they used to, which made open combo box popups
    // close on their own every poll interval.
    readonly property bool editable: DaemonController.online && !DaemonController.busy

    // The two touchpad slots, as coordinates or a dash while unused. Written
    // out here rather than in the label so the empty case reads as one word.
    readonly property string touchSummary: {
        if (!InputMonitor.hasTouch) {
            return qsTr("not reported");
        }
        const first = InputMonitor.touch1Active
            ? "%1,%2".arg(InputMonitor.touch1X).arg(InputMonitor.touch1Y) : "—";
        const second = InputMonitor.touch2Active
            ? "%1,%2".arg(InputMonitor.touch2X).arg(InputMonitor.touch2Y) : "—";
        return "%1  %2".arg(first).arg(second);
    }

    // set-type/set-name/set-backend recreate the virtual device and routinely
    // take a second or more. Showing progress instantly would make every
    // quick command flash; this only appears once a command is slow enough
    // that the user would otherwise wonder whether anything happened.
    property bool showProgress: false
    readonly property int progressDelayMs: 700

    Timer {
        id: progressDelay
        interval: window.progressDelayMs
        onTriggered: window.showProgress = true
    }

    Connections {
        target: DaemonController
        function onBusyChanged() {
            if (DaemonController.busy) {
                progressDelay.restart();
            } else {
                progressDelay.stop();
                window.showProgress = false;
            }
        }
    }

    header: ColumnLayout {
        spacing: 0

        ToolBar {
            Layout.fillWidth: true
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12

                Label {
                    text: DaemonController.online ? qsTr("Daemon connected") : qsTr("Daemon unreachable")
                    font.bold: true
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: qsTr("Applying…")
                    visible: window.showProgress
                }
                BusyIndicator {
                    running: window.showProgress
                    visible: running
                    implicitWidth: 20
                    implicitHeight: 20
                }
            }
        }

        TabBar {
            id: tabBar
            Layout.fillWidth: true

            TabButton { text: qsTr("Settings") }
            TabButton { text: qsTr("Controller") }
        }
    }

    // The live stream makes the daemon poll its input source far harder, so
    // it is subscribed to only while its page is actually on screen (and the
    // window is not sitting hidden in the tray).
    Binding {
        target: InputMonitor
        property: "active"
        value: window.visible && tabBar.currentIndex === 1
    }

    footer: Pane {
        visible: DaemonController.lastMessage.length > 0
        Label {
            width: parent.width
            text: DaemonController.lastMessage
            wrapMode: Text.Wrap
            color: DaemonController.lastMessageIsError ? "#c0392b" : palette.text
        }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: tabBar.currentIndex

        ScrollView {
            contentWidth: availableWidth

            ColumnLayout {
                width: parent.width
                spacing: 16

                GroupBox {
                    title: qsTr("Status")
                    Layout.fillWidth: true
                    Layout.margins: 12

                    GridLayout {
                        anchors.fill: parent
                        columns: 2
                        columnSpacing: 12

                        Label { text: qsTr("Physical controller:") }
                        Label {
                            text: DaemonController.physicalController || qsTr("—")
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                        }

                        Label { text: qsTr("Connection:") }
                        Label { text: DaemonController.connectionType || qsTr("—") }

                        Label { text: qsTr("Active backend:") }
                        Label { text: DaemonController.activeBackend || qsTr("—") }
                    }
                }

                GroupBox {
                    title: qsTr("Virtual controller type")
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12

                    ColumnLayout {
                        anchors.fill: parent

                        Label {
                            text: qsTr("Which controller games see. Changing this recreates the virtual device.")
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }

                        OptionSelector {
                            id: typeSelector
                            Layout.fillWidth: true
                            enabled: window.editable
                            values: DaemonController.typeValues()
                            labelMap: ({
                                "ds4": qsTr("DualShock 4"),
                                "dualsense": qsTr("DualSense"),
                                "none": qsTr("None"),
                                "hidden": qsTr("Hidden")
                            })
                            current: DaemonController.emulationType
                            onSelected: value => DaemonController.setType(value)
                        }
                    }
                }

                GroupBox {
                    title: qsTr("Emulation backend")
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8

                        Label {
                            text: qsTr("Set per emulated type. Applies the next time that type's virtual device is created.")
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: qsTr("DualShock 4:"); Layout.preferredWidth: 110 }
                            OptionSelector {
                                Layout.fillWidth: true
                                enabled: window.editable
                                values: DaemonController.backendValues()
                                current: DaemonController.ds4Backend
                                onSelected: value => DaemonController.setBackend("ds4", value)
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: qsTr("DualSense:"); Layout.preferredWidth: 110 }
                            OptionSelector {
                                Layout.fillWidth: true
                                enabled: window.editable
                                values: DaemonController.backendValues()
                                current: DaemonController.dualsenseBackend
                                onSelected: value => DaemonController.setBackend("dualsense", value)
                            }
                        }
                    }
                }

                GroupBox {
                    title: qsTr("Reported controller name")
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8

                        Label {
                            text: qsTr("Applying a name recreates the virtual device if that type is active — any app reading it sees a brief input interruption.")
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }

                        NameEditor {
                            Layout.fillWidth: true
                            enabled: window.editable
                            controller: "ds4"
                            label: qsTr("DualShock 4")
                            currentName: DaemonController.ds4Name
                            isDefault: DaemonController.ds4NameIsDefault
                        }

                        NameEditor {
                            Layout.fillWidth: true
                            enabled: window.editable
                            controller: "dualsense"
                            label: qsTr("DualSense")
                            currentName: DaemonController.dualsenseName
                            isDefault: DaemonController.dualsenseNameIsDefault
                        }
                    }
                }

                GroupBox {
                    title: qsTr("Hide method")
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12

                    ColumnLayout {
                        anchors.fill: parent

                        Label {
                            text: qsTr("How the physical controller is hidden from other apps. Applies on the next physical (re)connection.")
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }

                        OptionSelector {
                            Layout.fillWidth: true
                            enabled: window.editable
                            values: DaemonController.hideMethodValues()
                            labelMap: ({ "unbind": qsTr("unbind (default)") })
                            current: DaemonController.hideMethod
                            onSelected: value => DaemonController.setHideMethod(value)
                        }
                    }
                }

                // Preferences of this application itself, not of the daemon: they
                // stay usable while the daemon is unreachable.
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

        // Live view: what the daemon currently sees on the controller.
        ScrollView {
            contentWidth: availableWidth

            ColumnLayout {
                width: parent.width
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    Layout.margins: 12

                    Label {
                        text: InputMonitor.hasState
                            ? (InputMonitor.source === "VIRTUAL"
                                ? qsTr("Showing the emulated controller")
                                : qsTr("Showing the physical controller"))
                            : InputMonitor.note
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }

                    BusyIndicator {
                        running: InputMonitor.active && !InputMonitor.hasState
                        visible: running
                        implicitWidth: 20
                        implicitHeight: 20
                    }
                }

                ControllerView {
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12
                    // The drawing has a fixed aspect ratio; height follows the
                    // width it is given so nothing is letterboxed.
                    Layout.preferredHeight: width * (implicitHeight / implicitWidth)
                    opacity: InputMonitor.hasState ? 1 : 0.45
                }

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12
                    horizontalAlignment: Text.AlignHCenter
                    text: InputMonitor.hasState
                        ? qsTr("D-pad: %1     L2: %2     R2: %3     Touch: %4")
                            .arg(InputMonitor.dpadName()).arg(InputMonitor.l2).arg(InputMonitor.r2)
                            .arg(window.touchSummary)
                        : ""
                    opacity: 0.75
                }

                GroupBox {
                    title: qsTr("Motion sensors")
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12
                    Layout.bottomMargin: 12

                    MotionView {
                        anchors.fill: parent
                    }
                }
            }
        }
    }
}
