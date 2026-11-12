import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// A Teams meeting in the calendar: title, day and time from lists, length, and the people
// picked by searching them (as in the new-chat dialog).
Kirigami.Dialog {
    id: dialog
    title: qsTr("New meeting")
    preferredWidth: Kirigami.Units.gridUnit * 28
    preferredHeight: Kirigami.Units.gridUnit * 30
    standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel

    // [{name, email}] chosen so far.
    property var invited: []
    property var days: []   // [{text, date}]
    property var times: []  // ["08:00", "08:30", ...]

    function pad(n) { return (n < 10 ? "0" : "") + n }
    onOpened: {
        const now = new Date()
        const d = []
        for (let i = 0; i < 14; ++i) {
            const date = new Date(now.getFullYear(), now.getMonth(), now.getDate() + i)
            const label = i === 0 ? qsTr("Today") : (i === 1 ? qsTr("Tomorrow") : Qt.formatDate(date, "ddd d MMM"))
            d.push({ text: label, date: date })
        }
        days = d
        const t = []
        for (let h = 7; h <= 20; ++h) { t.push(pad(h) + ":00"); t.push(pad(h) + ":30") }
        times = t
        subject.text = ""
        day.currentIndex = 0
        const nextSlot = pad(now.getHours() + (now.getMinutes() >= 30 ? 1 : 0)) + ":" + (now.getMinutes() >= 30 ? "00" : "30")
        startTime.currentIndex = Math.max(0, times.indexOf(nextSlot))
        length.currentIndex = 1
        invited = []
        search.text = ""
        subject.forceActiveFocus()
    }
    onAccepted: {
        const date = days[day.currentIndex].date
        const h = times[startTime.currentIndex].split(":")
        const start = new Date(date.getFullYear(), date.getMonth(), date.getDate(), parseInt(h[0]), parseInt(h[1]), 0, 0)
        const end = new Date(start.getTime() + length.model[length.currentIndex].minutes * 60000)
        backend.createMeeting(subject.text, start, end, invited.map(p => p.email))
    }

    Timer {
        id: debounce
        interval: 300
        onTriggered: backend.searchPeople(search.text)
    }

    ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        Kirigami.FormLayout {
            Layout.fillWidth: true
            QQC2.TextField { id: subject; Kirigami.FormData.label: "Titolo:"; placeholderText: qsTr("Meeting subject") }
            RowLayout {
                Kirigami.FormData.label: "Quando:"
                QQC2.ComboBox { id: day; model: dialog.days; textRole: "text"; Layout.preferredWidth: Kirigami.Units.gridUnit * 9 }
                QQC2.ComboBox { id: startTime; model: dialog.times; Layout.preferredWidth: Kirigami.Units.gridUnit * 6 }
                QQC2.ComboBox {
                    id: length
                    textRole: "text"
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 8
                    model: [{ text: qsTr("30 minutes"), minutes: 30 }, { text: qsTr("1 hour"), minutes: 60 }, { text: qsTr("1.5 hours"), minutes: 90 }, { text: qsTr("2 hours"), minutes: 120 }]
                }
            }
        }

        Kirigami.Separator { Layout.fillWidth: true }

        // Who is invited: click a name to remove it.
        QQC2.Label { text: qsTr("Invited"); font.bold: true }
        Flow {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            Repeater {
                model: dialog.invited
                QQC2.Button {
                    required property var modelData
                    required property int index
                    text: modelData.name
                    icon.name: "list-remove"
                    flat: true
                    onClicked: { const v = dialog.invited.slice(); v.splice(index, 1); dialog.invited = v }
                }
            }
            QQC2.Label { visible: dialog.invited.length === 0; text: qsTr("Nobody yet: search people below"); color: Kirigami.Theme.disabledTextColor }
        }

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
            Layout.preferredHeight: Kirigami.Units.gridUnit * 10
            clip: true
            model: search.text.length >= 1 ? backend.peopleResults : []
            delegate: QQC2.ItemDelegate {
                id: person
                required property var modelData
                width: ListView.view.width
                enabled: modelData.email !== "" && !dialog.invited.some(p => p.email === modelData.email)
                onClicked: {
                    dialog.invited = dialog.invited.concat([{ name: modelData.name, email: modelData.email }])
                    search.text = ""
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
