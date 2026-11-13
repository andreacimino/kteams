import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami

// Message input: Enter sends, Shift+Enter adds a new line. Files dropped, picked or pasted
// wait above the field as attachments, and go with Enter (or are removed).
ColumnLayout {
    id: composer
    property alias placeholderText: input.placeholderText
    // In a channel thread: pictures go into the thread as replies.
    property bool inThread: false
    signal send(string text)

    // @mentions: people picked from the chat members, sent with their MRI.
    property var mentions: []

    function submit() {
        const text = input.text
        const hasText = text.trim().length > 0
        if (!hasText && attachments.length === 0) return
        // The text goes first, then the attachments: one message each, as before.
        if (hasText) {
            if (mentions.length > 0) backend.sendMessageWithMentions(text, mentions, inThread)
            else composer.send(text)
        }
        if (attachments.length > 0) {
            backend.sendAttachments(attachments.map(a => a.url), inThread)
            attachments = []
        }
        mentions = []
        input.clear()
    }

    // "@" plus what follows it up to the cursor, or "" when not writing a mention.
    function mentionQuery() {
        const upToCursor = input.text.substring(0, input.cursorPosition)
        const at = upToCursor.lastIndexOf("@")
        if (at < 0 || (at > 0 && !/\s/.test(upToCursor[at - 1]))) return null
        const q = upToCursor.substring(at + 1)
        if (/[\n]/.test(q) || q.length > 30) return null
        return { at: at, query: q.toLowerCase() }
    }

    function insertMention(member) {
        const m = mentionQuery()
        if (!m) return
        const before = input.text.substring(0, m.at)
        const after = input.text.substring(input.cursorPosition)
        input.text = before + "@" + member.name + " " + after
        input.cursorPosition = before.length + member.name.length + 2
        mentions = mentions.concat([member])
        mentionPopup.close()
    }

    spacing: Kirigami.Units.smallSpacing

    // Where the attachments would go: another chat or thread drops them, so a file staged
    // for one conversation is never sent to another.
    property string target: ""
    onTargetChanged: attachments = []

    // Attachments waiting for Enter: [{url, name, image}].
    property var attachments: []

    function addFiles(urls) {
        const added = []
        for (const u of urls) {
            const url = u.toString()
            if (url === "" || attachments.some(a => a.url === url) || added.some(a => a.url === url)) continue
            added.push({ url: url, name: decodeURIComponent(url.split("/").pop()), image: /\.(png|jpe?g|gif|webp|bmp)$/i.test(url) })
        }
        if (added.length === 0) return
        attachments = attachments.concat(added)
        input.forceActiveFocus()
    }

    function removeAttachment(index) {
        const rest = attachments.slice()
        rest.splice(index, 1)
        attachments = rest
    }

    Flow {
        Layout.fillWidth: true
        visible: composer.attachments.length > 0
        spacing: Kirigami.Units.smallSpacing
        Repeater {
            model: composer.attachments
            delegate: Kirigami.Chip {
                required property int index
                required property var modelData
                text: modelData.name.length > 40 ? modelData.name.substring(0, 37) + "…" : modelData.name
                icon.name: modelData.image ? "" : "mail-attachment"
                icon.source: modelData.image ? modelData.url : ""
                readonly property int iconSize: modelData.image ? Kirigami.Units.iconSizes.medium : Kirigami.Units.iconSizes.small
                icon.width: iconSize
                icon.height: iconSize
                closable: true
                onRemoved: composer.removeAttachment(index)
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing

        FileDialog {
            id: imagePicker
            title: qsTr("Choose a picture")
            fileMode: FileDialog.OpenFiles
            nameFilters: ["Immagini (*.png *.jpg *.jpeg *.gif *.webp *.bmp)"]
            onAccepted: composer.addFiles(selectedFiles)
        }

        FileDialog {
            id: filePicker
            title: qsTr("Choose a file to share")
            fileMode: FileDialog.OpenFiles
            onAccepted: composer.addFiles(selectedFiles)
        }

        // Emoji picker: common emoji, inserted at the cursor.
        QQC2.ToolButton {
            icon.name: "face-smile-big"
            enabled: composer.enabled
            onClicked: emojiPopup.open()
            QQC2.ToolTip.text: qsTr("Emoji")
            QQC2.ToolTip.visible: hovered
            QQC2.Popup {
                id: emojiPopup
                y: -height - Kirigami.Units.smallSpacing
                padding: Kirigami.Units.smallSpacing
                contentItem: Grid {
                    columns: 8
                    spacing: 2
                    Repeater {
                        model: ["😀","😂","😊","😉","😍","🤔","😎","😅","👍","👎","👏","🙏","💪","🙌","👋","🤝",
                                "❤️","🔥","✅","❌","⚠️","⭐","🎉","☕","😢","😡","😮","🤷","👀","💡","📌","📎"]
                        delegate: QQC2.ToolButton {
                            required property string modelData
                            text: modelData
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.3
                            onClicked: {
                                input.insert(input.cursorPosition, modelData)
                                emojiPopup.close()
                                input.forceActiveFocus()
                            }
                        }
                    }
                }
            }
        }
        QQC2.ToolButton {
            icon.name: "mail-attachment"
            enabled: composer.enabled && !backend.uploading
            onClicked: attachMenu.popup()
            QQC2.ToolTip.text: qsTr("Attach (pictures can also be pasted with Ctrl+V)")
            QQC2.ToolTip.visible: hovered
            QQC2.Menu {
                id: attachMenu
                QQC2.MenuItem { icon.name: "image-x-generic"; text: qsTr("Picture…"); onTriggered: imagePicker.open() }
                QQC2.MenuItem { icon.name: "document-open"; text: qsTr("File (OneDrive/SharePoint)…"); onTriggered: filePicker.open() }
            }
        }
        QQC2.BusyIndicator {
            visible: backend.uploading
            running: visible
            implicitWidth: Kirigami.Units.iconSizes.smallMedium
            implicitHeight: implicitWidth
        }
        QQC2.Label {
            visible: backend.uploadProgress >= 0
            text: Math.round(backend.uploadProgress * 100) + "%"
            font: Kirigami.Theme.smallFont
        }

        QQC2.Popup {
            id: mentionPopup
            y: -height - Kirigami.Units.smallSpacing
            width: Math.min(composer.width, Kirigami.Units.gridUnit * 20)
            padding: 0
            property var candidates: []
            contentItem: ListView {
                implicitHeight: Math.min(contentHeight, Kirigami.Units.gridUnit * 12)
                clip: true
                model: mentionPopup.candidates
                delegate: QQC2.ItemDelegate {
                    required property var modelData
                    width: ListView.view.width
                    text: modelData.name
                    onClicked: composer.insertMention(modelData)
                }
            }
        }

        QQC2.TextArea {
            id: input
            objectName: "composerInput"
            Layout.fillWidth: true
            Layout.maximumHeight: Kirigami.Units.gridUnit * 8
            wrapMode: TextEdit.Wrap
            onTextChanged: {
                if (text.length > 0) backend.userTyping()
                const m = composer.mentionQuery()
                if (!m) { mentionPopup.close(); return }
                const list = backend.currentMembers.filter(p => p.name.toLowerCase().indexOf(m.query) >= 0)
                mentionPopup.candidates = list
                if (list.length > 0) { if (!mentionPopup.opened) mentionPopup.open() } else mentionPopup.close()
            }
            Keys.onReturnPressed: event => {
                if (mentionPopup.opened && mentionPopup.candidates.length > 0) { composer.insertMention(mentionPopup.candidates[0]); return }
                if (event.modifiers & Qt.ShiftModifier) event.accepted = false
                else composer.submit()
            }
            Keys.onTabPressed: event => {
                if (mentionPopup.opened && mentionPopup.candidates.length > 0) composer.insertMention(mentionPopup.candidates[0])
                else event.accepted = false
            }
            Keys.onEscapePressed: event => {
                if (mentionPopup.opened) mentionPopup.close()
                else if (composer.attachments.length > 0) composer.attachments = []
                else event.accepted = false
            }
            Keys.onEnterPressed: event => composer.submit()
            // Ctrl+V with a picture in the clipboard attaches the picture.
            Keys.onPressed: event => {
                if (event.matches(StandardKey.Paste) && backend.clipboardHasImage()) {
                    const picture = backend.stageClipboardImage()
                    if (picture.toString() !== "") composer.addFiles([picture])
                    event.accepted = true
                }
            }
        }
        QQC2.Button {
            icon.name: "document-send"
            text: qsTr("Send")
            display: QQC2.AbstractButton.IconOnly
            enabled: input.text.trim().length > 0 || composer.attachments.length > 0
            onClicked: composer.submit()
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered
        }
    }
}
