// The message composer (qml/Composer.qml) in a QML engine with a stand-in backend:
// files wait as attachments until Enter, and never follow the user to another chat.
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest>

class BackendStub : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool uploading READ uploading CONSTANT)
    Q_PROPERTY(double uploadProgress READ uploadProgress CONSTANT)
    Q_PROPERTY(QVariantList currentMembers READ currentMembers CONSTANT)
public:
    bool uploading() const { return false; }
    double uploadProgress() const { return -1; }
    QVariantList currentMembers() const { return {}; }

    Q_INVOKABLE void userTyping() {}
    Q_INVOKABLE bool clipboardHasImage() const { return hasImage; }
    Q_INVOKABLE QUrl stageClipboardImage() { calls << "stage"; return QUrl::fromLocalFile("/tmp/image-pasted.png"); }
    Q_INVOKABLE void sendAttachments(const QVariantList &urls, bool inThread)
    {
        QStringList list;
        for (const QVariant &u : urls) list << u.toString();
        calls << QStringLiteral("attachments(%1;%2)").arg(list.join(','), inThread ? "thread" : "chat");
    }
    Q_INVOKABLE void sendMessageWithMentions(const QString &text, const QVariantList &, bool) { calls << "mentions:" + text; }

    bool hasImage = false;
    QStringList calls; // what the composer asked the backend to do, in order
};

class ComposerTest : public QObject
{
    Q_OBJECT
    QQmlEngine engine;
    BackendStub backend;
    QQuickWindow window;
    QQuickItem *composer = nullptr;
    QQmlComponent *component = nullptr;

    QVariantList attachments() const { return composer->property("attachments").toList(); }
    void add(const QStringList &urls)
    {
        QVariantList list;
        for (const QString &u : urls) list << QUrl(u);
        QVERIFY(QMetaObject::invokeMethod(composer, "addFiles", Q_ARG(QVariant, QVariant::fromValue(list))));
    }
    void typeInto(const QString &text) { QQuickItem *input = composer->findChild<QQuickItem *>("composerInput"); QVERIFY(input); input->setProperty("text", text); }

private slots:
    void initTestCase()
    {
        engine.rootContext()->setContextProperty("backend", &backend);
        component = new QQmlComponent(&engine, QUrl::fromLocalFile(QStringLiteral(KTEAMS_QML_DIR "/Composer.qml")));
        if (component->isError()) qWarning() << component->errors();
        QVERIFY2(component->isReady(), qPrintable(component->errorString()));
    }
    void cleanupTestCase() { delete component; }

    void init()
    {
        backend.calls.clear();
        backend.hasImage = false;
        composer = qobject_cast<QQuickItem *>(component->create());
        QVERIFY(composer);
        composer->setParentItem(window.contentItem());
        composer->setWidth(600);
        window.resize(640, 200);
        window.show();
    }
    void cleanup() { delete composer; composer = nullptr; }

    // --- staging ----------------------------------------------------------------------------

