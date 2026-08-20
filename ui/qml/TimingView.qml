import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Ds4Translator

// How fast the physical controller reports, and what the daemon adds on top.
//
// Both numbers come from the daemon, which is the only place they can be
// measured: it sees each physical report as it arrives and knows when the
// emulated one was written. The rate is a property of the controller and its
// connection (USB and Bluetooth differ), not something the daemon chooses.
ColumnLayout {
    id: root

    // Rounded to whole reports: the sample is a few seconds long, so decimals
    // would suggest a precision the measurement does not have.
    readonly property string rateText:
        InputMonitor.reportRate > 0
            ? qsTr("%1 reports/s (about %2 ms apart)")
                .arg(Math.round(InputMonitor.reportRate))
                .arg(InputMonitor.intervalMeanMs.toFixed(2))
            : qsTr("No reports arrived — is the controller awake?")

    spacing: 8

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Button {
            text: InputMonitor.measuring ? qsTr("Measuring…") : qsTr("Measure")
            enabled: InputMonitor.timingSupported && !InputMonitor.measuring
            onClicked: InputMonitor.measure()
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: !InputMonitor.timingSupported
                ? qsTr("This daemon does not measure report timing. Restart ds4-translator after updating it.")
                : InputMonitor.measuring
                    ? qsTr("Sampling for a few seconds. Leave the controller connected.")
                    : InputMonitor.measured
                        ? root.rateText
                        : qsTr("Measures the physical controller's report rate and the delay the daemon adds.")
            opacity: InputMonitor.measured && !InputMonitor.measuring ? 1 : 0.75
        }
    }

    GridLayout {
        visible: InputMonitor.measured && !InputMonitor.measuring
        Layout.fillWidth: true
        columns: 2
        columnSpacing: 12
        rowSpacing: 4

        Label { text: qsTr("Report rate:") }
        Label { text: qsTr("%1 Hz").arg(Math.round(InputMonitor.reportRate)) }

        Label { text: qsTr("Interval:") }
        Label {
            text: qsTr("%1 ms mean, %2 min, %3 max")
                .arg(InputMonitor.intervalMeanMs.toFixed(2))
                .arg(InputMonitor.intervalMinMs.toFixed(2))
                .arg(InputMonitor.intervalMaxMs.toFixed(2))
        }

        Label { text: qsTr("Translation delay:") }
        Label {
            text: qsTr("%1 ms mean, %2 ms worst")
                .arg(InputMonitor.latencyMeanMs.toFixed(3))
                .arg(InputMonitor.latencyMaxMs.toFixed(3))
        }
    }

    Label {
        visible: InputMonitor.measured && !InputMonitor.measuring
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: qsTr("Translation delay is the daemon's own share: from having read a report off the physical controller to having written the emulated one. It does not include what the game does with it afterwards, or the controller's own radio latency.")
        opacity: 0.75
    }
}
