import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Kollega

// Sign-in without an embedded browser: Microsoft's device code flow. The user opens
// the Microsoft page in their own browser, types the code and signs in as usual
// (MFA and company policies included).
Kirigami.Page {
    id: page
    objectName: "login"
    title: "Kollega"

    readonly property bool busy: backend.state === Backend.Starting || backend.state === Backend.Connecting
    readonly property bool waitingForCode: backend.state === Backend.WaitingForCode

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 26)
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Error
            text: backend.errorMessage
            visible: text.length > 0 && !page.busy
        }

        // Starting up or connecting with a saved session
        ColumnLayout {
            visible: page.busy
            Layout.alignment: Qt.AlignHCenter
            spacing: Kirigami.Units.largeSpacing
            QQC2.BusyIndicator { Layout.alignment: Qt.AlignHCenter; running: page.busy }
            QQC2.Label {
                Layout.alignment: Qt.AlignHCenter
                text: backend.statusMessage || "Avvio..."
                opacity: 0.7
            }
        }

        // Signed out
        Kirigami.PlaceholderMessage {
            visible: !page.busy && !page.waitingForCode
            Layout.fillWidth: true
            icon.name: "im-user"
            text: qsTr("Sign in to Microsoft Teams")
            explanation: qsTr("Use your work account. The sign-in happens on the Microsoft page, in your browser.")
            helpfulAction: Kirigami.Action {
                icon.name: "go-next"
                text: qsTr("Sign in")
                onTriggered: backend.startLogin()
            }
        }

        // Device code shown, waiting for the user to sign in
        ColumnLayout {
            visible: page.waitingForCode
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Kirigami.Heading {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                level: 2
                text: qsTr("Enter this code")
            }
            Kirigami.SelectableLabel {
                Layout.alignment: Qt.AlignHCenter
                text: backend.userCode
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 2.5
                font.family: "monospace"
                font.letterSpacing: 4
            }
            QQC2.Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                opacity: 0.7
                text: qsTr("on %1 and sign in as usual.").arg(backend.verificationUrl)
            }
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: Kirigami.Units.smallSpacing
                QQC2.Button {
                    icon.name: "internet-web-browser"
                    text: qsTr("Copy the code and open the page")
                    highlighted: true
                    onClicked: {
                        backend.copyToClipboard(backend.userCode)
                        Qt.openUrlExternally(backend.verificationUrl)
                    }
                }
                QQC2.Button {
                    icon.name: "dialog-cancel"
                    text: qsTr("Cancel")
                    onClicked: backend.cancelLogin()
                }
            }
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                QQC2.BusyIndicator { running: page.waitingForCode; implicitWidth: Kirigami.Units.gridUnit * 1.5; implicitHeight: implicitWidth }
                QQC2.Label { text: qsTr("Waiting for the sign-in..."); opacity: 0.7 }
            }
        }
    }
}
