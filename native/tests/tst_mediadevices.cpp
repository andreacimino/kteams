// Microphone, speaker and camera choice (src/mediadevices.cpp): the camera list from a fake
// sysfs tree, the choice kept in the settings, and the quoting for the pipeline checked against
// GStreamer's own parser.
#include "mediadevices.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <gst/gst.h>

using namespace MediaDevices;

namespace {
void makeNode(const QDir &root, const QString &node, const QString &name, const QString &index)
{
    QVERIFY(root.mkpath(node));
    if (!name.isNull()) { QFile f(root.filePath(node + "/name")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(name.toUtf8() + "\n"); }
    if (!index.isNull()) { QFile f(root.filePath(node + "/index")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(index.toUtf8() + "\n"); }
}
}

class MediaDevicesTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { gst_init(nullptr, nullptr); }
    void init() { QSettings().clear(); }

    // --- cameras ----------------------------------------------------------------------------

    void onlyTheCaptureNodesOfACameraAreListed()
    {
        QTemporaryDir sys;
        const QDir root(sys.path());
        makeNode(root, "video0", "Integrated RGB Camera: Integrat", "0");
        makeNode(root, "video1", "Integrated RGB Camera: Integrat", "1"); // its metadata node
        makeNode(root, "video2", "USB Camera", "0");
        makeNode(root, "video3", "USB Camera", "1");
        const QList<Device> list = cameras(sys.path(), "/dev");
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].id, QStringLiteral("/dev/video0"));
        QCOMPARE(list[0].name, QStringLiteral("Integrated RGB Camera: Integrat"));
        QCOMPARE(list[1].id, QStringLiteral("/dev/video2"));
    }

    void camerasAreOrderedByNumberAndTwinsAreToldApart()
    {
        QTemporaryDir sys;
        const QDir root(sys.path());
        makeNode(root, "video10", "Capture card", "0");
        makeNode(root, "video2", "Webcam", "0");
        makeNode(root, "video4", "Webcam", "0");
        makeNode(root, "video9", "Old camera", QString()); // no index file: taken as a capture node
        const QList<Device> list = cameras(sys.path(), "/devices");
        QStringList ids;
        for (const Device &d : list) ids << d.id;
        QCOMPARE(ids, QStringList({"/devices/video2", "/devices/video4", "/devices/video9", "/devices/video10"}));
        QCOMPARE(list[0].name, QStringLiteral("Webcam (/devices/video2)")); // same name twice: the path is added
        QCOMPARE(list[1].name, QStringLiteral("Webcam (/devices/video4)"));
        QCOMPARE(list[2].name, QStringLiteral("Old camera"));
    }

    void aNodeWithoutANameIsShownByItsNodeName()
    {
        QTemporaryDir sys;
        makeNode(QDir(sys.path()), "video0", QString(), "0");
        QCOMPARE(cameras(sys.path(), "/dev").value(0).name, QStringLiteral("video0"));
    }

    void noSysfsMeansNoCameras()
    {
        QVERIFY(cameras("/nonexistent/path", "/dev").isEmpty());
    }

    // --- the choice -------------------------------------------------------------------------

    void theChoiceIsKeptPerKindAndTheDefaultIsEmpty()
    {
        QCOMPARE(selected(Kind::AudioInput), QString());
        select(Kind::AudioInput, "alsa_input.usb-Mic");
        select(Kind::Camera, "/dev/video2");
        QCOMPARE(selected(Kind::AudioInput), QStringLiteral("alsa_input.usb-Mic"));
        QCOMPARE(selected(Kind::Camera), QStringLiteral("/dev/video2"));
        QCOMPARE(selected(Kind::AudioOutput), QString());
        select(Kind::AudioInput, QString()); // back to the default
        QCOMPARE(selected(Kind::AudioInput), QString());
        QVERIFY(!QSettings().contains("devices/audioInput"));
        QCOMPARE(selected(Kind::Camera), QStringLiteral("/dev/video2"));
    }

    void aChosenDeviceThatIsNotThereIsNotUsedButIsKept()
    {
        select(Kind::AudioInput, "alsa_input.unplugged");
        select(Kind::AudioOutput, "alsa_output.unplugged");
        select(Kind::Camera, "/dev/video99");
        QCOMPARE(selectedIfPresent(Kind::AudioInput), QString());
        QCOMPARE(selectedIfPresent(Kind::AudioOutput), QString());
        QCOMPARE(selectedIfPresent(Kind::Camera), QString());
        QCOMPARE(selected(Kind::AudioInput), QStringLiteral("alsa_input.unplugged")); // still the choice, for when it is back
        QCOMPARE(selected(Kind::Camera), QStringLiteral("/dev/video99"));
    }

    void nothingChosenMeansTheDefault()
    {
        QCOMPARE(selectedIfPresent(Kind::AudioInput), QString());
        QCOMPARE(selectedIfPresent(Kind::Camera), QString());
    }

    void aDeviceThatIsListedIsUsed()
    {
        // The microphones of this machine, if any: choosing the first one must give it back.
        const QList<Device> inputs = audioInputs();
        if (inputs.isEmpty()) QSKIP("no audio server here");
        select(Kind::AudioInput, inputs.first().id);
        QCOMPARE(selectedIfPresent(Kind::AudioInput), inputs.first().id);
    }

    void listsAreWellFormed()
    {
        for (const QList<Device> &list : {audioInputs(), audioOutputs(), cameras()}) {
            QSet<QString> ids;
            for (const Device &d : list) {
                QVERIFY(!d.id.isEmpty());
                QVERIFY(!d.name.isEmpty());
                QVERIFY2(!ids.contains(d.id), qPrintable("twice: " + d.id));
                ids.insert(d.id);
            }
        }
    }

    void thePulseMonitorsAreNotMicrophones()
    {
        for (const Device &d : audioInputs()) QVERIFY2(!d.id.endsWith(".monitor"), qPrintable(d.id));
    }

    void kindsFromTheNamesQmlUses_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("known");
        QTest::addColumn<int>("kind");
        QTest::newRow("microphone") << "audioInput" << true << int(Kind::AudioInput);
        QTest::newRow("speakers") << "audioOutput" << true << int(Kind::AudioOutput);
        QTest::newRow("camera") << "camera" << true << int(Kind::Camera);
        QTest::newRow("other") << "microphone" << false << 0;
        QTest::newRow("empty") << "" << false << 0;
    }
    void kindsFromTheNamesQmlUses()
    {
        QFETCH(QString, name);
        QFETCH(bool, known);
        QFETCH(int, kind);
        Kind k = Kind::Camera;
        QCOMPARE(kindFromName(name, &k), known);
        if (known) QCOMPARE(int(k), kind);
    }

    // --- which camera the call opens --------------------------------------------------------

    void theCameraTheCallOpens_data()
    {
        QTest::addColumn<QString>("environment");
        QTest::addColumn<QString>("chosen");
        QTest::addColumn<QString>("expected");
        QTest::newRow("nothing set") << "" << "" << "/dev/video0";
        QTest::newRow("chosen in the settings") << "" << "/dev/video2" << "/dev/video2";
        QTest::newRow("the variable wins over the settings") << "/dev/video4" << "/dev/video2" << "/dev/video4";
        QTest::newRow("the variable alone") << "/dev/video4" << "" << "/dev/video4";
    }
    void theCameraTheCallOpens()
    {
        QFETCH(QString, environment);
        QFETCH(QString, chosen);
        QFETCH(QString, expected);
        QCOMPARE(cameraToUse(environment, chosen), expected);
    }

    // --- quoting for the pipeline -----------------------------------------------------------

    void quotingForTheDescription()
    {
        QCOMPARE(quoteForPipeline("alsa_input.pci-0000_00_1f.3.analog-stereo"), QStringLiteral("\"alsa_input.pci-0000_00_1f.3.analog-stereo\""));
        QCOMPARE(quoteForPipeline("a \"b\" c"), QStringLiteral("\"a \\\"b\\\" c\""));
        QCOMPARE(quoteForPipeline("back\\slash"), QStringLiteral("\"back\\\\slash\""));
        QCOMPARE(pipelineDeviceProperty(QString()), QString());
        QCOMPARE(pipelineDeviceProperty("hw:1"), QStringLiteral("device=\"hw:1\" "));
    }

    void gstreamersOwnParserReadsTheQuotedValueBack_data()
    {
        QTest::addColumn<QString>("value");
        QTest::newRow("plain") << "alsa_input.usb-Blue_Microphones_Yeti.analog-stereo";
        QTest::newRow("spaces") << "My USB Mic (2)";
        QTest::newRow("quotes") << "the \"best\" mic";
        QTest::newRow("backslashes") << "C:\\devices\\mic";
        QTest::newRow("a pipeline in disguise") << "mic ! fakesink name=evil";
        QTest::newRow("exclamation and equals") << "a=b!c";
        QTest::newRow("accents") << "Microfono integrato è così";
    }
    void gstreamersOwnParserReadsTheQuotedValueBack()
    {
        QFETCH(QString, value);
        // fakesrc has a "name" property like any element: the value must come back unchanged and
        // must not be able to add elements to the pipeline.
        const QByteArray description = QStringLiteral("fakesrc name=%1 ! fakesink").arg(quoteForPipeline(value)).toUtf8();
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(description.constData(), &error);
        QVERIFY2(pipeline && !error, error ? error->message : "no pipeline");
        GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), value.toUtf8().constData());
        QVERIFY2(source, "the element was not found under the original name");
        int elements = 0;
        GstIterator *it = gst_bin_iterate_elements(GST_BIN(pipeline));
        GValue item = G_VALUE_INIT;
        while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) { ++elements; g_value_reset(&item); }
        g_value_unset(&item);
        gst_iterator_free(it);
        QCOMPARE(elements, 2); // fakesrc and fakesink, nothing smuggled in
        gst_object_unref(source);
        gst_object_unref(pipeline);
    }
};

int main(int argc, char **argv)
{
    // The settings go to a temporary XDG_CONFIG_HOME: the real ones are never touched.
    static QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("kteams-test"));
    QCoreApplication::setApplicationName(QStringLiteral("kteams-test"));
    MediaDevicesTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_mediadevices.moc"
