// Inline components below bind to the file's root id (the accent colour);
// Bound makes that resolution explicit rather than dynamic.
pragma ComponentBehavior: Bound

import QtQuick
import Ds4Translator

// The controller drawing, with every control lit up while it is pressed.
//
// The artwork carries no usable element ids, so the overlays are placed by
// coordinate in the SVG's own 600x400 space: the whole group is laid out at
// that size and scaled as one, which keeps the hit shapes glued to the drawing
// at any window size.
//
// It is drawn from two files cut out of assets/Dualshock_4_Layout.svg (kept as
// the original): a base with the stick knobs removed, and the knob on its own,
// which the sticks below move.
Item {
    id: root

    // The SVG's viewBox. All coordinates below are in these units.
    readonly property real designWidth: 600
    readonly property real designHeight: 400

    readonly property color accent: "#2d8cf0"

    // Live values, or a neutral controller when nothing is streaming, so the
    // drawing stays readable instead of showing a stale pose.
    readonly property bool live: InputMonitor.hasState
    readonly property int buttons: live ? InputMonitor.buttons : 0
    readonly property int dpad: live ? InputMonitor.dpad : 8

    implicitWidth: designWidth
    implicitHeight: designHeight

    // A lit control. Circular ones set radius to half their width.
    component Spot: Rectangle {
        id: spot

        property bool pressed: false
        color: root.accent
        opacity: spot.pressed ? 0.55 : 0
        border.width: 2
        border.color: root.accent
        visible: spot.opacity > 0
        Behavior on opacity { NumberAnimation { duration: 70 } }
    }

    component TriggerBar: Item {
        id: trigger

        property int value: 0     // 0..255, as reported
        property string label: ""
        width: 66
        height: 14

        Rectangle {
            anchors.fill: parent
            radius: 4
            color: "transparent"
            border.width: 1
            border.color: "#80000000"
        }
        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 2
            width: Math.max(0, (trigger.width - 4) * Math.min(1, trigger.value / 255))
            radius: 3
            color: root.accent
            opacity: 0.6
        }
        Text {
            anchors.centerIn: parent
            text: trigger.label
            font.pixelSize: 9
            color: "#404040"
        }
    }

    // One finger on the touchpad, placed by mapping the pad's own coordinate
    // space onto the drawn pad area.
    component Contact: Rectangle {
        id: contact

        property bool active: false
        property int padX: 0
        property int padY: 0
        // The touchpad area of the drawing, in artwork coordinates.
        readonly property real areaX: 211
        readonly property real areaY: 44
        readonly property real areaWidth: 181
        readonly property real areaHeight: 99

        width: 18
        height: 18
        radius: 9
        color: root.accent
        border.width: 2
        border.color: "#ffffff"
        opacity: contact.active ? 0.9 : 0
        visible: contact.opacity > 0
        x: contact.areaX - width / 2
           + contact.areaWidth * (contact.padX / InputMonitor.touchWidth)
        y: contact.areaY - height / 2
           + contact.areaHeight * (contact.padY / InputMonitor.touchHeight)
        Behavior on opacity { NumberAnimation { duration: 70 } }
    }

    component Stick: Item {
        id: stick

        // Where the well's centre sits in the artwork, so the knob rests
        // exactly where it was cut from.
        property real centreX: 0
        property real centreY: 0
        property int axisX: 128
        property int axisY: 128
        property bool clicked: false
        // How far the knob travels from centre at full deflection: what is
        // left of the well once the knob is in it, so the drawing stays inside
        // its own ring.
        readonly property real travel: 14

        // The knob's own size in the layout's units.
        width: 68
        height: 68
        x: stick.centreX - width / 2 + (stick.axisX - 128) / 127 * stick.travel
        y: stick.centreY - height / 2 + (stick.axisY - 128) / 127 * stick.travel

        // The knob itself, taken out of the layout drawing and moved as one
        // piece -- the base artwork below has the two knobs cut out of it, so
        // there is nothing left behind for this to slide away from.
        Image {
            anchors.fill: parent
            source: "qrc:/art/Dualshock_4_Stick.svg"
            sourceSize.width: stick.width * 4
            sourceSize.height: stick.height * 4
            fillMode: Image.PreserveAspectFit
        }

        // Lit ring for L3/R3: the click is a separate button from the
        // deflection, so it needs its own mark. It rides with the knob, which
        // is the thing that was clicked.
        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            radius: width / 2
            color: root.accent
            opacity: stick.clicked ? 0.55 : 0
            Behavior on opacity { NumberAnimation { duration: 70 } }
        }
    }

    Item {
        id: art
        width: root.designWidth
        height: root.designHeight
        anchors.centerIn: parent
        scale: Math.min(root.width / root.designWidth, root.height / root.designHeight)

        Image {
            anchors.fill: parent
            source: "qrc:/art/Dualshock_4_Layout_base.svg"
            // Rasterised above the on-screen size so the drawing stays sharp
            // when the window is enlarged.
            sourceSize.width: root.designWidth * 2
            sourceSize.height: root.designHeight * 2
            fillMode: Image.PreserveAspectFit
        }

        // ----------------------------------------------------- face buttons

        Spot {
            x: 445; y: 65; width: 44; height: 44; radius: 22
            pressed: (root.buttons & InputMonitor.Triangle) !== 0
        }
        Spot {
            x: 407; y: 103; width: 44; height: 44; radius: 22
            pressed: (root.buttons & InputMonitor.Square) !== 0
        }
        Spot {
            x: 484; y: 103; width: 44; height: 44; radius: 22
            pressed: (root.buttons & InputMonitor.Circle) !== 0
        }
        Spot {
            x: 445; y: 145; width: 44; height: 44; radius: 22
            pressed: (root.buttons & InputMonitor.Cross) !== 0
        }

        // ------------------------------------------------------------ d-pad
        // The hat is a single value, not four bits: diagonals light both of
        // the arms they lie between.

        Spot {
            x: 108; y: 85; width: 29; height: 36; radius: 6
            pressed: root.dpad === 7 || root.dpad === 0 || root.dpad === 1
        }
        Spot {
            x: 125; y: 111; width: 36; height: 29; radius: 6
            pressed: root.dpad === 1 || root.dpad === 2 || root.dpad === 3
        }
        Spot {
            x: 108; y: 130; width: 29; height: 36; radius: 6
            pressed: root.dpad === 3 || root.dpad === 4 || root.dpad === 5
        }
        Spot {
            x: 84; y: 111; width: 36; height: 29; radius: 6
            pressed: root.dpad === 5 || root.dpad === 6 || root.dpad === 7
        }

        // -------------------------------------------- share/options/PS/pad

        Spot {
            x: 177; y: 56; width: 16; height: 35; radius: 8
            pressed: (root.buttons & InputMonitor.Share) !== 0
        }
        Spot {
            x: 400; y: 56; width: 16; height: 35; radius: 8
            pressed: (root.buttons & InputMonitor.Options) !== 0
        }
        Spot {
            x: 284; y: 185; width: 32; height: 32; radius: 16
            pressed: (root.buttons & InputMonitor.Ps) !== 0
        }
        Spot {
            x: 211; y: 44; width: 181; height: 99; radius: 10
            pressed: (root.buttons & InputMonitor.Touchpad) !== 0
        }

        Contact {
            active: root.live && InputMonitor.touch1Active
            padX: InputMonitor.touch1X
            padY: InputMonitor.touch1Y
        }
        Contact {
            active: root.live && InputMonitor.touch2Active
            padX: InputMonitor.touch2X
            padY: InputMonitor.touch2Y
        }

        // ------------------------------------------------ shoulders/triggers
        // L1/R1 sit on the shoulder tabs the top view actually shows. L2/R2
        // are behind the controller from here, so they get analog bars in the
        // empty margin directly above their side.

        Spot {
            x: 101; y: 30; width: 66; height: 14; radius: 6
            pressed: (root.buttons & InputMonitor.L1) !== 0
        }
        Spot {
            x: 433; y: 30; width: 66; height: 14; radius: 6
            pressed: (root.buttons & InputMonitor.R1) !== 0
        }

        TriggerBar {
            x: 101; y: 10
            label: "L2"
            value: root.live ? InputMonitor.l2 : 0
        }
        TriggerBar {
            x: 433; y: 10
            label: "R2"
            value: root.live ? InputMonitor.r2 : 0
        }

        // ----------------------------------------------------------- sticks

        Stick {
            // The well centres come from the layout file itself (the knob
            // circles' own coordinates), not from measuring the picture: the
            // drawing is not perfectly mirrored, so the two differ.
            centreX: 213.4; centreY: 197.9
            axisX: root.live ? InputMonitor.leftX : 128
            axisY: root.live ? InputMonitor.leftY : 128
            clicked: (root.buttons & InputMonitor.L3) !== 0
        }
        Stick {
            centreX: 388.3; centreY: 197.9
            axisX: root.live ? InputMonitor.rightX : 128
            axisY: root.live ? InputMonitor.rightY : 128
            clicked: (root.buttons & InputMonitor.R3) !== 0
        }
    }
}
