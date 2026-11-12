import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtMultimedia
import org.kde.kirigami as Kirigami

// The video of a native call: the other side full size, our camera in a corner. The call
// controls stay in the window's footer bar.
Kirigami.Page {
    id: page
    objectName: "callPage"
    title: backend.nativeCallState
    padding: 0

    Rectangle {
        anchors.fill: parent
        color: "#1b1b1b"

        // A shared screen takes the stage; the remote video then moves to a corner.
        VideoOutput {
            id: remoteShare
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectFit
            visible: backend.remoteShare
        }

        VideoOutput {
            id: remoteVideo
            x: backend.remoteShare ? Kirigami.Units.largeSpacing : 0
            y: backend.remoteShare ? parent.height - height - Kirigami.Units.largeSpacing : 0
            width: backend.remoteShare ? Math.round(parent.width / 4) : parent.width
            height: backend.remoteShare ? Math.round(width * 9 / 16) : parent.height
            fillMode: VideoOutput.PreserveAspectFit
            visible: backend.remoteVideo
        }

        ColumnLayout {
            anchors.centerIn: parent
            visible: !backend.remoteVideo && !backend.remoteShare
            spacing: Kirigami.Units.largeSpacing
            Kirigami.Icon {
                Layout.alignment: Qt.AlignHCenter
                source: "camera-off"
                color: "white"
                implicitWidth: Kirigami.Units.iconSizes.huge
                implicitHeight: implicitWidth
            }
            QQC2.Label {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("The other person is not sending video")
                color: "white"
            }
        }

        VideoOutput {
            id: localVideo
            width: Math.round(parent.width / 4)
            height: Math.round(width * 9 / 16)
            anchors { right: parent.right; bottom: parent.bottom; margins: Kirigami.Units.largeSpacing }
            fillMode: VideoOutput.PreserveAspectCrop
            visible: backend.cameraOn
        }
    }

    Component.onCompleted: backend.setVideoSinks(remoteVideo.videoSink, localVideo.videoSink, remoteShare.videoSink)
    Component.onDestruction: backend.setVideoSinks(null, null, null)
}
