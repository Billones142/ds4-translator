import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Ds4Translator

// The controller's motion sensors: three gyro axes (angular rate) and three
// accelerometer axes.
//
// Values are the raw signed counts the controller reports, because that is
// what the daemon forwards -- turning them into degrees/s and g would need
// the per-unit calibration block, which is not part of the input report. The
// bars are therefore relative: they show movement and direction, not units.
ColumnLayout {
    id: root

    readonly property bool live: InputMonitor.hasState
    // Full-scale of the bars. The sensors are 16-bit signed, but normal
    // handling stays well inside that, so a smaller span keeps the bars
    // useful instead of nearly always flat.
    readonly property int gyroSpan: 8192
    readonly property int accelSpan: 8192

    spacing: 8

    // One signed axis: a bar growing left or right from a centre tick.
    component AxisBar: RowLayout {
        id: axis

        property string label: ""
        property int value: 0
        property int span: 8192

        spacing: 8
        Layout.fillWidth: true

        Label {
            text: axis.label
            Layout.preferredWidth: 60
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 16
            radius: 3
            color: "transparent"
            border.width: 1
            border.color: "#40808080"

            // Centre tick: zero is in the middle, since these axes are signed.
            Rectangle {
                x: parent.width / 2
                width: 1
                height: parent.height
                color: "#40808080"
            }

            Rectangle {
                readonly property real fraction:
                    Math.max(-1, Math.min(1, axis.value / axis.span))
                height: parent.height - 4
                y: 2
                x: fraction >= 0 ? parent.width / 2 : parent.width / 2 + fraction * (parent.width / 2 - 2)
                width: Math.abs(fraction) * (parent.width / 2 - 2)
                radius: 2
                color: "#2d8cf0"
                opacity: 0.7
            }
        }

        Label {
            text: axis.value
            horizontalAlignment: Text.AlignRight
            Layout.preferredWidth: 60
        }
    }

    Label {
        text: qsTr("Angular rate (gyroscope)")
        font.bold: true
    }

    AxisBar {
        label: qsTr("Pitch")
        span: root.gyroSpan
        value: root.live ? InputMonitor.gyroPitch : 0
    }
    AxisBar {
        label: qsTr("Yaw")
        span: root.gyroSpan
        value: root.live ? InputMonitor.gyroYaw : 0
    }
    AxisBar {
        label: qsTr("Roll")
        span: root.gyroSpan
        value: root.live ? InputMonitor.gyroRoll : 0
    }

    Label {
        text: qsTr("Acceleration")
        font.bold: true
        Layout.topMargin: 8
    }

    AxisBar {
        label: qsTr("X")
        span: root.accelSpan
        value: root.live ? InputMonitor.accelX : 0
    }
    AxisBar {
        label: qsTr("Y")
        span: root.accelSpan
        value: root.live ? InputMonitor.accelY : 0
    }
    AxisBar {
        label: qsTr("Z")
        span: root.accelSpan
        value: root.live ? InputMonitor.accelZ : 0
    }

    RowLayout {
        Layout.topMargin: 8
        Layout.fillWidth: true
        spacing: 12

        // Where the controller is being held, derived from gravity: with the
        // pad at rest the accelerometer measures which way is down, so the
        // dot leaves the centre as it is tilted and the marker turns with it.
        Rectangle {
            id: tilt

            readonly property real range: 8192
            readonly property real tiltX:
                Math.max(-1, Math.min(1, (root.live ? InputMonitor.accelX : 0) / range))
            readonly property real tiltY:
                Math.max(-1, Math.min(1, (root.live ? InputMonitor.accelY : 0) / range))

            Layout.preferredWidth: 120
            Layout.preferredHeight: 120
            radius: width / 2
            color: "transparent"
            border.width: 1
            border.color: "#40808080"

            Rectangle {
                width: parent.width
                height: 1
                y: parent.height / 2
                color: "#20808080"
            }
            Rectangle {
                height: parent.height
                width: 1
                x: parent.width / 2
                color: "#20808080"
            }

            Rectangle {
                width: 14
                height: 14
                radius: 7
                color: "#2d8cf0"
                opacity: 0.8
                x: (parent.width - width) / 2 + tilt.tiltX * (parent.width / 2 - 10)
                y: (parent.height - height) / 2 + tilt.tiltY * (parent.height / 2 - 10)
                Behavior on x { NumberAnimation { duration: 60 } }
                Behavior on y { NumberAnimation { duration: 60 } }
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.live && !InputMonitor.hasMotion
                ? qsTr("This daemon does not report motion data. Restart ds4-translator after updating it to see the sensors here.")
                : qsTr("Tilt the controller and the dot follows gravity. A resting controller sits near the centre; a still, centred dot with flat gyro bars means this controller reports no motion data.")
            opacity: 0.75
        }
    }
}
