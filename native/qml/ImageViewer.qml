import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// A picture from a chat shown inside the app, over the window: wheel or buttons to zoom,
// drag to move, double click to switch between fit and full size, Esc or a click outside to close.
QQC2.Popup {
    id: viewer
    property string source          // the local file URL
    property string remoteUrl       // the picture's address in Teams, for "Save image"
    property real zoom: 1.0         // 1.0 = fits the window
    readonly property real minZoom: 1.0
    readonly property real maxZoom: 8.0

    parent: QQC2.Overlay.overlay
    anchors.centerIn: parent
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    modal: true
    padding: 0
    closePolicy: QQC2.Popup.CloseOnEscape
    background: Rectangle { color: Qt.rgba(0, 0, 0, 0.85) }

    function show(url, remote) {
        source = url
        remoteUrl = remote || ""
        zoom = 1.0
        open()
    }
    function setZoom(value, cx, cy) {
        const old = zoom
        zoom = Math.max(minZoom, Math.min(maxZoom, value))
        if (zoom === old) return
        // Keep the point under the cursor (cx, cy in the view) where it was.
        const ratio = zoom / old
        flick.contentX = (flick.contentX + cx) * ratio - cx
        flick.contentY = (flick.contentY + cy) * ratio - cy
        flick.returnToBounds()
    }

    focus: true
    contentItem: Item {
        focus: true
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) { viewer.setZoom(viewer.zoom * 1.4, flick.width / 2, flick.height / 2); event.accepted = true }
            else if (event.key === Qt.Key_Minus) { viewer.setZoom(viewer.zoom / 1.4, flick.width / 2, flick.height / 2); event.accepted = true }
            else if (event.key === Qt.Key_0) { viewer.zoom = 1.0; event.accepted = true }
        }
        Flickable {
            id: flick
            anchors.fill: parent
            clip: true
            contentWidth: Math.max(width, picture.width)
            contentHeight: Math.max(height, picture.height)
            boundsBehavior: Flickable.StopAtBounds
            interactive: viewer.zoom > 1.0

            Image {
                id: picture
                objectName: "viewerPicture"
                readonly property real fitScale: status === Image.Ready
                    ? Math.min(flick.width / implicitWidth, flick.height / implicitHeight, 1) : 1
                width: implicitWidth * fitScale * viewer.zoom
                height: implicitHeight * fitScale * viewer.zoom
                x: Math.max(0, (flick.contentWidth - width) / 2)
                y: Math.max(0, (flick.contentHeight - height) / 2)
                source: viewer.source
                fillMode: Image.PreserveAspectFit
                smooth: true
                asynchronous: true
            }
            QQC2.BusyIndicator { anchors.centerIn: parent; running: picture.status === Image.Loading; visible: running }
        }

        // Wheel zooms around the cursor; a click outside the picture closes; double click toggles.
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: event => viewer.setZoom(viewer.zoom * (event.angleDelta.y > 0 ? 1.15 : 1 / 1.15), point.position.x, point.position.y)
        }
        TapHandler {
            onTapped: (point, button) => {
                const p = picture.mapFromItem(null, point.scenePosition.x, point.scenePosition.y)
                if (p.x < 0 || p.y < 0 || p.x > picture.width || p.y > picture.height) viewer.close()
            }
            onDoubleTapped: (point, button) => viewer.setZoom(viewer.zoom > 1.01 ? 1.0 : 2.5, point.position.x, point.position.y)
        }

        RowLayout {
            anchors { top: parent.top; right: parent.right; margins: Kirigami.Units.largeSpacing }
            spacing: Kirigami.Units.smallSpacing
            QQC2.ToolButton {
                icon.name: "zoom-out"
                enabled: viewer.zoom > viewer.minZoom
                onClicked: viewer.setZoom(viewer.zoom / 1.4, flick.width / 2, flick.height / 2)
                QQC2.ToolTip.text: qsTr("Zoom out"); QQC2.ToolTip.visible: hovered
            }
            QQC2.ToolButton {
                icon.name: "zoom-in"
                enabled: viewer.zoom < viewer.maxZoom
                onClicked: viewer.setZoom(viewer.zoom * 1.4, flick.width / 2, flick.height / 2)
                QQC2.ToolTip.text: qsTr("Zoom in"); QQC2.ToolTip.visible: hovered
            }
            QQC2.ToolButton {
                icon.name: "zoom-fit-best"
                onClicked: viewer.zoom = 1.0
                QQC2.ToolTip.text: qsTr("Fit to window"); QQC2.ToolTip.visible: hovered
            }
            QQC2.ToolButton {
                icon.name: "document-save"
                visible: viewer.remoteUrl !== ""
                onClicked: backend.saveImage(viewer.remoteUrl)
                QQC2.ToolTip.text: qsTr("Save image"); QQC2.ToolTip.visible: hovered
            }
            QQC2.ToolButton {
                icon.name: "window-close"
                onClicked: viewer.close()
                QQC2.ToolTip.text: qsTr("Close"); QQC2.ToolTip.visible: hovered
            }
        }
    }
}
