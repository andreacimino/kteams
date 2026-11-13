// The picture viewer (qml/ImageViewer.qml) in a QML engine with a stand-in backend: it opens over
// the window with the picture fitted, zooms inside its limits, goes back to "fit", and closes.
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>

class BackendStub : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE void saveImage(const QString &url) { saved << url; }
    QStringList saved;
};

class ImageViewerTest : public QObject
{
    Q_OBJECT
    QQmlEngine engine;
    BackendStub backend;
    QQuickWindow window;
    QTemporaryDir dir;
    QString imageUrl;
    QObject *viewer = nullptr;
    QQmlComponent *component = nullptr;

    QQuickItem *picture() { QQuickItem *p = viewer->findChild<QQuickItem *>(QStringLiteral("viewerPicture")); Q_ASSERT(p); return p; }
    double zoom() const { return viewer->property("zoom").toDouble(); }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        QImage image(800, 400, QImage::Format_RGB32);
        image.fill(Qt::darkGreen);
        const QString path = dir.filePath(QStringLiteral("picture.png"));
        QVERIFY(image.save(path));
        imageUrl = QUrl::fromLocalFile(path).toString();
        engine.rootContext()->setContextProperty("backend", &backend);
        component = new QQmlComponent(&engine, QUrl::fromLocalFile(QStringLiteral(KTEAMS_QML_DIR "/ImageViewer.qml")));
        QVERIFY2(component->isReady(), qPrintable(component->errorString()));
        window.resize(400, 300);
        window.show();
    }
    void cleanupTestCase() { delete component; }

    void init()
    {
        viewer = component->create();
        QVERIFY(viewer);
        viewer->setParent(&engine);
        viewer->setProperty("parent", QVariant::fromValue(window.contentItem()));
        backend.saved.clear();
    }
    void cleanup() { delete viewer; viewer = nullptr; }

    void openFitsThePictureInTheWindowAtFirst()
    {
        QMetaObject::invokeMethod(viewer, "show", Q_ARG(QVariant, imageUrl), Q_ARG(QVariant, QStringLiteral("https://x/img")));
        QTRY_VERIFY(viewer->property("opened").toBool());
        QCOMPARE(zoom(), 1.0);
        QTRY_COMPARE(picture()->property("status").toInt(), 1); // Image.Ready
        // 800x400 into 400x300: scaled to 400x200, never beyond its own size.
        QTRY_COMPARE(picture()->width(), 400.0);
        QCOMPARE(picture()->height(), 200.0);
    }
    void zoomingStaysBetweenFitAndEightTimes()
    {
        QMetaObject::invokeMethod(viewer, "show", Q_ARG(QVariant, imageUrl), Q_ARG(QVariant, QString()));
        QTRY_VERIFY(viewer->property("opened").toBool());
        QMetaObject::invokeMethod(viewer, "setZoom", Q_ARG(QVariant, 0.2), Q_ARG(QVariant, 0), Q_ARG(QVariant, 0));
        QCOMPARE(zoom(), 1.0);
        QMetaObject::invokeMethod(viewer, "setZoom", Q_ARG(QVariant, 100.0), Q_ARG(QVariant, 0), Q_ARG(QVariant, 0));
        QCOMPARE(zoom(), 8.0);
        QMetaObject::invokeMethod(viewer, "setZoom", Q_ARG(QVariant, 2.0), Q_ARG(QVariant, 200), Q_ARG(QVariant, 150));
        QCOMPARE(zoom(), 2.0);
        QTRY_COMPARE(picture()->width(), 800.0);
    }
    void showingAnotherPictureStartsFitted()
    {
        QMetaObject::invokeMethod(viewer, "show", Q_ARG(QVariant, imageUrl), Q_ARG(QVariant, QString()));
        QMetaObject::invokeMethod(viewer, "setZoom", Q_ARG(QVariant, 3.0), Q_ARG(QVariant, 0), Q_ARG(QVariant, 0));
        QCOMPARE(zoom(), 3.0);
        QMetaObject::invokeMethod(viewer, "show", Q_ARG(QVariant, imageUrl), Q_ARG(QVariant, QString()));
        QCOMPARE(zoom(), 1.0);
    }
    void thePlusMinusAndZeroKeysZoom()
    {
        QMetaObject::invokeMethod(viewer, "show", Q_ARG(QVariant, imageUrl), Q_ARG(QVariant, QString()));
        QTRY_VERIFY(viewer->property("opened").toBool());
        QTest::keyClick(&window, Qt::Key_Plus);
        QVERIFY(zoom() > 1.3);
        QTest::keyClick(&window, Qt::Key_Minus);
        QCOMPARE(zoom(), 1.0);
        QTest::keyClick(&window, Qt::Key_Plus);
        QTest::keyClick(&window, Qt::Key_Plus);
        QTest::keyClick(&window, Qt::Key_0);
        QCOMPARE(zoom(), 1.0);
    }
    void escapeClosesIt()
    {
        QMetaObject::invokeMethod(viewer, "show", Q_ARG(QVariant, imageUrl), Q_ARG(QVariant, QString()));
        QTRY_VERIFY(viewer->property("opened").toBool());
        QTest::keyClick(&window, Qt::Key_Escape);
        QTRY_VERIFY(!viewer->property("visible").toBool());
    }
};

QTEST_MAIN(ImageViewerTest)
#include "tst_imageviewer.moc"