    void filesWaitAsAttachmentsWithTheirNames()
    {
        add({"file:///home/me/My%20Report.pdf", "file:///home/me/photo.JPG"});
        const QVariantList list = attachments();
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].toMap().value("name").toString(), QStringLiteral("My Report.pdf"));
        QCOMPARE(list[0].toMap().value("image").toBool(), false);
        QCOMPARE(list[1].toMap().value("image").toBool(), true); // by extension, any case
        QVERIFY(backend.calls.isEmpty()); // nothing is sent yet
    }

    void theSameFileIsStagedOnce()
    {
        add({"file:///a/x.txt", "file:///a/x.txt"});
        add({"file:///a/x.txt", "file:///a/y.txt"});
        QCOMPARE(attachments().size(), 2);
    }

    void anAttachmentCanBeRemoved()
    {
        add({"file:///a/1.txt", "file:///a/2.txt", "file:///a/3.txt"});
        QVERIFY(QMetaObject::invokeMethod(composer, "removeAttachment", Q_ARG(QVariant, 1)));
        const QVariantList list = attachments();
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].toMap().value("name").toString(), QStringLiteral("1.txt"));
        QCOMPARE(list[1].toMap().value("name").toString(), QStringLiteral("3.txt"));
    }

    void changingChatOrThreadDropsTheAttachments()
    {
        composer->setProperty("target", "chat-a");
        add({"file:///a/1.txt"});
        QCOMPARE(attachments().size(), 1);
        composer->setProperty("target", "chat-a"); // same chat: kept
        QCOMPARE(attachments().size(), 1);
        composer->setProperty("target", "chat-b");
        QCOMPARE(attachments().size(), 0);
    }

    // --- sending ----------------------------------------------------------------------------

    void enterWithOnlyAttachmentsSendsThem()
    {
        add({"file:///a/1.txt", "file:///a/2.png"});
        QSignalSpy sent(composer, SIGNAL(send(QString)));
        QVERIFY(QMetaObject::invokeMethod(composer, "submit"));
        QCOMPARE(sent.count(), 0); // no text, no message
        QCOMPARE(backend.calls, QStringList{"attachments(file:///a/1.txt,file:///a/2.png;chat)"});
        QCOMPARE(attachments().size(), 0);
    }

    void textGoesFirstThenTheAttachments()
    {
        add({"file:///a/1.txt"});
        typeInto("see attached");
        QSignalSpy sent(composer, SIGNAL(send(QString)));
        QVERIFY(QMetaObject::invokeMethod(composer, "submit"));
        QCOMPARE(sent.count(), 1);
        QCOMPARE(sent[0].at(0).toString(), QStringLiteral("see attached"));
        QCOMPARE(backend.calls, QStringList{"attachments(file:///a/1.txt;chat)"});
        QCOMPARE(composer->findChild<QQuickItem *>("composerInput")->property("text").toString(), QString()); // field cleared too
    }

    void inAThreadTheAttachmentsGoToTheThread()
    {
        composer->setProperty("inThread", true);
        add({"file:///a/1.txt"});
        QVERIFY(QMetaObject::invokeMethod(composer, "submit"));
        QCOMPARE(backend.calls, QStringList{"attachments(file:///a/1.txt;thread)"});
    }

    void enterWithNothingDoesNothing()
    {
        QSignalSpy sent(composer, SIGNAL(send(QString)));
        QVERIFY(QMetaObject::invokeMethod(composer, "submit"));
        typeInto("   ");
        QVERIFY(QMetaObject::invokeMethod(composer, "submit"));
        QCOMPARE(sent.count(), 0);
        QVERIFY(backend.calls.isEmpty());
    }

    // --- keyboard ---------------------------------------------------------------------------

    void escapeDiscardsTheAttachments()
    {
        add({"file:///a/1.txt"});
        QTRY_VERIFY(window.isExposed());
        QTest::keyClick(&window, Qt::Key_Escape);
        QCOMPARE(attachments().size(), 0);
        QVERIFY(backend.calls.isEmpty()); // discarded, not sent
    }

    void aPastedPictureBecomesAnAttachment()
    {
        backend.hasImage = true;
        add({"file:///a/1.txt"}); // gives the field the focus
        QTRY_VERIFY(window.isExposed());
        QTest::keyClick(&window, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(backend.calls, QStringList{"stage"});
        const QVariantList list = attachments();
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[1].toMap().value("name").toString(), QStringLiteral("image-pasted.png"));
        QCOMPARE(list[1].toMap().value("image").toBool(), true);
    }

    // --- looks ------------------------------------------------------------------------------

    void theAttachmentsAreDrawn()
    {
        QTemporaryDir dir;
        QImage picture(200, 120, QImage::Format_RGB32);
        picture.fill(QColor(0x3d, 0xae, 0xe9));
        const QString imagePath = dir.filePath("holiday photo.png");
        QVERIFY(picture.save(imagePath));
        QFile pdf(dir.filePath("Contract 2026.pdf"));
        QVERIFY(pdf.open(QIODevice::WriteOnly));
        add({QUrl::fromLocalFile(imagePath).toString(), QUrl::fromLocalFile(pdf.fileName()).toString()});
        typeInto("Here are the files");
        window.resize(640, 140);
        QTRY_VERIFY(window.isExposed());
        QTest::qWait(300); // layout and the thumbnail load
        const QImage shot = window.grabWindow();
        QVERIFY(!shot.isNull());
        // The attachments take room above the text field.
        QQuickItem *input = composer->findChild<QQuickItem *>("composerInput");
        QVERIFY(input->mapToItem(composer, QPointF(0, 0)).y() > 20);
        if (const QByteArray out = qgetenv("KTEAMS_TEST_SCREENSHOT"); !out.isEmpty()) shot.save(QString::fromLocal8Bit(out));
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QGuiApplication app(argc, argv);
    ComposerTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_composer.moc"
