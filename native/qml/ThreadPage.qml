import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// A channel post with its replies, and the box to reply in the thread.
Kirigami.Page {
    id: page
    title: qsTr("Thread")
    padding: 0

    actions: [
        Kirigami.Action {
            icon.name: "window-close"
            text: qsTr("Close")
            displayHint: Kirigami.DisplayHint.IconOnly
            onTriggered: applicationWindow().closeThread()
        }
    ]

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            ListView {
                model: backend.thread
                verticalLayoutDirection: ListView.BottomToTop
                spacing: Kirigami.Units.smallSpacing
                topMargin: Kirigami.Units.largeSpacing
                bottomMargin: Kirigami.Units.largeSpacing
                delegate: MessageBubble {}
            }
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
            id: composer
            target: backend.currentConversationId + "/" + backend.currentThreadId
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.smallSpacing
            placeholderText: qsTr("Reply (Shift+Enter for a new line)")
            inThread: true
            onSend: text => backend.sendReply(text)
        }
    }

    // Drag and drop: the files wait in the composer as attachments until Enter.
    DropArea {
        id: dropArea
        anchors.fill: parent
        keys: ["text/uri-list"]
        enabled: backend.currentConversationId !== ""
        onDropped: drop => {
            if (!drop.hasUrls) return
            composer.addFiles(drop.urls)
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
                text: qsTr("Drop to attach")
            }
        }
    }
}
