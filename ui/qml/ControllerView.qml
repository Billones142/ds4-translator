// Inline components below bind to the file's root id (the accent colour);
// Bound makes that resolution explicit rather than dynamic.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import Ds4Translator

// The controller drawing, with every control lit up while it is pressed.
//
// A pressed control is lit by filling its own outline, taken from the artwork
// (see ControllerPaths.qml), so the highlight is the shape of the button
// rather than a rectangle sitting near it. Everything is laid out in the SVG's
// own 600x400 space and scaled as one group, which keeps the two glued
// together at any window size.
//
// The drawing itself is the layout SVG with the stick knobs removed; the
// sticks are drawn on top so they can lean.
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

    // A control lit while it is pressed, filled along the outline the artwork
    // draws it with.
    component Hit: ShapePath {
        id: hit

        required property string outline
        property bool pressed: false
        property real alpha: hit.pressed ? 0.55 : 0

        fillColor: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, hit.alpha)
        strokeColor: "transparent"

        Behavior on alpha { NumberAnimation { duration: 70 } }

        PathSvg { path: hit.outline }
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
        // The dotted part of the drawn pad, which is the part that actually
        // senses -- the rounded surround is the frame around it. Taken from
        // the bounding box of the artwork's dot grid.
        readonly property real areaX: 215.4
        readonly property real areaY: 61.2
        readonly property real areaWidth: 170.8
        readonly property real areaHeight: 77.9

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

    // One ring of the stick's cap, foreshortened across the direction the
    // stick leans. Only the radius across the lean shrinks; the outline does
    // not, which is what a rounded rim looks like from above.
    component Knob: ShapePath {
        id: knob

        required property real ringRadius
        // How much the ring is foreshortened across the lean: 1 upright, less
        // the further the stick is pushed.
        required property real squash
        property color tint: "#cccccc"
        // The artwork draws the two inner rings half transparent; the L3/R3
        // highlight fades in and out on the same property.
        property real alpha: 1

        fillColor: Qt.rgba(knob.tint.r, knob.tint.g, knob.tint.b, knob.alpha)
        strokeColor: Qt.rgba(0, 0, 0, knob.alpha)
        strokeWidth: 1.2
        capStyle: ShapePath.RoundCap

        PathAngleArc {
            centerX: 34
            centerY: 34
            radiusY: knob.ringRadius
            radiusX: knob.ringRadius * knob.squash
            startAngle: 0
            sweepAngle: 360
        }
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

        // The view is straight down on a stick that pivots at its base, so a
        // pushed stick is a tilted disc: it slides away from the well's centre
        // and narrows along the direction it was pushed, while staying full
        // width across it. Both come from one angle -- the throw of the real
        // stick, which is shallow, so the cap stays close to round and it is
        // mostly the offset that shows.
        readonly property real maxTiltDeg: 21
        // How far the knob travels at full deflection: what is left of the
        // well once the knob is in it, so the drawing stays inside its ring.
        readonly property real travel: 14
        // The height of the cap above the pivot that puts the knob exactly
        // that far out when tilted all the way.
        readonly property real armLength:
            stick.travel / Math.sin(stick.maxTiltDeg * Math.PI / 180)

        // -1..1 per axis. The neutral value is 128, so the two halves of the
        // range are one step apart; each is scaled by its own half.
        readonly property real deflectX:
            stick.axisX >= 128 ? (stick.axisX - 128) / 127 : (stick.axisX - 128) / 128
        readonly property real deflectY:
            stick.axisY >= 128 ? (stick.axisY - 128) / 127 : (stick.axisY - 128) / 128
        // The gate is round, so a diagonal is not further out than a straight
        // push even though both axes read full.
        readonly property real deflection:
            Math.min(1, Math.hypot(stick.deflectX, stick.deflectY))
        readonly property real tiltRad:
            stick.maxTiltDeg * Math.PI / 180 * stick.deflection
        // Which way it was pushed, in degrees, for the squash below.
        readonly property real tiltDirection:
            Math.atan2(stick.deflectY, stick.deflectX) * 180 / Math.PI
        readonly property real offset: stick.armLength * Math.sin(stick.tiltRad)
        // Seen from above, the cap is that much narrower across the lean.
        readonly property real squash: Math.cos(stick.tiltRad)

        // The knob's own size in the layout's units.
        width: 68
        height: 68
        x: stick.centreX - width / 2
           + (stick.deflection > 0 ? stick.offset * stick.deflectX / stick.deflection : 0)
        y: stick.centreY - height / 2
           + (stick.deflection > 0 ? stick.offset * stick.deflectY / stick.deflection : 0)

        // The knob leans as one piece, so the whole item turns with the
        // push and the drawing below only has to foreshorten across it.
        rotation: stick.tiltDirection

        // The knob is drawn rather than lifted out of the SVG on purpose: a
        // squashed image squashes its outlines too, and the rim of a real
        // stick is rounded, so its edge keeps the same thickness however far
        // it leans. Drawing it means the radii foreshorten while the strokes
        // stay put. The circles are the ones cut out of the layout drawing --
        // radii, fill and the two half-transparent inner rings all come from
        // its path4031* paths.
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer

            Knob { ringRadius: 31.9; squash: stick.squash }
            Knob { ringRadius: 26.0; squash: stick.squash; alpha: 0.5 }
            Knob { ringRadius: 19.5; squash: stick.squash; alpha: 0.5 }

            // Lit ring for L3/R3: the click is a separate button from the
            // deflection, so it needs its own mark. Drawn with the cap so it
            // leans with it -- it marks the thing that was clicked.
            Knob {
                ringRadius: 30
                squash: stick.squash
                tint: root.accent
                alpha: stick.clicked ? 0.55 : 0
                Behavior on alpha { NumberAnimation { duration: 70 } }
            }
        }

    }

    ControllerPaths { id: paths }

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

        // Every button lights up as its own outline. One Shape draws them
        // all: they never overlap, and the renderer only has to walk one list.
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer

            Hit {
                outline: paths.triangle
                pressed: (root.buttons & InputMonitor.Triangle) !== 0
            }
            Hit {
                outline: paths.square
                pressed: (root.buttons & InputMonitor.Square) !== 0
            }
            Hit {
                outline: paths.circle
                pressed: (root.buttons & InputMonitor.Circle) !== 0
            }
            Hit {
                outline: paths.cross
                pressed: (root.buttons & InputMonitor.Cross) !== 0
            }

            // The hat is a single value, not four bits: diagonals light both
            // of the arms they lie between.
            Hit {
                outline: paths.dpadUp
                pressed: root.dpad === 7 || root.dpad === 0 || root.dpad === 1
            }
            Hit {
                outline: paths.dpadRight
                pressed: root.dpad === 1 || root.dpad === 2 || root.dpad === 3
            }
            Hit {
                outline: paths.dpadDown
                pressed: root.dpad === 3 || root.dpad === 4 || root.dpad === 5
            }
            Hit {
                outline: paths.dpadLeft
                pressed: root.dpad === 5 || root.dpad === 6 || root.dpad === 7
            }

            Hit {
                outline: paths.share
                pressed: (root.buttons & InputMonitor.Share) !== 0
            }
            Hit {
                outline: paths.options
                pressed: (root.buttons & InputMonitor.Options) !== 0
            }
            Hit {
                outline: paths.ps
                pressed: (root.buttons & InputMonitor.Ps) !== 0
            }
            Hit {
                outline: paths.touchpad
                pressed: (root.buttons & InputMonitor.Touchpad) !== 0
            }
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
        // L1/R1 light up on the shoulder tabs the drawing gives them, like
        // every other button.

        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer

            Hit {
                outline: paths.l1
                pressed: (root.buttons & InputMonitor.L1) !== 0
            }
            Hit {
                outline: paths.r1
                pressed: (root.buttons & InputMonitor.R1) !== 0
            }
        }

        // L2/R2 are behind the controller from here, so they get analog bars
        // in the margin above their side.
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
