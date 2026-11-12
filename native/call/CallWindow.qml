import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtWebEngine

ApplicationWindow {
    id: window
    width: 1100
    height: 750
    visible: startVisible
    title: (web.title || "Chiamata") + " — KTeams"
    readonly property double startedAt: Date.now()

    // A call or meeting handed over by the main app (CallController::open).
    property url target
    onTargetChanged: if (target.toString() !== "") web.url = target

    // Closing ends the call (the page goes back home) and hides the window; Teams stays
    // loaded, so the next call starts at once and incoming calls can ring here.
    onClosing: close => {
        close.accepted = false
        web.url = controller.homeUrl
        window.hide()
        controller.windowHidden()
    }

    // One signed-in web session for the main window and the windows Teams opens.
    WebEngineProfile {
        id: teamsProfile
        storageName: "kteams-auth"
        persistentStoragePath: profilePath   // set by main.cpp
        offTheRecord: false
        spellCheckEnabled: false
        httpUserAgent: "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36"
    }

    // The Teams web view, with what a call needs and nothing else. Used by the main window
    // and by every window Teams opens (calls and meetings often run in one).
    component CallView: WebEngineView {
        id: view
        profile: teamsProfile
        settings.pluginsEnabled: false
        settings.pdfViewerEnabled: false
        settings.spatialNavigationEnabled: false
        settings.dnsPrefetchEnabled: false
        settings.autoLoadIconsForPage: false
        settings.javascriptCanOpenWindows: true       // Teams opens calls in their own window
        settings.playbackRequiresUserGesture: false   // the ringtone must play by itself

        // --capture: the protocol recorder runs before the page's own scripts, in every frame.
        userScripts.collection: captureLog ? [{
            name: "ac-capture",
            sourceUrl: Qt.resolvedUrl("qrc:/call/capture.js"),
            injectionPoint: WebEngineScript.DocumentCreation,
            worldId: WebEngineScript.MainWorld,
            runsOnSubFrames: true
        }] : []
        onJavaScriptConsoleMessage: (level, message, lineNumber, sourceID) => {
            if (captureLog && message.startsWith("ACCAPTURE ")) captureLog.page(message.substring(10))
        }

        onPermissionRequested: permission => policy.decide(permission)
        onNewWindowRequested: request => {
            if (!policy.keepInWindow(request.requestedUrl)) return
            // A real window, linked to its opener (Teams talks to it).
            const popup = popupComponent.createObject(window)
            request.openIn(popup.view)
        }
        onNavigationRequested: request => {
            if (!request.isMainFrame) return
            if (!policy.keepInWindow(request.url)) request.reject()
        }
        // Screen sharing: let the user pick a screen or a window.
        onDesktopMediaRequested: request => {
            sharePicker.request = request
            sharePicker.open()
        }
    }

    CallView {
        id: web
        objectName: "web"   // main.cpp attaches the request filter to its profile
        anchors.fill: parent
        url: startUrl

        // Host only: tells whether the saved session was enough or a sign-in is needed.
        onLoadingChanged: request => {
            if (request.status === WebEngineView.LoadSucceededStatus)
                console.info("call window loaded:", new URL(request.url).host, "after", Date.now() - window.startedAt, "ms")
        }
        onFullScreenRequested: request => {
            request.accept()
            if (request.toggleOn) window.showFullScreen()
            else window.showNormal()
        }
    }

    Component {
        id: popupComponent
        ApplicationWindow {
            id: popup
            property alias view: popupView
            width: 1000
            height: 700
            visible: true
            title: (popupView.title || "Chiamata") + " — KTeams"
            onClosing: popup.destroy()
            CallView {
                id: popupView
                anchors.fill: parent
                onWindowCloseRequested: popup.close()
                onFullScreenRequested: request => {
                    request.accept()
                    if (request.toggleOn) popup.showFullScreen()
                    else popup.showNormal()
                }
            }
        }
    }

    Dialog {
        id: sharePicker
        property var request
        anchors.centerIn: parent
        modal: true
        title: "Cosa vuoi condividere?"
        standardButtons: Dialog.Cancel
        onRejected: if (request) request.cancel()

        ColumnLayout {
            Label { text: "Schermi"; font.bold: true }
            Repeater {
                model: sharePicker.request ? sharePicker.request.screensModel : null
                delegate: Button {
                    required property int index
                    required property var model
                    Layout.fillWidth: true
                    text: model.display
                    onClicked: {
                        sharePicker.request.selectScreen(sharePicker.request.screensModel.index(index, 0))
                        sharePicker.request = undefined
                        sharePicker.close()
                    }
                }
            }
            Label { text: "Finestre"; font.bold: true }
            Repeater {
                model: sharePicker.request ? sharePicker.request.windowsModel : null
                delegate: Button {
                    required property int index
                    required property var model
                    Layout.fillWidth: true
                    text: model.display
                    onClicked: {
                        sharePicker.request.selectWindow(sharePicker.request.windowsModel.index(index, 0))
                        sharePicker.request = undefined
                        sharePicker.close()
                    }
                }
            }
        }
    }
}
