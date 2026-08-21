// The swatch Repeater's delegate reaches the file's root id; Bound makes that
// resolution explicit rather than dynamic.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Ds4Translator

// The physical controller's light bar and rumble motors: what they are doing
// right now, and a way to drive them by hand to check the hardware.
//
// Two modes, the switch decides which. Following: the sliders mirror what the
// emulated device is asking for, so a game moving the light bar moves them
// too, and nothing here can be dragged. Driving: what the sliders say goes
// straight to the controller -- until the game's next output report takes it
// back, which is why this is a test and not a setting.
ColumnLayout {
    id: root

    // The daemon writes to the physical controller, so there has to be one.
    readonly property bool available: DaemonController.online
        && DaemonController.physicalController.length > 0
    // Whether the emulated device is in charge (the switch).
    readonly property bool following: driveSwitch.checked
    readonly property bool editable: root.available && !root.following

    property int red: 0
    property int green: 0
    property int blue: 255
    property int rumbleLeft: 0
    property int rumbleRight: 0
    // A send is waiting for the rate limit below to come round again.
    property bool pendingSend: false

    spacing: 8

    // Copies what the daemon says the emulated device is asking for. Only
    // meaningful while following -- the values are the user's own otherwise.
    function syncFromDaemon() {
        if (!InputMonitor.hasOutput) {
            return;
        }
        root.red = InputMonitor.ledRed;
        root.green = InputMonitor.ledGreen;
        root.blue = InputMonitor.ledBlue;
        root.rumbleLeft = InputMonitor.rumbleLeft;
        root.rumbleRight = InputMonitor.rumbleRight;
        root.syncWheelFromLevels();
    }

    // The light bar is a lamp, not a painted surface: half power still looks
    // clearly coloured, and only all three channels at zero is black. Feeding
    // the raw 0-255 values into a screen colour gets none of that -- sRGB is
    // gamma encoded, so 128 draws at about a fifth of full brightness and every
    // mid value reads as near-black next to the real controller. Decoding the
    // level as light (the same 2.2 the display applies in reverse) puts the
    // swatch back where the eye expects it.
    function lit(level) {
        return Math.pow(level / 255, 1 / 2.2);
    }

    // Inverse of lit(): a colour picked on screen becomes the LED level that
    // shows up as that colour, so the wheel, the swatch and the hardware agree.
    function level(light) {
        return Math.round(Math.pow(Math.max(0, Math.min(1, light)), 2.2) * 255);
    }

    // The wheel's own state. It cannot be derived from the levels alone: black
    // and white have no hue, so dragging the brightness slider to either end
    // would lose the colour the user picked and never give it back. The levels
    // stay authoritative, and these follow them whenever they say something
    // about hue.
    property real hue: 0.66
    property real saturation: 1
    property real brightness: 1

    function syncWheelFromLevels() {
        const colour = Qt.rgba(root.lit(root.red), root.lit(root.green),
                               root.lit(root.blue), 1);
        // hsvHue is -1 for greys, where the hue genuinely has no value; keep
        // the one the user last chose.
        if (colour.hsvHue >= 0) {
            root.hue = colour.hsvHue;
        }
        if (colour.hsvValue > 0) {
            root.saturation = colour.hsvSaturation;
        }
        root.brightness = colour.hsvValue;
    }

    function applyWheel() {
        const colour = Qt.hsva(root.hue, root.saturation, root.brightness, 1);
        root.red = root.level(colour.r);
        root.green = root.level(colour.g);
        root.blue = root.level(colour.b);
        root.send();
    }

    function sendNow() {
        DaemonController.testLed(root.red, root.green, root.blue,
                                 root.rumbleLeft, root.rumbleRight);
    }

    // Sends immediately, then rate-limits: a drag is followed live rather
    // than only on release, without a command per pixel. The last position is
    // always sent, so where the finger stopped is where the light bar ends up.
    function send() {
        if (!root.editable) {
            return;
        }
        if (sendThrottle.running) {
            root.pendingSend = true;
            return;
        }
        root.sendNow();
        sendThrottle.restart();
    }

    Timer {
        id: sendThrottle
        interval: 40
        onTriggered: {
            if (root.pendingSend) {
                root.pendingSend = false;
                root.sendNow();
                sendThrottle.restart();
            }
        }
    }

    Connections {
        target: InputMonitor

        // The daemon sends this only when something actually changed, so this
        // stays quiet while a game leaves the light bar alone -- and stops
        // entirely when the test view is closed, since the stream is dropped
        // with it and nothing here runs in the background.
        function onOutputChanged() {
            if (root.following) {
                root.syncFromDaemon();
            }
        }

        // Leaving the view (or losing the daemon) while driving would strand
        // the controller on a test colour, so control goes back by itself.
        function onActiveChanged() {
            if (!InputMonitor.active && !root.following) {
                driveSwitch.checked = true;
                DaemonController.resetLed();
            }
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

        Switch {
            id: driveSwitch
            text: qsTr("Let the game drive")
            checked: true
            enabled: root.available
            onToggled: {
                if (checked) {
                    // Hand back: the daemon re-sends what the emulated device
                    // wants, and that state comes back on the stream as the
                    // values the sliders then show.
                    DaemonController.resetLed();
                    root.syncFromDaemon();
                } else {
                    // Take over from exactly what is on the controller now, so
                    // nothing jumps at the moment control changes hands.
                    root.syncFromDaemon();
                    root.sendNow();
                }
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: !root.available
                ? qsTr("Needs a physical controller connected to the daemon.")
                : !InputMonitor.hasOutput
                    ? qsTr("This daemon does not report the light bar state, so the values below are only what is sent from here. Restart ds4-translator after updating it.")
                    : root.following
                        ? qsTr("Showing what the emulated device is asking for. Switch this off to drive the light bar and motors by hand.")
                        : qsTr("Driving the controller directly. The game's next output report takes it back.")
            opacity: 0.75
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
        Item {
            Layout.preferredWidth: 48
            Layout.preferredHeight: 24

            // The halo stands in for the light the bar throws on its
            // surroundings, which is most of what makes the real one look
            // bright. It fades out with the swatch, so "Off" is still plainly
            // off.
            Rectangle {
                anchors.centerIn: parent
                width: parent.width + 10
                height: parent.height + 10
                radius: 9
                color: swatch.color
                opacity: 0.35 * Math.max(root.lit(root.red),
                                         Math.max(root.lit(root.green),
                                                  root.lit(root.blue)))
            }

            Rectangle {
                id: swatch
                anchors.fill: parent
                radius: 4
                color: Qt.rgba(root.lit(root.red), root.lit(root.green),
                               root.lit(root.blue), 1)
                border.width: 1
                border.color: "#40808080"
            }
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
                    enabled: root.editable
                    onClicked: {
                        root.red = modelData.r;
                        root.green = modelData.g;
                        root.blue = modelData.b;
                        root.syncWheelFromLevels();
                        root.send();
                    }
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 12

        ColorWheel {
            id: wheel
            hue: root.hue
            saturation: root.saturation
            value: root.brightness
            enabled: root.editable
            opacity: root.editable ? 1 : 0.5
            Layout.preferredWidth: 140
            Layout.preferredHeight: 140
            onPicked: (hue, saturation) => {
                root.hue = hue;
                root.saturation = saturation;
                // Picking a colour on a light bar that is off should turn it
                // on, otherwise the wheel looks broken.
                if (root.brightness <= 0) {
                    root.brightness = 1;
                }
                root.applyWheel();
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 4

            Label {
                text: qsTr("Brightness")
                opacity: 0.75
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                // Black at one end, the picked colour at full at the other:
                // the one axis the disc does not carry.
                Slider {
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    enabled: root.editable
                    value: root.brightness
                    onMoved: {
                        root.brightness = value;
                        root.applyWheel();
                    }
                }

                Label {
                    text: qsTr("%1%").arg(Math.round(root.brightness * 100))
                    horizontalAlignment: Text.AlignRight
                    Layout.preferredWidth: 40
                }
            }
        }
    }

    LevelSlider {
        label: qsTr("Red")
        value: root.red
        enabled: root.editable
        onMoved: value => { root.red = value; root.syncWheelFromLevels(); root.send(); }
    }
    LevelSlider {
        label: qsTr("Green")
        value: root.green
        enabled: root.editable
        onMoved: value => { root.green = value; root.syncWheelFromLevels(); root.send(); }
    }
    LevelSlider {
        label: qsTr("Blue")
        value: root.blue
        enabled: root.editable
        onMoved: value => { root.blue = value; root.syncWheelFromLevels(); root.send(); }
    }

    LevelSlider {
        label: qsTr("Rumble heavy")
        value: root.rumbleLeft
        enabled: root.editable
        onMoved: value => { root.rumbleLeft = value; root.send(); }
    }
    LevelSlider {
        label: qsTr("Rumble light")
        value: root.rumbleRight
        enabled: root.editable
        onMoved: value => { root.rumbleRight = value; root.send(); }
    }

    Button {
        text: qsTr("Stop rumble")
        enabled: root.editable && (root.rumbleLeft > 0 || root.rumbleRight > 0)
        onClicked: {
            root.rumbleLeft = 0;
            root.rumbleRight = 0;
            root.send();
        }
    }
}
