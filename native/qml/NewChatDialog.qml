import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Search people in the organization and open (or create) a chat with them.
Kirigami.Dialog {
    id: dialog
    // Set to a group chat id: the person picked is added to that chat instead.
    property string addToChat: ""
    title: addToChat !== "" ? qsTr("Add people") : qsTr("New chat")
    preferredWidth: Kirigami.Units.gridUnit * 24
    preferredHeight: Kirigami.Units.gridUnit * 24
    standardButtons: Kirigami.Dialog.NoButton

    onOpened: { search.text = ""; search.forceActiveFocus() }

    // Wait for a short pause in typing before asking the server.
    Timer {
        id: debounce
        interval: 300
        onTriggered: backend.searchPeople(search.text)
    }

    ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        Kirigami.SearchField {
            id: search
            Layout.fillWidth: true
            placeholderText: qsTr("Search people by name or email")
            onTextChanged: debounce.restart()
        }

        ListView {
            id: results
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 18
            clip: true
            model: backend.peopleResults

            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.gridUnit * 2
                visible: results.count === 0
                text: search.text.length < 1 ? qsTr("Type a name") : qsTr("Nobody found")
            }

            delegate: QQC2.ItemDelegate {
                id: person
                required property var modelData
                width: ListView.view.width
                onClicked: {
                    if (dialog.addToChat !== "") backend.addMembers(dialog.addToChat, [modelData.mri])
                    else backend.startChatWith(modelData.mri, modelData.name)
                    dialog.close()
                }
                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing
                    PresenceAvatar {
                        Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                        Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                        name: person.modelData.name
                        mri: person.modelData.mri
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        QQC2.Label { Layout.fillWidth: true; text: person.modelData.name; elide: Text.ElideRight }
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: person.modelData.email
                            visible: text.length > 0
                            font: Kirigami.Theme.smallFont
                            color: Kirigami.Theme.disabledTextColor
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }
    }
}
