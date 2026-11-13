import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import KTeams

// Kirigami column layout: chat list and chat side by side on wide windows,
// one at a time (with back navigation) on narrow ones.
Kirigami.ApplicationWindow {
    id: root
    title: backend.unreadCount > 0 ? "(" + backend.unreadCount + ") KTeams" : "KTeams"

    // Keyboard shortcuts
    Shortcut { sequence: "Ctrl+K"; onActivated: root.focusSearch() }
    Shortcut { sequence: "Ctrl+N"; onActivated: if (backend.state === Backend.Connected) root.newChat() }
    Shortcut { sequence: "Ctrl+1"; onActivated: root.selectTab(0) }
    Shortcut { sequence: "Ctrl+2"; onActivated: root.selectTab(1) }
    Shortcut { sequence: "Ctrl+3"; onActivated: root.selectTab(2) }
    Shortcut { sequence: "Ctrl+4"; onActivated: root.selectTab(3) }
    Shortcut { sequence: "Ctrl+Q"; onActivated: Qt.quit() }
    property var conversationsPageItem: pageStack.depth > 0 && pageStack.items[0].objectName === "conversations" ? pageStack.items[0] : null
    function focusSearch() { if (conversationsPageItem) conversationsPageItem.focusSearch() }
    function selectTab(i) { if (conversationsPageItem) conversationsPageItem.selectTab(i) }
    width: Kirigami.Units.gridUnit * 60
    height: Kirigami.Units.gridUnit * 40
    minimumWidth: Kirigami.Units.gridUnit * 20
    minimumHeight: Kirigami.Units.gridUnit * 20

    pageStack.defaultColumnWidth: Kirigami.Units.gridUnit * 20

    // Closing hides to the tray; qsTr("Sign out") in the tray menu quits.
    onClosing: close => {
        if (desktop.trayAvailable) {
            close.accepted = false
            root.hide()
        }
    }
    pageStack.globalToolBar.style: Kirigami.ApplicationHeaderStyle.ToolBar
    pageStack.globalToolBar.showNavigationButtons: Kirigami.ApplicationHeaderStyle.ShowBackButton

    Component { id: loginPage; LoginPage {} }
    Component { id: conversationsPage; ConversationsPage {} }
    Component { id: chatPage; ChatPage {} }
    Component { id: threadPage; ThreadPage {} }
    Component { id: callPage; CallPage {} }
    Component { id: settingsPage; SettingsPage {} }

    // The video page sits on a layer above everything; opened when video flows either way.
    property bool callPageShown: pageStack.layers.depth > 1 && pageStack.layers.currentItem.objectName === "callPage"
    function showSettings() { pageStack.layers.push(settingsPage) }
    function showCallPage() { if (!callPageShown) pageStack.layers.push(callPage) }
    function hideCallPage() { if (callPageShown) pageStack.layers.pop() }

    NewChatDialog { id: newChatDialog }
    PersonCard { id: personCard }
    function showPerson(mri) { if (mri) personCard.show(mri) }

    Kirigami.PromptDialog {
        id: editDialog
        property string messageId
        title: qsTr("Edit message")
        standardButtons: Kirigami.Dialog.Save | Kirigami.Dialog.Cancel
        preferredWidth: Kirigami.Units.gridUnit * 26
        QQC2.TextArea {
            id: editText
            wrapMode: TextEdit.Wrap
        }
        onAccepted: backend.editMessage(messageId, editText.text)
    }

    Kirigami.PromptDialog {
        id: deleteDialog
        property string messageId
        title: qsTr("Delete the message?")
        subtitle: qsTr("It will be deleted for everyone.")
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel
        onAccepted: backend.deleteMessage(messageId)
    }

    Kirigami.PromptDialog {
        id: statusMessageDialog
        title: qsTr("Status message")
        subtitle: qsTr("Others see it next to your name. Empty to remove it.")
        standardButtons: Kirigami.Dialog.Save | Kirigami.Dialog.Cancel
        QQC2.TextField { id: statusMessageText; placeholderText: qsTr("E.g. On holiday until Monday") }
        onAccepted: backend.setStatusMessage(statusMessageText.text)
    }

    Kirigami.PromptDialog {
        id: renameDialog
        property string chatId
        title: qsTr("Rename chat")
        standardButtons: Kirigami.Dialog.Save | Kirigami.Dialog.Cancel
        QQC2.TextField { id: renameText; placeholderText: qsTr("Chat name") }
        onAccepted: backend.renameChat(chatId, renameText.text)
    }
    Kirigami.PromptDialog {
        id: leaveDialog
        property string chatId
        title: qsTr("Leave the chat?")
        subtitle: qsTr("You will no longer receive this chat's messages.")
        standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel
        onAccepted: backend.leaveChat(chatId)
    }
    NewMeetingDialog { id: newMeetingDialog }
    function renameChat(id, title) { renameDialog.chatId = id; renameText.text = title; renameDialog.open(); renameText.forceActiveFocus(); renameText.selectAll() }
    function confirmLeave(id, title) { leaveDialog.chatId = id; leaveDialog.title = qsTr("Leave \"%1\"?").arg(title); leaveDialog.open() }
    function addPeople(id) { newChatDialog.addToChat = id; newChatDialog.open() }
    function newMeeting() { newMeetingDialog.open() }

    function editStatusMessage() {
        statusMessageDialog.open()
        statusMessageText.forceActiveFocus()
    }
    function newChat() { newChatDialog.addToChat = ""; newChatDialog.open() }
    function editMessage(id, text) {
        editDialog.messageId = id
        editText.text = text
        editDialog.open()
        editText.forceActiveFocus()
    }
    function confirmDelete(id) {
        deleteDialog.messageId = id
        deleteDialog.open()
    }

    function showPagesForState() {
        const connected = backend.state === Backend.Connected
        const onLogin = pageStack.depth > 0 && pageStack.items[0].objectName === "login"
        if (pageStack.depth > 0 && connected !== onLogin) return
        pageStack.clear()
        pageStack.push(connected ? conversationsPage : loginPage)
    }

    // Called by ConversationsPage after a chat is selected.
    function openChat() {
        while (pageStack.depth > 2) pageStack.pop()
        if (pageStack.depth < 2) pageStack.push(chatPage)
        else pageStack.currentIndex = 1
    }

    // Called by ChatPage when a channel post's thread is opened.
    function openThread() {
        if (pageStack.depth < 3) pageStack.push(threadPage)
        else pageStack.currentIndex = 2
    }

    function closeThread() {
        backend.closeThread()
    }

    Connections {
        target: backend
        function onStateChanged() { root.showPagesForState() }
        function onCurrentThreadChanged() {
            if (backend.currentThreadId === "" && pageStack.depth > 2) pageStack.pop()
        }
        function onOpenConversationRequested(id) {
            root.raise()
            root.requestActivate()
            backend.selectConversation(id)
            root.openChat()
        }
        function onNativeCallChanged() {
            if (!backend.nativeCallActive) root.hideCallPage()
            else if (backend.remoteVideo || backend.cameraOn || backend.remoteShare) root.showCallPage()
        }
    }
    // Native call (prototype): state, mute, hang up.
    footer: QQC2.ToolBar {
        visible: backend.nativeCallActive || backend.nativeCallState !== ""
        RowLayout {
            anchors.fill: parent
            Kirigami.Icon { source: "phone"; implicitWidth: Kirigami.Units.iconSizes.small; implicitHeight: implicitWidth }
            QQC2.Label { Layout.fillWidth: true; text: backend.nativeCallState; elide: Text.ElideRight }
            QQC2.ToolButton {
                visible: backend.nativeCallIncoming
                icon.name: "call-start"
                text: qsTr("Reply")
                highlighted: true
                onClicked: backend.acceptNativeCall()
            }
            QQC2.ToolButton {
                visible: backend.nativeCallIncoming
                icon.name: "call-stop"
                text: qsTr("Decline")
                onClicked: backend.rejectNativeCall()
            }
            QQC2.ToolButton {
                id: muteButton
                visible: backend.nativeCallActive && !backend.nativeCallIncoming
                checkable: true
                icon.name: checked ? "microphone-sensitivity-muted" : "microphone-sensitivity-high"
                text: checked ? qsTr("Unmute") : qsTr("Mute")
                onToggled: backend.muteNativeCall(checked)
            }
            QQC2.ToolButton {
                visible: backend.nativeCallActive && !backend.nativeCallIncoming
                icon.name: backend.cameraOn ? "camera-video" : "camera-off"
                text: backend.cameraOn ? qsTr("Camera off") : qsTr("Camera")
                onClicked: backend.setCameraEnabled(!backend.cameraOn)
            }
            QQC2.ToolButton {
                visible: backend.nativeCallActive && !backend.nativeCallIncoming
                icon.name: backend.sharingOn ? "media-playback-stop" : "screen-share"
                text: backend.sharingOn ? qsTr("Stop sharing") : qsTr("Share screen")
                onClicked: backend.sharingOn ? backend.stopScreenShare() : backend.startScreenShare()
            }
            QQC2.ToolButton {
                visible: backend.nativeCallActive && !backend.nativeCallIncoming
                icon.name: root.callPageShown ? "view-restore" : "video-display"
                text: root.callPageShown ? qsTr("Back to chat") : qsTr("Video")
                onClicked: root.callPageShown ? root.hideCallPage() : root.showCallPage()
            }
            QQC2.ToolButton {
                visible: backend.nativeCallActive && !backend.nativeCallIncoming && backend.callChatId !== ""
                icon.name: "mail-message"
                text: qsTr("Chat")
                onClicked: {
                    root.hideCallPage()
                    backend.selectConversation(backend.callChatId, backend.callChatTitle)
                    root.openChat()
                }
            }
            QQC2.ToolButton {
                visible: backend.nativeCallActive && !backend.nativeCallIncoming
                icon.name: "call-stop"
                text: qsTr("Hang up")
                onClicked: { muteButton.checked = false; backend.hangUpNativeCall() }
            }
        }
    }

    Component.onCompleted: showPagesForState()
}
