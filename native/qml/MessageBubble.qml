import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// One message, as a bubble: mine on the right, others on the left.
// In a channel, posts show a "N risposte / Rispondi" link that opens the thread.
Item {
    id: msg
    required property string id
    required property string from
    required property string content
    required property var timestamp
    required property bool mine
    required property int replyCount
    required property string fromId
    required property string richText
    required property var images   // remote URLs
    required property var files    // [{name, type, url}]
    required property var reactions // [{key, emoji, count, mine, names}]
    required property bool edited
    required property bool deleted
    required property bool mentionsMe
    required property int seenCount   // -1: no "seen" line for this message
    required property int seenTotal
    // Show the thread link (channel posts in the conversation view).
    property bool showThreadLink: false
    signal threadRequested(string postId)

    function formatTime(ms) {
        if (!ms) return ""
        const d = new Date(ms)
        const time = d.toLocaleTimeString(Qt.locale(), Locale.ShortFormat)
        if (d.toDateString() === new Date().toDateString()) return time
        return d.toLocaleDateString(Qt.locale(), Locale.ShortFormat) + " " + time
    }

    width: ListView.view ? ListView.view.width : implicitWidth
    height: bubble.height + (seenLine.visible ? seenLine.height + Kirigami.Units.smallSpacing / 2 : 0)

    // Read receipt under my newest message, as in Teams.
    QQC2.Label {
        id: seenLine
        visible: msg.mine && msg.seenCount >= 0
        anchors.right: bubble.right
        anchors.top: bubble.bottom
        anchors.topMargin: Kirigami.Units.smallSpacing / 2
        font: Kirigami.Theme.smallFont
        color: Kirigami.Theme.disabledTextColor
        text: msg.seenTotal <= 1
              ? (msg.seenCount > 0 ? "👁 Visualizzato" : "✓ Inviato")
              : (msg.seenCount > 0 ? qsTr("👁 Seen by %1 of %2").arg(msg.seenCount).arg(msg.seenTotal) : qsTr("✓ Sent"))
    }

    HoverHandler { id: hover }

    // Add a reaction: appears next to the bubble on hover, as in Teams.
    QQC2.ToolButton {
        id: reactButton
        visible: (hover.hovered || picker.opened) && !msg.deleted
        anchors.verticalCenter: bubble.verticalCenter
        x: msg.mine ? bubble.x - width - Kirigami.Units.smallSpacing : bubble.x + bubble.width + Kirigami.Units.smallSpacing
        icon.name: "smiley-add"
        onClicked: picker.open()
        QQC2.ToolTip.text: qsTr("React")
        QQC2.ToolTip.visible: hovered

        // Reply (quote), copy; own messages also edit / delete. Next to the reaction button.
        QQC2.ToolButton {
            anchors.right: msg.mine ? parent.left : undefined
            anchors.left: msg.mine ? undefined : parent.right
            anchors.verticalCenter: parent.verticalCenter
            icon.name: "overflow-menu"
            onClicked: ownMenu.popup()
            QQC2.Menu {
                id: ownMenu
                QQC2.MenuItem { icon.name: "mail-reply-sender"; text: qsTr("Reply"); onTriggered: backend.setQuote(msg.id) }
                QQC2.MenuItem { icon.name: "edit-copy"; text: qsTr("Copy text"); onTriggered: backend.copyToClipboard(msg.content) }
                QQC2.MenuSeparator { visible: msg.mine }
                QQC2.MenuItem { visible: msg.mine; height: visible ? implicitHeight : 0; icon.name: "document-edit"; text: qsTr("Edit"); onTriggered: applicationWindow().editMessage(msg.id, msg.content) }
                QQC2.MenuItem { visible: msg.mine; height: visible ? implicitHeight : 0; icon.name: "edit-delete"; text: qsTr("Delete"); onTriggered: applicationWindow().confirmDelete(msg.id) }
            }
        }

        QQC2.Popup {
            id: picker
            y: -height
            padding: Kirigami.Units.smallSpacing
            contentItem: Row {
                spacing: Kirigami.Units.smallSpacing
                Repeater {
                    model: backend.quickReactions()
                    delegate: QQC2.ToolButton {
                        required property var modelData
                        text: modelData.emoji
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.4
                        onClicked: {
                            backend.toggleReaction(msg.id, modelData.key)
                            picker.close()
                        }
                    }
                }
            }
        }
    }

    readonly property real avatarSize: Kirigami.Units.iconSizes.medium
    readonly property real imageMaxWidth: Kirigami.Units.gridUnit * 16

    function fileIcon(type) {
        switch (type) {
        case "xlsx": case "xls": case "csv": return "x-office-spreadsheet"
        case "docx": case "doc": case "odt": return "x-office-document"
        case "pptx": case "ppt": return "x-office-presentation"
        case "pdf": return "application-pdf"
        case "zip": case "rar": case "7z": case "tar": case "gz": return "package-x-generic"
        case "png": case "jpg": case "jpeg": case "gif": case "webp": return "image-x-generic"
        case "mp4": case "mov": return "video-x-generic"
        default: return "text-x-generic"
        }
    }

    // Others' messages: their picture (or initials) and availability, as in Teams.
    PresenceAvatar {
        visible: !msg.mine
        x: Kirigami.Units.largeSpacing
        width: msg.avatarSize
        height: msg.avatarSize
        name: msg.from
        mri: msg.mine ? "" : msg.fromId
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: applicationWindow().showPerson(msg.fromId) }
    }

    Rectangle {
        id: bubble
        readonly property real maxWidth: msg.width * 0.75
        x: msg.mine ? msg.width - width - Kirigami.Units.largeSpacing
                    : Kirigami.Units.largeSpacing + msg.avatarSize + Kirigami.Units.smallSpacing
        width: Math.min(maxWidth, Math.max(header.implicitWidth,
                                           body.visible ? body.implicitWidth : 0,
                                           threadLink.visible ? threadLink.implicitWidth : 0,
                                           msg.images.length > 0 ? msg.imageMaxWidth : 0,
                                           fileList.visible ? fileList.Layout.preferredWidth : 0)
                        + Kirigami.Units.largeSpacing * 2)
        height: layout.implicitHeight + Kirigami.Units.mediumSpacing * 2
        radius: Kirigami.Units.cornerRadius * 2
        // Mine: a light tint of the accent colour under the normal text colour, readable with
        // any theme (the full highlight colour with its text was unreadable on light accents).
        color: msg.deleted ? "transparent"
             : (msg.mine ? Qt.tint(Kirigami.Theme.backgroundColor,
                                   Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.22))
                         : Kirigami.Theme.alternateBackgroundColor)
        // Mentions of you stand out, as in Teams.
        border.width: msg.mentionsMe || msg.deleted ? 1 : 0
        border.color: msg.mentionsMe ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.disabledTextColor

        ColumnLayout {
            id: layout
            x: Kirigami.Units.largeSpacing
            y: Kirigami.Units.mediumSpacing
            width: bubble.width - Kirigami.Units.largeSpacing * 2
            spacing: Kirigami.Units.smallSpacing / 2

            QQC2.Label {
                id: header
                text: (msg.mine ? "" : msg.from + "  ·  ") + msg.formatTime(msg.timestamp) + (msg.edited ? "  ·  modificato" : "")
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
            }
            Kirigami.SelectableLabel {
                id: body
                Layout.fillWidth: true
                visible: msg.content.length > 0
                // richText is built by the backend from plain text: escaped, links only.
                text: msg.richText
                textFormat: TextEdit.RichText
                wrapMode: Text.Wrap
                color: msg.deleted ? Kirigami.Theme.disabledTextColor
                     : Kirigami.Theme.textColor
                font.italic: msg.deleted
                onLinkActivated: link => Qt.openUrlExternally(link)
            }

            // Pictures pasted in the message; a click opens the full image in the system viewer.
            Repeater {
                model: msg.images
                delegate: Image {
                    id: picture
                    required property string modelData
                    readonly property string localUrl: { backend.mediaRevision; return backend.mediaUrl(modelData) }
                    Layout.preferredWidth: Math.min(msg.imageMaxWidth, layout.width)
                    Layout.preferredHeight: status === Image.Ready
                        ? Math.min(Kirigami.Units.gridUnit * 14, Layout.preferredWidth * implicitHeight / Math.max(1, implicitWidth))
                        : Kirigami.Units.gridUnit * 6
                    source: localUrl
                    fillMode: Image.PreserveAspectFit
                    horizontalAlignment: Image.AlignLeft
                    asynchronous: true
                    // Decode at display size: big screenshots do not cost full-size memory.
                    sourceSize.width: msg.imageMaxWidth * 2
                    QQC2.BusyIndicator {
                        anchors.centerIn: parent
                        running: picture.localUrl === ""
                        visible: running
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: picture.localUrl !== ""
                        cursorShape: Qt.PointingHandCursor
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: mouse => {
                            if (mouse.button === Qt.RightButton) imageMenu.popup()
                            else Qt.openUrlExternally(picture.localUrl)
                        }
                    }
                    QQC2.Menu {
                        id: imageMenu
                        QQC2.MenuItem { icon.name: "document-save"; text: qsTr("Save image"); onTriggered: backend.saveImage(picture.modelData) }
                        QQC2.MenuItem { icon.name: "document-open"; text: qsTr("Open"); onTriggered: Qt.openUrlExternally(picture.localUrl) }
                    }
                }
            }

            // Reactions: mine highlighted; a click adds or removes mine.
            Flow {
                Layout.fillWidth: true
                visible: msg.reactions.length > 0
                spacing: Kirigami.Units.smallSpacing
                Repeater {
                    model: msg.reactions
                    delegate: QQC2.AbstractButton {
                        id: chip
                        required property var modelData
                        padding: Kirigami.Units.smallSpacing
                        leftPadding: Kirigami.Units.mediumSpacing
                        rightPadding: Kirigami.Units.mediumSpacing
                        onClicked: backend.toggleReaction(msg.id, modelData.key)
                        background: Rectangle {
                            radius: height / 2
                            color: chip.modelData.mine
                                ? Qt.rgba(Kirigami.Theme.highlightColor.r, Kirigami.Theme.highlightColor.g, Kirigami.Theme.highlightColor.b, 0.25)
                                : Kirigami.Theme.backgroundColor
                            border.width: 1
                            border.color: chip.modelData.mine ? Kirigami.Theme.highlightColor
                                        : (chip.hovered ? Kirigami.Theme.disabledTextColor : "transparent")
                        }
                        contentItem: QQC2.Label {
                            text: chip.modelData.emoji + " " + chip.modelData.count
                            color: Kirigami.Theme.textColor
                        }
                        QQC2.ToolTip.text: chip.modelData.names
                        QQC2.ToolTip.visible: hovered
                    }
                }
            }

            // Shared files: open in SharePoint / OneDrive in the browser.
            ColumnLayout {
                id: fileList
                visible: msg.files.length > 0
                // A fixed width: deriving it from the bubble (whose width comes from this list)
                // collapsed file-only messages to a tiny square.
                Layout.preferredWidth: Math.min(Kirigami.Units.gridUnit * 16, bubble.maxWidth - Kirigami.Units.largeSpacing * 2)
                spacing: Kirigami.Units.smallSpacing
                Repeater {
                    model: msg.files
                    delegate: QQC2.ItemDelegate {
                        id: file
                        required property var modelData
                        Layout.fillWidth: true
                        enabled: modelData.url !== ""
                        // A click asks what to do, as Teams does: download or open in the browser.
                        onClicked: fileMenu.popup()
                        QQC2.Menu {
                            id: fileMenu
                            QQC2.MenuItem {
                                icon.name: "download"
                                text: qsTr("Download")
                                enabled: !backend.downloading && (file.modelData.shareUrl || "") !== ""
                                onTriggered: backend.downloadFile(file.modelData)
                            }
                            QQC2.MenuItem {
                                icon.name: "internet-web-browser"
                                text: qsTr("Open in browser")
                                onTriggered: Qt.openUrlExternally(file.modelData.url)
                            }
                        }
                        background: Rectangle {
                            radius: Kirigami.Units.cornerRadius
                            color: Kirigami.Theme.backgroundColor
                            border.color: Kirigami.Theme.disabledTextColor
                            border.width: file.hovered ? 1 : 0
                        }
                        contentItem: RowLayout {
                            spacing: Kirigami.Units.smallSpacing
                            Kirigami.Icon {
                                source: msg.fileIcon(file.modelData.type)
                                implicitWidth: Kirigami.Units.iconSizes.medium
                                implicitHeight: implicitWidth
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: file.modelData.name
                                color: Kirigami.Theme.textColor
                                elide: Text.ElideMiddle
                            }
                            QQC2.ToolButton {
                                icon.name: "download"
                                enabled: !backend.downloading && (file.modelData.shareUrl || "") !== ""
                                onClicked: backend.downloadFile(file.modelData)
                                QQC2.ToolTip.text: qsTr("Download to Downloads")
                                QQC2.ToolTip.visible: hovered
                            }
                        }
                        QQC2.ToolTip.text: qsTr("Download or open in browser")
                        QQC2.ToolTip.visible: hovered
                    }
                }
            }
            QQC2.Label {
                id: threadLink
                visible: msg.showThreadLink
                text: msg.replyCount > 0 ? (msg.replyCount === 1 ? qsTr("1 reply") : qsTr("%1 replies").arg(msg.replyCount)) : qsTr("Reply")
                font.pointSize: Kirigami.Theme.smallFont.pointSize
                font.underline: linkArea.containsMouse
                color: Kirigami.Theme.linkColor
                MouseArea {
                    id: linkArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: msg.threadRequested(msg.id)
                }
            }
        }
    }
}
