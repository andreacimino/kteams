import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page
    objectName: "conversations"
    function focusSearch() { tabs.currentIndex = 0; searchField.forceActiveFocus(); searchField.selectAll() }
    function selectTab(i) { tabs.currentIndex = i }
    title: activityView ? qsTr("Activity") : (calendarView ? qsTr("Calendar") : (teamsView ? qsTr("Teams") : qsTr("Chat")))

    readonly property bool teamsView: tabs.currentIndex === 1
    readonly property bool calendarView: tabs.currentIndex === 2
    readonly property bool activityView: tabs.currentIndex === 3
    // Message results instead of chats while searching (the Messages toggle).
    property bool messageSearch: false
    readonly property bool messagesView: !teamsView && !calendarView && !activityView && messageSearch && searchField.text.length >= 1

    function formatTime(ms) {
        if (!ms) return ""
        const d = new Date(ms)
        const now = new Date()
        if (d.toDateString() === now.toDateString())
            return d.toLocaleTimeString(Qt.locale(), Locale.ShortFormat)
        return d.toLocaleDateString(Qt.locale(), Locale.ShortFormat)
    }

    actions: [
        Kirigami.Action {
            icon.name: "document-new"
            text: qsTr("New chat")
            displayHint: Kirigami.DisplayHint.IconOnly
            onTriggered: applicationWindow().newChat()
        },
        Kirigami.Action {
            icon.name: "view-refresh"
            text: qsTr("Refresh")
            displayHint: Kirigami.DisplayHint.IconOnly
            onTriggered: backend.refresh()
        },
        Kirigami.Action {
            icon.name: "call-start"
            text: qsTr("Test call")
            displayHint: Kirigami.DisplayHint.AlwaysHide
            enabled: !backend.nativeCallActive
            onTriggered: backend.startNativeTestCall()
        },
        Kirigami.Action {
            icon.name: "system-run"
            text: qsTr("Start at login")
            checkable: true
            checked: desktop.startAtLogin
            displayHint: Kirigami.DisplayHint.AlwaysHide
            onTriggered: desktop.startAtLogin = checked
        },
        Kirigami.Action {
            icon.name: "system-log-out"
            text: qsTr("Sign out") + (backend.selfDisplayName ? " (" + backend.selfDisplayName + ")" : "")
            displayHint: Kirigami.DisplayHint.AlwaysHide
            onTriggered: backend.logout()
        }
    ]

    header: QQC2.ToolBar {
        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing

            // Me: picture, name and status; click to change the status.
            QQC2.ItemDelegate {
                id: me
                Layout.fillWidth: true
                readonly property string presence: { backend.presenceRevision; return backend.presenceOf(backend.selfMriValue) }
                readonly property var labels: ({
                    "Available": qsTr("Available"), "AvailableIdle": qsTr("Available"), "Busy": qsTr("Busy"), "BusyIdle": qsTr("Busy"),
                    "DoNotDisturb": qsTr("Do not disturb"), "Presenting": qsTr("Presenting"), "InACall": qsTr("In a call"),
                    "InAConferenceCall": qsTr("In a meeting"), "BeRightBack": qsTr("Be right back"), "Away": qsTr("Away"), "Offline": "Offline"
                })
                onClicked: statusMenu.popup()
                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing
                    PresenceAvatar {
                        Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                        Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                        name: backend.selfDisplayName
                        mri: backend.selfMriValue
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        QQC2.Label { Layout.fillWidth: true; text: backend.selfDisplayName; font.bold: true; elide: Text.ElideRight }
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: me.labels[me.presence] || qsTr("Set status")
                            font: Kirigami.Theme.smallFont
                            color: Kirigami.Theme.disabledTextColor
                        }
                    }
                    Kirigami.Icon { source: "go-down"; implicitWidth: Kirigami.Units.iconSizes.small; implicitHeight: implicitWidth }
                }
                QQC2.Menu {
                    id: statusMenu
                    Repeater {
                        model: [["Available", qsTr("Available")], ["Busy", qsTr("Busy")], ["DoNotDisturb", qsTr("Do not disturb")],
                                ["BeRightBack", qsTr("Be right back")], ["Away", qsTr("Away")], ["Offline", qsTr("Appear offline")]]
                        delegate: QQC2.MenuItem {
                            required property var modelData
                            text: modelData[1]
                            checkable: true
                            checked: me.presence === modelData[0]
                            onTriggered: backend.setMyAvailability(modelData[0])
                        }
                    }
                    QQC2.MenuSeparator {}
                    QQC2.MenuItem {
                        icon.name: "document-edit"
                        text: qsTr("Status message…")
                        onTriggered: applicationWindow().editStatusMessage()
                    }
                }
            }
            QQC2.TabBar {
                id: tabs
                Layout.fillWidth: true
                QQC2.TabButton { text: backend.unreadCount > 0 ? qsTr("Chat (%1)").arg(backend.unreadCount) : qsTr("Chat"); icon.name: "dialog-messages" }
                QQC2.TabButton { text: qsTr("Teams"); icon.name: "group" }
                QQC2.TabButton { text: qsTr("Calendar"); icon.name: "view-calendar" }
                QQC2.TabButton { text: backend.activityUnread > 0 ? qsTr("Activity (%1)").arg(backend.activityUnread) : qsTr("Activity"); icon.name: "notifications" }
                onCurrentIndexChanged: if (currentIndex === 2) backend.loadCalendarWeek(backend.calendarWeekOffset)
                Component.onCompleted: if (backend.startTab >= 0) currentIndex = backend.startTab
            }
            RowLayout {
                Layout.fillWidth: true
                visible: !page.teamsView && !page.calendarView && !page.activityView
                Kirigami.SearchField {
                    id: searchField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Search chats and messages")
                    onTextChanged: { backend.setConversationFilter(text); messageSearchDebounce.restart() }
                }
                Timer {
                    id: messageSearchDebounce
                    interval: 300
                    onTriggered: backend.searchMessages(searchField.text)
                }
                QQC2.ToolButton {
                    // Only chats with unread messages.
                    icon.name: "mail-unread"
                    checkable: true
                    checked: backend.unreadOnly
                    onToggled: backend.unreadOnly = checked
                    QQC2.ToolTip.text: qsTr("Unread only")
                    QQC2.ToolTip.visible: hovered
                }
            }
            RowLayout {
                visible: !page.teamsView && !page.calendarView && !page.activityView && searchField.text.length >= 1
                Layout.fillWidth: true
                QQC2.ToolButton { text: qsTr("Chats"); checkable: true; autoExclusive: true; checked: !page.messageSearch; onClicked: page.messageSearch = false }
                QQC2.ToolButton {
                    text: backend.messageResults.length > 0 ? qsTr("Messages (%1)").arg(backend.messageResults.length) : qsTr("Messages")
                    checkable: true; autoExclusive: true; checked: page.messageSearch; onClicked: page.messageSearch = true
                }
                QQC2.BusyIndicator { visible: backend.indexing; running: visible; implicitHeight: Kirigami.Units.iconSizes.small; implicitWidth: implicitHeight }
                QQC2.Label { visible: backend.indexing; text: qsTr("reading the other chats…"); font: Kirigami.Theme.smallFont; color: Kirigami.Theme.disabledTextColor }
            }
            RowLayout {
                visible: page.activityView
                Layout.fillWidth: true
                QQC2.ToolButton { text: qsTr("All"); checkable: true; autoExclusive: true; checked: backend.activityFilter === ""; onClicked: backend.activityFilter = "" }
                QQC2.ToolButton { text: qsTr("Mentions"); checkable: true; autoExclusive: true; checked: backend.activityFilter === "mention"; onClicked: backend.activityFilter = "mention" }
                QQC2.ToolButton { text: qsTr("Replies"); checkable: true; autoExclusive: true; checked: backend.activityFilter === "reply"; onClicked: backend.activityFilter = "reply" }
                QQC2.ToolButton { text: qsTr("Reactions"); checkable: true; autoExclusive: true; checked: backend.activityFilter === "reaction"; onClicked: backend.activityFilter = "reaction" }
                Item { Layout.fillWidth: true }
                QQC2.ToolButton {
                    icon.name: "mail-mark-read"
                    enabled: backend.activityUnread > 0
                    onClicked: backend.markActivityRead()
                    QQC2.ToolTip.text: qsTr("Mark all as read")
                    QQC2.ToolTip.visible: hovered
                }
            }
            RowLayout {
                visible: page.calendarView
                Layout.fillWidth: true
                QQC2.ToolButton {
                    icon.name: "list-add"
                    onClicked: applicationWindow().newMeeting()
                    QQC2.ToolTip.text: qsTr("New meeting")
                    QQC2.ToolTip.visible: hovered
                }
                QQC2.ToolButton {
                    icon.name: "go-previous"
                    onClicked: backend.loadCalendarWeek(backend.calendarWeekOffset - 1)
                    QQC2.ToolTip.text: qsTr("Previous week")
                    QQC2.ToolTip.visible: hovered
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: backend.calendarTitle
                    font.bold: true
                }
                QQC2.ToolButton {
                    text: qsTr("Today")
                    enabled: backend.calendarWeekOffset !== 0
                    onClicked: backend.loadCalendarWeek(0)
                }
                QQC2.ToolButton {
                    icon.name: "go-next"
                    onClicked: backend.loadCalendarWeek(backend.calendarWeekOffset + 1)
                    QQC2.ToolTip.text: qsTr("Next week")
                    QQC2.ToolTip.visible: hovered
                }
            }
        }
    }

    ListView {
        id: list
        model: page.messagesView ? backend.messageResults : (page.activityView ? backend.activity : (page.calendarView ? backend.calendar : (page.teamsView ? backend.channels : backend.filteredConversations)))
        delegate: page.messagesView ? messageResultDelegate : (page.activityView ? activityDelegate : (page.calendarView ? eventDelegate : (page.teamsView ? channelDelegate : chatDelegate)))
        section.property: page.calendarView ? "day" : (page.teamsView ? "team" : "")
        section.delegate: Kirigami.ListSectionHeader {
            required property string section
            width: ListView.view.width
            text: section
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 2
            visible: list.count === 0
            text: backend.errorMessage ? qsTr("Error")
                : (page.messagesView ? qsTr("No messages found") : (page.activityView ? qsTr("No activity") : (page.calendarView ? qsTr("No events this week") : (page.teamsView ? qsTr("No teams") : qsTr("No chats")))))
            explanation: backend.errorMessage || backend.statusMessage
        }

    }

    Component {
        id: messageResultDelegate
        QQC2.ItemDelegate {
            id: hit
            required property var modelData
            width: ListView.view.width
            onClicked: {
                backend.selectConversation(modelData.conversationId, modelData.title)
                applicationWindow().openChat()
            }
            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing
                PresenceAvatar {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    name: hit.modelData.who
                    mri: hit.modelData.fromMri
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    RowLayout {
                        Layout.fillWidth: true
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: hit.modelData.who + (hit.modelData.title && hit.modelData.title !== hit.modelData.who ? " · " + hit.modelData.title : "")
                            elide: Text.ElideRight
                        }
                        QQC2.Label { text: page.formatTime(hit.modelData.timestamp); font: Kirigami.Theme.smallFont; color: Kirigami.Theme.disabledTextColor }
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        text: hit.modelData.text
                        font: Kirigami.Theme.smallFont
                        elide: Text.ElideRight
                        maximumLineCount: 2
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }

    Component {
        id: activityDelegate
        QQC2.ItemDelegate {
            id: act
            required property string who
            required property string whoMri
            required property string what
            required property string preview
            required property string where
            required property string threadId
            required property var timestamp
            required property bool read

            width: ListView.view.width
            onClicked: {
                if (threadId === "") return
                backend.selectConversation(threadId, who)
                applicationWindow().openChat()
            }
            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing
                PresenceAvatar {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    name: act.who
                    mri: act.whoMri
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    RowLayout {
                        Layout.fillWidth: true
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: act.who + " " + act.what + (act.where ? " · " + act.where : "")
                            font.bold: !act.read
                            elide: Text.ElideRight
                        }
                        QQC2.Label {
                            text: page.formatTime(act.timestamp)
                            font: Kirigami.Theme.smallFont
                            color: Kirigami.Theme.disabledTextColor
                        }
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        text: act.preview
                        visible: text.length > 0
                        font: Kirigami.Theme.smallFont
                        color: Kirigami.Theme.disabledTextColor
                        elide: Text.ElideRight
                        maximumLineCount: 1
                    }
                }
            }
        }
    }

    Component {
        id: eventDelegate
        QQC2.ItemDelegate {
            id: event
            required property string subject
            required property string timeRange
            required property string location
            required property string organizer
            required property string chatId
            required property bool online
            required property bool now
            required property string showAs
            required property string joinUrl
            required property string iCalUid
            required property string myResponse

            width: ListView.view.width
            // Teams meetings open their chat; other events only show their details.
            hoverEnabled: chatId !== ""
            highlighted: chatId !== "" && chatId === backend.currentConversationId
            onClicked: {
                if (chatId === "") return
                backend.selectConversation(chatId, subject)
                applicationWindow().openChat()
            }
            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing
                Rectangle {
                    // Colour bar: like Teams, stronger for busy, faded for tentative/free.
                    Layout.fillHeight: true
                    Layout.preferredWidth: Kirigami.Units.smallSpacing
                    radius: width / 2
                    color: event.now ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.highlightColor
                    opacity: event.showAs === "Busy" || event.showAs === "Oof" ? 1.0 : 0.4
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    QQC2.Label {
                        Layout.fillWidth: true
                        text: event.subject || qsTr("(no title)")
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        text: [event.timeRange, event.location, event.organizer].filter(s => s).join("  ·  ")
                        font: Kirigami.Theme.smallFont
                        color: Kirigami.Theme.disabledTextColor
                        elide: Text.ElideRight
                    }
                }
                QQC2.Button {
                    visible: event.joinUrl !== ""
                            icon.name: "camera-web"
                    text: qsTr("Join")
                    // Meetings open in the Teams web call window.
                    onClicked: backend.joinMeeting(event.joinUrl)
                }
            }
        }
    }

    Component {
        id: channelDelegate
        QQC2.ItemDelegate {
            id: channel
            required property string id
            required property string name
            required property bool unread

            width: ListView.view.width
            highlighted: id === backend.currentConversationId
            leftPadding: Kirigami.Units.largeSpacing * 2
            onClicked: {
                backend.selectConversation(id)
                applicationWindow().openChat()
            }
            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing
                QQC2.Label {
                    Layout.fillWidth: true
                    text: channel.name
                    font.bold: channel.unread
                    elide: Text.ElideRight
                }
                Rectangle {
                    visible: channel.unread
                    width: Kirigami.Units.smallSpacing * 2
                    height: width
                    radius: width / 2
                    color: Kirigami.Theme.highlightColor
                }
            }
        }
    }

    Component {
        id: chatDelegate
        QQC2.ItemDelegate {
            id: row
            required property string id
            required property string title
            required property string preview
            required property var lastActivity
            required property bool unread
            required property string peer
            required property bool pinned
            required property bool muted

            width: ListView.view.width
            // Rows move when new messages arrive: follow the id, not the row.
            highlighted: id === backend.currentConversationId
            onClicked: {
                backend.selectConversation(id)
                applicationWindow().openChat()
            }
            // Right click: pin, mute, hide; group chats also rename and leave.
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: chatMenu.popup()
            }
            TapHandler {
                acceptedDevices: PointerDevice.TouchScreen
                onLongPressed: chatMenu.popup()
            }
            QQC2.Menu {
                id: chatMenu
                QQC2.MenuItem { icon.name: row.pinned ? "window-unpin" : "window-pin"; text: row.pinned ? qsTr("Unpin") : qsTr("Pin"); onTriggered: backend.pinChat(row.id, !row.pinned) }
                QQC2.MenuItem { icon.name: row.muted ? "notifications" : "notifications-disabled"; text: row.muted ? qsTr("Unmute") : qsTr("Mute"); onTriggered: backend.muteChat(row.id, !row.muted) }
                QQC2.MenuItem { icon.name: "view-hidden"; text: qsTr("Hide"); onTriggered: backend.hideChat(row.id) }
                QQC2.MenuSeparator { visible: row.peer === "" }
                QQC2.MenuItem { visible: row.peer === ""; height: visible ? implicitHeight : 0; icon.name: "edit-rename"; text: qsTr("Rename…"); onTriggered: applicationWindow().renameChat(row.id, row.title) }
                QQC2.MenuItem { visible: row.peer === ""; height: visible ? implicitHeight : 0; icon.name: "list-add-user"; text: qsTr("Add people…"); onTriggered: applicationWindow().addPeople(row.id) }
                QQC2.MenuItem { visible: row.peer === ""; height: visible ? implicitHeight : 0; icon.name: "im-kick-user"; text: qsTr("Leave"); onTriggered: applicationWindow().confirmLeave(row.id, row.title) }
            }

            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing
                PresenceAvatar {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    name: row.title
                    mri: row.peer
                    MouseArea { anchors.fill: parent; enabled: row.peer !== ""; cursorShape: Qt.PointingHandCursor; onClicked: applicationWindow().showPerson(row.peer) }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    RowLayout {
                        Layout.fillWidth: true
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: row.title
                            font.bold: row.unread
                            elide: Text.ElideRight
                        }
                        Kirigami.Icon { visible: row.pinned; source: "window-pin"; implicitWidth: Kirigami.Units.iconSizes.small; implicitHeight: implicitWidth; opacity: 0.6 }
                        Kirigami.Icon { visible: row.muted; source: "notifications-disabled"; implicitWidth: Kirigami.Units.iconSizes.small; implicitHeight: implicitWidth; opacity: 0.6 }
                        QQC2.Label {
                            text: page.formatTime(row.lastActivity)
                            font: Kirigami.Theme.smallFont
                            color: row.unread ? Kirigami.Theme.highlightColor : Kirigami.Theme.disabledTextColor
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        QQC2.Label {
                            Layout.fillWidth: true
                            text: row.preview
                            font: Kirigami.Theme.smallFont
                            color: row.unread ? Kirigami.Theme.textColor : Kirigami.Theme.disabledTextColor
                            elide: Text.ElideRight
                            maximumLineCount: 1
                        }
                        Rectangle {
                            visible: row.unread
                            width: Kirigami.Units.smallSpacing * 2
                            height: width
                            radius: width / 2
                            color: Kirigami.Theme.highlightColor
                        }
                    }
                }
            }
        }
    }
}
