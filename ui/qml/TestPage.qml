import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Ds4Translator

// Live view: what the daemon currently sees on the controller, plus the
// hardware tests that go the other way (light bar, rumble, identify).
ScrollView {
    id: root

    // Names of the two test sources. The emulated one is named after the type
    // it is currently emulating, the physical one after the device the daemon
    // reports owning, so "the same controller twice" cannot be confused.
    readonly property string emulatedSourceLabel: {
        const type = DaemonController.emulationType;
        const label = type === "ds4" ? qsTr("Emulated DualShock 4")
            : type === "dualsense" ? qsTr("Emulated DualSense")
            : qsTr("Emulated controller (off)");
        return InputMonitor.virtualLive ? label : qsTr("%1 — idle").arg(label);
    }
    readonly property string physicalSourceLabel: {
        const device = DaemonController.physicalController;
        const connection = DaemonController.connectionType;
        // The connection is half of what identifies the pad here: the same
        // controller reports at a different rate over USB than over
        // Bluetooth, which is exactly what the timing panel measures.
        const named = device.length > 0
            ? (connection.length > 0 && connection !== "N/A"
                ? qsTr("Physical: %1 (%2)").arg(device).arg(connection)
                : qsTr("Physical: %1").arg(device))
            : qsTr("Physical controller");
        return InputMonitor.physicalLive ? named : qsTr("%1 — idle").arg(named);
    }

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

    contentWidth: availableWidth

    ColumnLayout {
        width: parent.width
        spacing: 12

        // Which of the daemon's two streams to watch. Both are
        // followed at once, so switching shows the other one
        // immediately instead of reconnecting.
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 12
            spacing: 8

            Label { text: qsTr("Watching:") }

            // A plain ComboBox, not the OptionSelector used in the
            // settings tab: this changes nothing on the daemon, so
            // there is nothing to stage behind an Apply button.
            ComboBox {
                Layout.fillWidth: true
                model: [root.emulatedSourceLabel, root.physicalSourceLabel]
                currentIndex: InputMonitor.source === InputMonitor.Virtual ? 0 : 1
                onActivated: index => {
                    InputMonitor.source = index === 0
                        ? InputMonitor.Virtual
                        : InputMonitor.Physical;
                }
            }

            // Which of several controllers is this one? The daemon
            // blinks the light bar of the pad it is actually holding.
            Button {
                text: qsTr("Identify")
                enabled: DaemonController.online
                    && DaemonController.physicalController.length > 0
                onClicked: DaemonController.identify()
            }

            BusyIndicator {
                running: InputMonitor.active && !InputMonitor.hasState
                visible: running
                implicitWidth: 20
                implicitHeight: 20
            }
        }

        // Straight under the picker: the light bar and motors belong
        // to the physical controller named in it.
        GroupBox {
            title: qsTr("Light bar and rumble test")
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12

            LedTester {
                anchors.fill: parent
            }
        }

        // Only shown when the selected source has nothing to report:
        // with data on screen the drawing says it better than words.
        Label {
            visible: !InputMonitor.hasState
            text: InputMonitor.note
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
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
                    .arg(root.touchSummary)
                : ""
            opacity: 0.75
        }

        GroupBox {
            title: qsTr("Motion sensors")
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12

            MotionView {
                anchors.fill: parent
            }
        }

        GroupBox {
            title: qsTr("Report timing")
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.bottomMargin: 12

            TimingView {
                anchors.fill: parent
            }
        }
    }
}
