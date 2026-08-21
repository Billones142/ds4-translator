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

    // What closing does is the user's choice (Settings tab), but only when
    // there is a tray icon to go back to: without one, closing must really
    // quit or the process would linger with no way to reach it.
    onClosing: (close) => {
        if (AppController.hideOnClose) {
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

            TabButton { text: qsTr("Emulation") }
            TabButton { text: qsTr("Test") }
            TabButton { text: qsTr("Settings") }
        }
    }

    // Index of the test page in the tab bar / stack above.
    readonly property int testTabIndex: 1

    // The live stream makes the daemon poll its input source far harder, so
    // it is subscribed to only while its page is actually on screen (and the
    // window is not sitting hidden in the tray).
    Binding {
        target: InputMonitor
        property: "active"
        value: window.visible && tabBar.currentIndex === window.testTabIndex
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

        EmulationPage {}

        TestPage {}

        AppSettingsPage {}
    }
}
