import QtQuick
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.components as Components

// Profile picture (or initials) with the availability dot, as in Teams.
Item {
    id: root
    property string name
    property string mri   // empty: no picture, no presence (group chats)

    readonly property string presence: { backend.presenceRevision; return mri ? backend.presenceOf(mri) : "" }

    function presenceColor(p) {
        switch (p) {
        case "Available": case "AvailableIdle": return Kirigami.Theme.positiveTextColor
        case "Busy": case "BusyIdle": case "DoNotDisturb": case "InACall": case "InAConferenceCall": case "Presenting":
            return Kirigami.Theme.negativeTextColor
        case "Away": case "BeRightBack": return Kirigami.Theme.neutralTextColor
        default: return Kirigami.Theme.disabledTextColor
        }
    }

    Components.Avatar {
        anchors.fill: parent
        name: root.name
        source: { backend.avatarRevision; return root.mri ? backend.avatarUrl(root.mri) : "" }
    }

    Rectangle {
        visible: root.presence !== "" && root.presence !== "PresenceUnknown"
        width: Math.max(8, root.width * 0.3)
        height: width
        radius: width / 2
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        color: root.presenceColor(root.presence)
        border.width: 2
        border.color: Kirigami.Theme.backgroundColor
    }
}
