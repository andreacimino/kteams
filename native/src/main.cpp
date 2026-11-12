#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QMutex>
#include <QLocale>
#include <QStandardPaths>
#include <QTimer>
#include <QTranslator>
#include <QQuickWindow>

#include "backend.h"
#include "desktopintegration.h"

// Everything logged also goes to ~/.cache/kteams/kteams/kteams.log (kept
// small: rotated at 5 MB, one older copy), so a run from the menu can be looked at too.
static void installFileLog()
{
    static QFile *file = nullptr;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/kteams.log");
    if (QFileInfo(path).size() > 5 * 1024 * 1024) { QFile::remove(path + QStringLiteral(".1")); QFile::rename(path, path + QStringLiteral(".1")); }
    file = new QFile(path);
    if (!file->open(QIODevice::Append | QIODevice::Text)) return;
    static QtMessageHandler previous = nullptr;
    static QMutex mutex;
    previous = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &context, const QString &message) {
        {
            QMutexLocker lock(&mutex);
            const QString line = QStringLiteral("%1 %2: %3\n").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                                                                  QString::fromLatin1(context.category ? context.category : "default"), message);
            file->write(line.toUtf8());
            file->flush();
        }
        if (previous) previous(type, context, message);
    });
}

int main(int argc, char *argv[])
{
    // QApplication (not QGuiApplication): the KDE desktop style draws controls through QStyle.
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("kteams"));
    QCoreApplication::setApplicationName(QStringLiteral("kteams"));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.kteams.kteams"));
    installFileLog();
    // Interface language: the system's, when a translation exists (English otherwise).
    QTranslator translator;
    if (translator.load(QLocale(), QStringLiteral("kteams"), QStringLiteral("_"), QStringLiteral(":/i18n")))
        QCoreApplication::installTranslator(&translator);
    // The task bar finds the icon through the desktop entry (installed); the embedded one
    // covers a build run from the checkout.
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("org.kteams.kteams"), QIcon(QStringLiteral(":/KTeams/icon.svg"))));

    // Native Breeze look on Plasma; QT_QUICK_CONTROLS_STYLE still overrides it.
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));

    qmlRegisterUncreatableType<Backend>("KTeams", 1, 0, "Backend", QStringLiteral("Provided by the application"));

    Backend backend;
    DesktopIntegration desktop;
    // Closing the window keeps the app in the tray (notifications and push keep working).
    app.setQuitOnLastWindowClosed(false);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    engine.rootContext()->setContextProperty(QStringLiteral("desktop"), &desktop);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.load(QUrl(QStringLiteral("qrc:/KTeams/qml/Main.qml")));
    if (!engine.rootObjects().isEmpty())
        desktop.setWindow(qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()));
    QObject::connect(&backend, &Backend::unreadCountChanged, &desktop, [&] { desktop.setUnreadCount(backend.unreadCount()); });
    // Screenshot helper (demo mode): KTEAMS_SCREENSHOT=<file.png> saves the window after
    // KTEAMS_SCREENSHOT_DELAY seconds (default 20) and quits.
    if (qEnvironmentVariableIsSet("KTEAMS_SCREENSHOT") && !engine.rootObjects().isEmpty()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        const int delay = qEnvironmentVariableIsSet("KTEAMS_SCREENSHOT_DELAY") ? qEnvironmentVariableIntValue("KTEAMS_SCREENSHOT_DELAY") : 20;
        const QString file = qEnvironmentVariable("KTEAMS_SCREENSHOT");
        if (window) { window->setWidth(1280); window->setHeight(800); }
        QTimer::singleShot(std::chrono::seconds(delay), &app, [window, file] {
            if (window) window->grabWindow().save(file);
            QCoreApplication::quit();
        });
    }
    // Own presence on the tray icon; the tray's status menu sets it.
    QObject::connect(&backend, &Backend::presenceChanged, &desktop, [&] { desktop.setPresence(backend.presenceOf(backend.property("selfMriValue").toString())); });
    QObject::connect(&backend, &Backend::selfChanged, &desktop, [&] { desktop.setPresence(backend.presenceOf(backend.property("selfMriValue").toString())); });
    QObject::connect(&desktop, &DesktopIntegration::availabilityRequested, &backend, &Backend::setMyAvailability);

    return app.exec();
}
