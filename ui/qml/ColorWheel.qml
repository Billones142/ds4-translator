import QtQuick

// Hue and saturation as a disc: angle picks the hue, distance from the centre
// picks how much of it, so a colour is one gesture instead of three sliders.
//
// Brightness is deliberately not on the disc -- it is the caller's slider --
// but the disc dims with it, so a wheel that would produce black looks black.
Item {
    id: root

    property real hue: 0
    property real saturation: 0
    // Only used to dim the drawing; picking never changes it.
    property real value: 1

    // The user moved the marker. Emitted while dragging, not only on release.
    signal picked(real hue, real saturation)

    implicitWidth: 140
    implicitHeight: 140

    readonly property real radius: Math.min(width, height) / 2

    Canvas {
        id: disc
        anchors.fill: parent
        // The disc never changes, so it is painted once and then only dimmed
        // by the overlay below.
        onPaint: {
            const ctx = getContext("2d");
            const cx = width / 2;
            const cy = height / 2;
            const r = Math.min(cx, cy);
            ctx.reset();
            // One wedge per degree, each a white-to-hue gradient outwards.
            // Overlapping the wedges by half a degree hides the seams that
            // rounding to whole pixels would otherwise leave between them.
            for (let deg = 0; deg < 360; ++deg) {
                const from = (deg - 0.5) * Math.PI / 180;
                const to = (deg + 1) * Math.PI / 180;
                const gradient = ctx.createRadialGradient(cx, cy, 0, cx, cy, r);
                gradient.addColorStop(0, Qt.hsva(deg / 360, 0, 1, 1));
                gradient.addColorStop(1, Qt.hsva(deg / 360, 1, 1, 1));
                ctx.beginPath();
                ctx.moveTo(cx, cy);
                ctx.arc(cx, cy, r, from, to, false);
                ctx.closePath();
                ctx.fillStyle = gradient;
                ctx.fill();
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: "black"
        opacity: 1 - root.value
    }

    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: "transparent"
        border.width: 1
        border.color: "#40808080"
    }

    // Where the current colour sits. Outlined in both directions so it stays
    // visible over pale and dark parts of the disc alike.
    Rectangle {
        width: 12
        height: 12
        radius: 6
        color: "transparent"
        border.width: 2
        border.color: "white"
        x: root.width / 2 + Math.cos(root.hue * 2 * Math.PI) * root.saturation * root.radius - width / 2
        y: root.height / 2 + Math.sin(root.hue * 2 * Math.PI) * root.saturation * root.radius - height / 2

        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            radius: width / 2
            color: "transparent"
            border.width: 1
            border.color: "black"
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.enabled

        function pick(mouseX, mouseY) {
            const dx = mouseX - root.width / 2;
            const dy = mouseY - root.height / 2;
            // Dragging past the rim keeps the hue and pins saturation at full,
            // which is friendlier than losing the colour off the edge.
            const distance = Math.min(Math.sqrt(dx * dx + dy * dy), root.radius);
            let angle = Math.atan2(dy, dx) / (2 * Math.PI);
            if (angle < 0) {
                angle += 1;
            }
            root.picked(angle, root.radius > 0 ? distance / root.radius : 0);
        }

        onPressed: mouse => pick(mouse.x, mouse.y)
        onPositionChanged: mouse => pick(mouse.x, mouse.y)
    }
}
