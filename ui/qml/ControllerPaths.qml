import QtQuick

// The outline of every control, copied out of assets/Dualshock_4_Layout.svg
// with the group transforms folded in, so these are ready to draw in the
// artwork's own 600x400 space.
//
// Lighting a control means filling its own outline rather than a rectangle
// placed near it, which is why the drawing and the highlight cannot drift
// apart. Coordinates are rounded to a hundredth of a unit; the drawing is
// 600 units wide, so that is far below a pixel.
QtObject {
    readonly property string triangle: "m 489.21,85.07 a 17.08,17.08 0 0,1 -34.16,0 a 17.08,17.08 0 1,1 34.16,0 z"
    readonly property string circle: "m 530.15,124.76 a 17.08,17.08 0 0,1 -34.16,0 a 17.08,17.08 0 1,1 34.16,0 z"
    readonly property string square: "m 449.06,124.76 a 17.08,17.08 0 0,1 -34.16,0 a 17.08,17.08 0 1,1 34.16,0 z"
    readonly property string cross: "m 489.21,164.89 a 17.08,17.08 0 0,1 -34.16,0 a 17.08,17.08 0 1,1 34.16,0 z"
    readonly property string dpadUp: "m 128.36,116.72 c -0.77,0 -1.6,-0.16 -2.64,-0.85 l -9.39,-9.05 c -0.75,-0.71 -1.27,-1.49 -1.62,-2.52 l -1.4,-17.01 c 0.93,-3.49 3.13,-5.82 6.72,-6.47 c 6.26,-0.49 11.07,-0.42 16.65,0 c 3.59,0.65 5.81,2.99 6.74,6.47 l -1.4,17.01 c -0.35,1.03 -0.87,1.81 -1.62,2.52 l -9.39,9.05 c -1.04,0.69 -1.89,0.85 -2.65,0.85 z"
    readonly property string dpadRight: "m 136.57,124.86 c 0,0.77 0.16,1.6 0.85,2.64 l 9.05,9.39 c 0.71,0.75 1.49,1.27 2.52,1.62 l 17.01,1.4 c 3.49,-0.93 5.82,-3.13 6.47,-6.72 c 0.49,-6.26 0.42,-11.07 0,-16.65 c -0.65,-3.59 -2.99,-5.81 -6.47,-6.74 l -17.01,1.4 c -1.03,0.35 -1.81,0.87 -2.52,1.62 l -9.05,9.39 c -0.69,1.04 -0.85,1.89 -0.85,2.65 z"
    readonly property string dpadDown: "m 128.36,133.08 c -0.77,0 -1.6,0.16 -2.64,0.85 l -9.39,9.05 c -0.75,0.71 -1.27,1.49 -1.62,2.52 l -1.4,17.01 c 0.93,3.49 3.13,5.82 6.72,6.47 c 6.26,0.49 11.07,0.42 16.65,0 c 3.59,-0.65 5.81,-2.99 6.74,-6.47 l -1.4,-17.01 c -0.35,-1.03 -0.87,-1.81 -1.62,-2.52 l -9.39,-9.05 c -1.04,-0.69 -1.89,-0.85 -2.65,-0.85 z"
    readonly property string dpadLeft: "m 119.92,124.84 c 0,-0.77 -0.16,-1.6 -0.85,-2.64 l -9.05,-9.39 c -0.71,-0.75 -1.49,-1.27 -2.52,-1.62 l -17.01,-1.4 c -3.49,0.93 -5.82,3.13 -6.47,6.72 c -0.49,6.26 -0.42,11.07 0,16.65 c 0.65,3.59 2.99,5.81 6.47,6.74 l 17.01,-1.4 c 1.03,-0.35 1.81,-0.87 2.52,-1.62 l 9.05,-9.39 c 0.69,-1.04 0.85,-1.89 0.85,-2.65 z"
    readonly property string share: "m 188.83,56.58 c -4.82,0 -8.73,3.91 -8.73,8.73 l 0,13.31 c 0,4.82 3.91,8.73 8.73,8.73 c 4.82,0 8.73,-3.91 8.73,-8.73 l 0,-13.31 c 0,-4.82 -3.91,-8.73 -8.73,-8.73 z"
    readonly property string options: "m 412.8,56.58 c -4.82,0 -8.73,3.91 -8.73,8.73 l 0,13.31 c 0,4.82 3.91,8.73 8.73,8.73 c 4.82,0 8.73,-3.91 8.73,-8.73 l 0,-13.31 c 0,-4.82 -3.91,-8.73 -8.73,-8.73 z"
    readonly property string ps: "m 313.75,203.37 a 13.75,13.75 0 1,1 -27.51,0 a 13.75,13.75 0 1,1 27.51,0 z"
    readonly property string l1: "M 98.96,39.66 C 119.06,28.57 142.7,26.47 166.53,35.98 l 0,15.43 l -67.57,0 z"
    readonly property string r1: "M 501.04,39.66 C 480.94,28.57 457.3,26.47 433.47,35.98 l 0,15.43 l 67.57,0 z"
    readonly property string touchpad: "m 217.5,45.22 c -1.97,0 -7.44,0.45 -7.44,5.29 l 0,16.28 l 0,66.52 c 0,6.16 7.08,11.6 13.24,11.6 l 157.06,0 c 6.16,0 11.55,-5.3 11.55,-11.46 l 0,-66.67 l 0,-16.28 c 0,-4.85 -5.47,-5.29 -7.44,-5.29 l -17.2,0 l -132.57,0 z"
}
