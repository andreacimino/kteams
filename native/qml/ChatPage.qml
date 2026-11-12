import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Page {
    id: page
    title: backend.currentConversationTitle || qsTr("Chat")
    padding: 0

    // Search inside this chat: a field above the messages, Ctrl+F.
    property bool searching: false
    Shortcut { sequence: StandardKey.Find; onActivated: { page.searching = true; chatSearch.forceActiveFocus() } }
    onSearchingChanged: if (!searching) backend.chatFilter = ""

    actions: [
        Kirigami.Action {
            icon.name: "edit-find"
            text: qsTr("Search in chat")
            checkable: true
            checked: page.searching
            onTriggered: { page.searching = checked; if (checked) chatSearch.forceActiveFocus() }
        },
        Kirigami.Action {
            icon.name: "phone"
            text: qsTr("Call")
            visible: backend.canNativeCall
            enabled: !backend.nativeCallActive
            onTriggered: backend.startNativeCall()
        },
        Kirigami.Action {
            icon.name: "camera-web"
            text: qsTr("Video call")
            visible: backend.canNativeCall
            enabled: !backend.nativeCallActive
            onTriggered: backend.startNativeVideoCall()
        },
        Kirigami.Action {
            icon.name: "list-add-user"
            text: qsTr("Add people…")
            visible: !backend.currentIsChannel && backend.currentMembers.length >= 2
            displayHint: Kirigami.DisplayHint.AlwaysHide
            onTriggered: applicationWindow().addPeople(backend.currentConversationId)
        },
        Kirigami.Action {
            icon.name: "edit-rename"
            text: qsTr("Rename chat…")
            visible: !backend.currentIsChannel && backend.currentMembers.length >= 2
            displayHint: Kirigami.DisplayHint.AlwaysHide
            onTriggered: applicationWindow().renameChat(backend.currentConversationId, backend.currentConversationTitle)
        },
        Kirigami.Action {
            icon.name: "im-kick-user"
            text: qsTr("Leave chat")
            visible: !backend.currentIsChannel && backend.currentMembers.length >= 2
            displayHint: Kirigami.DisplayHint.AlwaysHide
            onTriggered: applicationWindow().confirmLeave(backend.currentConversationId, backend.currentConversationTitle)
        }
    ]

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.smallSpacing
            visible: page.searching
            Kirigami.SearchField {
                id: chatSearch
                Layout.fillWidth: true
                placeholderText: qsTr("Search in this chat")
                text: backend.chatFilter
                onTextChanged: backend.chatFilter = text
                Keys.onEscapePressed: page.searching = false
            }
            QQC2.Label {
                visible: backend.chatFilter !== ""
                text: qsTr("%1 found").arg(messages.count)
                color: Kirigami.Theme.disabledTextColor
            }
            QQC2.ToolButton {
                // The filter only sees the messages loaded so far: older pages on request.
                visible: backend.chatFilter !== "" && backend.hasMoreHistory
                enabled: !backend.loadingOlder
                icon.name: "go-up"
                text: qsTr("Older messages")
                onClicked: backend.loadOlder()
            }
            QQC2.ToolButton { icon.name: "dialog-close"; onClicked: page.searching = false }
        }

        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: messages
                model: backend.chatFilter !== "" ? backend.filteredMessages : backend.messages
                // Model rows are newest first: row 0 sits at the bottom, older rows above.
                verticalLayoutDirection: ListView.BottomToTop
                spacing: Kirigami.Units.smallSpacing
                topMargin: Kirigami.Units.largeSpacing
                bottomMargin: Kirigami.Units.largeSpacing

                // Reaching the oldest row (top of the view) loads the previous page.
                onCountChanged: if (count > 0 && atYBeginning && backend.chatFilter === "") backend.loadOlder()
                onAtYBeginningChanged: if (atYBeginning && backend.chatFilter === "") backend.loadOlder()

                // With BottomToTop the footer sits above the oldest message.
                footer: Item {
                    width: ListView.view.width
                    height: backend.loadingOlder ? busy.implicitHeight + Kirigami.Units.largeSpacing : 0
                    QQC2.BusyIndicator {
                        id: busy
                        anchors.centerIn: parent
                        running: backend.loadingOlder
                        visible: running
                    }
                }

                delegate: MessageBubble {
                    showThreadLink: backend.currentIsChannel
                    onThreadRequested: postId => {
                        backend.openThread(postId)
                        applicationWindow().openThread()
                    }
                }
            }
        }

        QQC2.Label {
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.largeSpacing
            visible: backend.typingText.length > 0
            text: backend.typingText
            font: Kirigami.Theme.smallFont
            color: Kirigami.Theme.disabledTextColor
            elide: Text.ElideRight
        }

        Kirigami.Separator { Layout.fillWidth: true }

        // The message being quoted in the next one (Teams "reply").
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.smallSpacing
            visible: backend.quoteWho !== ""
            type: Kirigami.MessageType.Information
            text: "<b>" + backend.quoteWho + "</b>: " + backend.quoteText
            showCloseButton: true
            onVisibleChanged: if (!visible) backend.clearQuote()
        }
        Composer {
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.smallSpacing
            enabled: backend.currentConversationId !== ""
            placeholderText: backend.currentIsChannel
                ? qsTr("New post (Shift+Enter for a new line)")
                : qsTr("Write a message (Shift+Enter for a new line)")
            onSend: text => backend.sendMessage(text)
        }
    }

    // Drag and drop: pictures are sent inline, other files through OneDrive/SharePoint.
    DropArea {
        id: dropArea
        anchors.fill: parent
        keys: ["text/uri-list"]
        enabled: backend.currentConversationId !== ""
        onDropped: drop => {
            if (!drop.hasUrls) return
            for (const url of drop.urls) {
                const name = url.toString().toLowerCase()
                if (/\.(png|jpe?g|gif|webp|bmp)$/.test(name)) backend.sendImageFile(url, false)
                else backend.sendFile(url, false)
            }
            drop.accept(Qt.CopyAction)
        }
        Rectangle {
            anchors.fill: parent
            visible: dropArea.containsDrag
            color: Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.15)
            border.color: Kirigami.Theme.highlightColor
            border.width: 2
            radius: Kirigami.Units.cornerRadius
            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.gridUnit * 4
                icon.name: "document-send"
                text: qsTr("Drop to send")
            }
        }
    }
}
