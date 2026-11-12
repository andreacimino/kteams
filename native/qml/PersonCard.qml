import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Who a person is: picture, role, department, email, with chat and call shortcuts.
Kirigami.Dialog {
    id: card
    property string mri
    property var info: ({})
    title: info.name || qsTr("Person")
    preferredWidth: Kirigami.Units.gridUnit * 22
    standardButtons: Kirigami.Dialog.NoButton

    function show(personMri) {
        mri = personMri
        info = backend.personInfo(personMri)
        open()
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        RowLayout {
            spacing: Kirigami.Units.largeSpacing
            PresenceAvatar {
                Layout.preferredWidth: Kirigami.Units.iconSizes.huge
                Layout.preferredHeight: Kirigami.Units.iconSizes.huge
                name: card.info.name || ""
                mri: card.mri
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Kirigami.Heading { level: 3; text: card.info.name || ""; elide: Text.ElideRight; Layout.fillWidth: true }
                QQC2.Label { text: card.info.jobTitle || ""; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                QQC2.Label { text: card.info.department || ""; visible: text.length > 0; color: Kirigami.Theme.disabledTextColor; Layout.fillWidth: true }
                Kirigami.SelectableLabel { text: card.info.email || ""; visible: text.length > 0; Layout.fillWidth: true }
            }
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            QQC2.Button {
                icon.name: "dialog-messages"
                text: qsTr("Chat")
                enabled: card.mri !== "" && card.mri !== backend.selfMriValue
                onClicked: { backend.openChatWith(card.mri); card.close() }
            }
            QQC2.Button {
                icon.name: "mail-send"
                text: qsTr("Email")
                visible: (card.info.email || "") !== ""
                onClicked: Qt.openUrlExternally("mailto:" + card.info.email)
            }
            QQC2.Button { text: qsTr("Close"); onClicked: card.close() }
        }
    }
}
