// The swatch Repeater's delegate reaches the file's root id; Bound makes that
// resolution explicit rather than dynamic.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Ds4Translator

// Drives the physical controller's light bar and rumble motors directly, to
// check the hardware works.
//
// Nothing here is a setting: the daemon forwards what the emulated device
// asks for as soon as a game sends its next output report, which takes the
// controller straight back. "Hand back" does it immediately.
ColumnLayout {
    id: root

    // The daemon writes to the physical controller, so there has to be one.
    readonly property bool available: DaemonController.online
        && DaemonController.physicalController.length > 0

    property int red: 0
    property int green: 0
    property int blue: 255
    property int rumbleLeft: 0
    property int rumbleRight: 0

    spacing: 8

    // Slider drags would otherwise send a command per pixel; the controller
    // only needs the value the finger stopped on, plus enough intermediate
    // ones to look live.
    Timer {
        id: sendThrottle
        interval: 60
        onTriggered: DaemonController.testLed(root.red, root.green, root.blue,
                                              root.rumbleLeft, root.rumbleRight)
    }

    function send() {
        if (root.available) {
            sendThrottle.restart();
        }
    }

    component LevelSlider: RowLayout {
        id: level

        property string label: ""
        property int value: 0

        signal moved(int value)

        spacing: 8
        Layout.fillWidth: true

        Label {
            text: level.label
            Layout.preferredWidth: 90
        }

        Slider {
            Layout.fillWidth: true
            from: 0
            to: 255
            stepSize: 1
            value: level.value
            onMoved: level.moved(Math.round(value))
        }

        Label {
            text: level.value
            horizontalAlignment: Text.AlignRight
            Layout.preferredWidth: 34
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Label {
            text: qsTr("Light bar")
            Layout.preferredWidth: 90
        }

        // What the sliders currently describe, so the swatch and the hardware
        // can be compared side by side.
        Rectangle {
            Layout.preferredWidth: 48
            Layout.preferredHeight: 24
            radius: 4
            color: Qt.rgba(root.red / 255, root.green / 255, root.blue / 255, 1)
            border.width: 1
            border.color: "#40808080"
        }

        // Fixed colours worth having one click away: the primaries prove each
        // LED channel separately, white proves all three, off proves the
        // controller obeys at all. A Flow so a narrow window wraps them onto
        // a second line instead of pushing the value readouts off the edge.
        Flow {
            Layout.fillWidth: true
            spacing: 6

            Repeater {
                model: [
                    { name: qsTr("Red"), r: 255, g: 0, b: 0 },
                    { name: qsTr("Green"), r: 0, g: 255, b: 0 },
                    { name: qsTr("Blue"), r: 0, g: 0, b: 255 },
                    { name: qsTr("White"), r: 255, g: 255, b: 255 },
                    { name: qsTr("Off"), r: 0, g: 0, b: 0 }
                ]
                delegate: Button {
                    required property var modelData
                    text: modelData.name
                    enabled: root.available
                    onClicked: {
                        root.red = modelData.r;
                        root.green = modelData.g;
                        root.blue = modelData.b;
                        root.send();
                    }
                }
            }
        }
    }

    LevelSlider {
        label: qsTr("Red")
        value: root.red
        enabled: root.available
        onMoved: value => { root.red = value; root.send(); }
    }
    LevelSlider {
        label: qsTr("Green")
        value: root.green
        enabled: root.available
        onMoved: value => { root.green = value; root.send(); }
    }
    LevelSlider {
        label: qsTr("Blue")
        value: root.blue
        enabled: root.available
        onMoved: value => { root.blue = value; root.send(); }
    }

    LevelSlider {
        label: qsTr("Rumble heavy")
        value: root.rumbleLeft
        enabled: root.available
        onMoved: value => { root.rumbleLeft = value; root.send(); }
    }
    LevelSlider {
        label: qsTr("Rumble light")
        value: root.rumbleRight
        enabled: root.available
        onMoved: value => { root.rumbleRight = value; root.send(); }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Button {
            text: qsTr("Stop rumble")
            enabled: root.available && (root.rumbleLeft > 0 || root.rumbleRight > 0)
            onClicked: {
                root.rumbleLeft = 0;
                root.rumbleRight = 0;
                root.send();
            }
        }

        Button {
            text: qsTr("Hand back")
            enabled: root.available
            onClicked: DaemonController.resetLed()
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.available
                ? qsTr("Sent straight to the physical controller. A game's own light bar and rumble take over again on its next output report.")
                : qsTr("Needs a physical controller connected to the daemon.")
            opacity: 0.75
        }
    }
}
