// The settings page (qml/SettingsPage.qml) in a QML engine with a stand-in backend: each list
// starts with the system default, shows the stored choice, and choosing writes it.
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>

class BackendStub : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE QVariantList mediaDevices(const QString &kind) const
    {
        asked << kind;
        QVariantList list{QVariantMap{{"id", ""}, {"name", "System default"}}};
        if (kind == "audioInput") list << QVariantMap{{"id", "mic.usb"}, {"name", "USB Microphone"}} << QVariantMap{{"id", "mic.builtin"}, {"name", "Built-in Microphone"}};
        else if (kind == "audioOutput") list << QVariantMap{{"id", "spk.headset"}, {"name", "Headset"}};
        else if (kind == "camera") list << QVariantMap{{"id", "/dev/video0"}, {"name", "Integrated Camera"}} << QVariantMap{{"id", "/dev/video2"}, {"name", "USB Camera"}};
        return list;
    }
    Q_INVOKABLE QString selectedDevice(const QString &kind) const { return chosen.value(kind); }
    Q_INVOKABLE void selectDevice(const QString &kind, const QString &id) { chosen.insert(kind, id); changes << kind + "=" + id; }

    QMap<QString, QString> chosen;
    QStringList changes;
    mutable QStringList asked;
};

class SettingsPageTest : public QObject
{
    Q_OBJECT
    QQmlEngine engine;
    BackendStub backend;
    QQuickWindow window;
    QQmlComponent *component = nullptr;
    QQuickItem *page = nullptr;

    QObject *combo(const QString &name) { QObject *c = page->findChild<QObject *>(name); Q_ASSERT(c); return c; }

private slots:
    void initTestCase()
    {
        engine.rootContext()->setContextProperty("backend", &backend);
        component = new QQmlComponent(&engine, QUrl::fromLocalFile(QStringLiteral(KTEAMS_QML_DIR "/SettingsPage.qml")));
        if (component->isError()) qWarning() << component->errors();
        QVERIFY2(component->isReady(), qPrintable(component->errorString()));
    }
    void cleanupTestCase() { delete component; }

    void init()
    {
        backend.chosen.clear();
        backend.changes.clear();
        backend.asked.clear();
    }
    void cleanup() { delete page; page = nullptr; }

    void open()
    {
        page = qobject_cast<QQuickItem *>(component->create());
        QVERIFY(page);
        page->setParentItem(window.contentItem());
        page->setSize(QSizeF(600, 400));
        window.resize(640, 440);
        window.show();
    }

    void everyListStartsWithTheSystemDefaultAndShowsItWhenNothingIsChosen()
    {
        open();
        QStringList asked = backend.asked;
        asked.sort(); // QML completes sibling items in reverse order
        QCOMPARE(asked, (QStringList{"audioInput", "audioOutput", "camera"}));
        for (const char *name : {"audioInputCombo", "audioOutputCombo", "cameraCombo"}) {
            QObject *c = combo(name);
            QCOMPARE(c->property("currentIndex").toInt(), 0);
            QCOMPARE(c->property("count").toInt() >= 2, true);
            QCOMPARE(c->property("currentText").toString(), QStringLiteral("System default"));
        }
        QCOMPARE(combo("audioInputCombo")->property("count").toInt(), 3);
        QCOMPARE(combo("cameraCombo")->property("count").toInt(), 3);
    }

    void aStoredChoiceIsSelectedWhenThePageOpens()
    {
        backend.chosen["audioInput"] = "mic.builtin";
        backend.chosen["camera"] = "/dev/video2";
        open();
        QCOMPARE(combo("audioInputCombo")->property("currentText").toString(), QStringLiteral("Built-in Microphone"));
        QCOMPARE(combo("cameraCombo")->property("currentText").toString(), QStringLiteral("USB Camera"));
        QCOMPARE(combo("audioOutputCombo")->property("currentText").toString(), QStringLiteral("System default"));
        QVERIFY(backend.changes.isEmpty()); // opening the page changes nothing
    }

    void aStoredChoiceThatIsUnpluggedShowsTheDefaultWithoutForgettingIt()
    {
        backend.chosen["audioInput"] = "mic.unplugged";
        open();
        QCOMPARE(combo("audioInputCombo")->property("currentText").toString(), QStringLiteral("System default"));
        QCOMPARE(backend.chosen.value("audioInput"), QStringLiteral("mic.unplugged"));
        QVERIFY(backend.changes.isEmpty());
    }

    void choosingADeviceWritesTheChoice()
    {
        open();
        QObject *microphone = combo("audioInputCombo");
        // A click on the second entry sets the index, then emits activated().
        microphone->setProperty("currentIndex", 1);
        QVERIFY(QMetaObject::invokeMethod(microphone, "activated", Q_ARG(int, 1)));
        QVERIFY(backend.changes.contains("audioInput=mic.usb"));
        microphone->setProperty("currentIndex", 0);
        QVERIFY(QMetaObject::invokeMethod(microphone, "activated", Q_ARG(int, 0)));
        QCOMPARE(backend.chosen.value("audioInput"), QString()); // back to the default
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QGuiApplication app(argc, argv);
    SettingsPageTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_settingspage.moc"
