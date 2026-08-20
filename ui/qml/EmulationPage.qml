import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Ds4Translator

// Everything that belongs to the daemon: what it is emulating, how, and under
// what name. The application's own preferences live in AppSettingsPage.qml --
// those stay usable while the daemon is unreachable, these do not.
ScrollView {
    id: root

    // Every control is disabled while the daemon is unreachable: sending a
    // command that cannot arrive would only produce an error banner. It is
    // also disabled while a command the user triggered is still running, so
    // a second one cannot be queued on top of it. Background status polls do
    // not count as busy -- they used to, which made open combo box popups
    // close on their own every poll interval.
    readonly property bool editable: DaemonController.online && !DaemonController.busy

    contentWidth: availableWidth

    ColumnLayout {
        width: parent.width
        spacing: 16

        GroupBox {
            title: qsTr("Status")
            Layout.fillWidth: true
            Layout.margins: 12

            GridLayout {
                anchors.fill: parent
                columns: 2
                columnSpacing: 12

                Label { text: qsTr("Physical controller:") }
                Label {
                    text: DaemonController.physicalController || qsTr("—")
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                }

                Label { text: qsTr("Connection:") }
                Label { text: DaemonController.connectionType || qsTr("—") }

                Label { text: qsTr("Active backend:") }
                Label { text: DaemonController.activeBackend || qsTr("—") }
            }
        }

        GroupBox {
            title: qsTr("Virtual controller type")
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12

            ColumnLayout {
                anchors.fill: parent

                Label {
                    text: qsTr("Which controller games see. Changing this recreates the virtual device.")
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                OptionSelector {
                    id: typeSelector
                    Layout.fillWidth: true
                    enabled: root.editable
                    values: DaemonController.typeValues()
                    labelMap: ({
                        "ds4": qsTr("DualShock 4"),
                        "dualsense": qsTr("DualSense"),
                        "none": qsTr("None"),
                        "hidden": qsTr("Hidden")
                    })
                    current: DaemonController.emulationType
                    onSelected: value => DaemonController.setType(value)
                }
            }
        }

        GroupBox {
            title: qsTr("Emulation backend")
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                Label {
                    text: qsTr("Set per emulated type. Applies the next time that type's virtual device is created.")
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("DualShock 4:"); Layout.preferredWidth: 110 }
                    OptionSelector {
                        Layout.fillWidth: true
                        enabled: root.editable
                        values: DaemonController.backendValues()
                        current: DaemonController.ds4Backend
                        onSelected: value => DaemonController.setBackend("ds4", value)
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("DualSense:"); Layout.preferredWidth: 110 }
                    OptionSelector {
                        Layout.fillWidth: true
                        enabled: root.editable
                        values: DaemonController.backendValues()
                        current: DaemonController.dualsenseBackend
                        onSelected: value => DaemonController.setBackend("dualsense", value)
                    }
                }
            }
        }

        GroupBox {
            title: qsTr("Reported controller name")
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                Label {
                    text: qsTr("Applying a name recreates the virtual device if that type is active — any app reading it sees a brief input interruption.")
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                NameEditor {
                    Layout.fillWidth: true
                    enabled: root.editable
                    controller: "ds4"
                    label: qsTr("DualShock 4")
                    currentName: DaemonController.ds4Name
                    isDefault: DaemonController.ds4NameIsDefault
                }

                NameEditor {
                    Layout.fillWidth: true
                    enabled: root.editable
                    controller: "dualsense"
                    label: qsTr("DualSense")
                    currentName: DaemonController.dualsenseName
                    isDefault: DaemonController.dualsenseNameIsDefault
                }
            }
        }

        GroupBox {
            title: qsTr("Hide method")
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12

            ColumnLayout {
                anchors.fill: parent

                Label {
                    text: qsTr("How the physical controller is hidden from other apps. Applies on the next physical (re)connection.")
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                OptionSelector {
                    Layout.fillWidth: true
                    enabled: root.editable
                    values: DaemonController.hideMethodValues()
                    labelMap: ({ "unbind": qsTr("unbind (default)") })
                    current: DaemonController.hideMethod
                    onSelected: value => DaemonController.setHideMethod(value)
                }
            }
        }

    }
}
