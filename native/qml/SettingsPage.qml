import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The microphone, speakers and camera of the calls. The first entry of each list is the system
// default; the choice is kept in the settings and used when a call starts.
Kirigami.ScrollablePage {
    id: page
    objectName: "settingsPage"
    title: qsTr("Settings")

    component DeviceCombo: QQC2.ComboBox {
        id: combo
        required property string kind
        objectName: kind + "Combo"
        textRole: "name"
        valueRole: "id"
        Layout.preferredWidth: Kirigami.Units.gridUnit * 22
        function refresh() {
            model = backend.mediaDevices(kind)
            const i = indexOfValue(backend.selectedDevice(kind))
            currentIndex = i >= 0 ? i : 0   // a chosen device that is unplugged shows the default
        }
        Component.onCompleted: refresh()
        onActivated: backend.selectDevice(kind, currentValue)
    }

    Kirigami.FormLayout {
        DeviceCombo {
            Kirigami.FormData.label: qsTr("Microphone:")
            kind: "audioInput"
        }
        DeviceCombo {
            Kirigami.FormData.label: qsTr("Speakers:")
            kind: "audioOutput"
        }
        DeviceCombo {
            Kirigami.FormData.label: qsTr("Camera:")
            kind: "camera"
        }
        QQC2.Label {
            Layout.maximumWidth: Kirigami.Units.gridUnit * 22
            wrapMode: Text.Wrap
            font: Kirigami.Theme.smallFont
            opacity: 0.7
            text: qsTr("The choice applies from the next call (the camera, the next time it is turned on).")
        }
    }
}
