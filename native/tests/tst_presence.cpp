// How the own presence is shown (src/presenceinfo.cpp): the colours, names and the tray icon
// with its dot. The icon is drawn and its pixels are looked at.
#include "presenceinfo.h"

#include <QGuiApplication>
#include <QPainter>
#include <QtTest>

using namespace PresenceInfo;

namespace {
// A flat black icon: whatever is not black in the result was drawn by iconWithPresence().
QIcon blackIcon()
{
    QPixmap pm(128, 128);
    pm.fill(Qt::black);
    return QIcon(pm);
}
QColor pixel(const QImage &image, qreal x, qreal y) { return image.pixelColor(int(x), int(y)); }
bool close(const QColor &a, const QColor &b, int tolerance = 6)
{
    return qAbs(a.red() - b.red()) <= tolerance && qAbs(a.green() - b.green()) <= tolerance && qAbs(a.blue() - b.blue()) <= tolerance;
}
}

class PresenceTest : public QObject
{
    Q_OBJECT
private slots:
    void themeIconNames_data()
    {
        QTest::addColumn<QString>("availability");
        QTest::addColumn<QString>("icon");
        QTest::newRow("available") << "Available" << "user-online";
        QTest::newRow("away") << "Away" << "user-away";
        QTest::newRow("be right back") << "BeRightBack" << "user-away";
        QTest::newRow("offline") << "Offline" << "user-offline";
        QTest::newRow("unknown") << "" << "user-offline";
        QTest::newRow("busy") << "Busy" << "user-busy";
        QTest::newRow("do not disturb") << "DoNotDisturb" << "user-busy";
        QTest::newRow("in a call") << "InACall" << "user-busy";
        QTest::newRow("in a meeting") << "InAMeeting" << "user-busy";
        QTest::newRow("presenting") << "Presenting" << "user-busy";
    }
    void themeIconNames()
    {
        QFETCH(QString, availability);
        QFETCH(QString, icon);
        QCOMPARE(presenceIcon(availability), icon);
    }

    void dotColours_data()
    {
        QTest::addColumn<QString>("availability");
        QTest::addColumn<QColor>("color");
        const QColor green(0x2e, 0xb8, 0x5c), amber(0xf5, 0xb8, 0x00), grey(0x9a, 0x9a, 0x9a), red(0xd9, 0x2d, 0x2d);
        QTest::newRow("available") << "Available" << green;
        QTest::newRow("away") << "Away" << amber;
        QTest::newRow("be right back") << "BeRightBack" << amber;
        QTest::newRow("offline") << "Offline" << grey;
        QTest::newRow("busy") << "Busy" << red;
        QTest::newRow("do not disturb") << "DoNotDisturb" << red;
        QTest::newRow("in a call") << "InACall" << red;
        QTest::newRow("a state we do not know is still red") << "SomethingNew" << red;
        QTest::newRow("unknown") << "" << QColor();
    }
    void dotColours()
    {
        QFETCH(QString, availability);
        QFETCH(QColor, color);
        QCOMPARE(presenceColor(availability), color);
        QCOMPARE(presenceColor(availability).isValid(), color.isValid());
    }

    void presenceNamesAreTheTeamsWords()
    {
        QCOMPARE(presenceText("Available"), QStringLiteral("Available"));
        QCOMPARE(presenceText("Busy"), QStringLiteral("Busy"));
        QCOMPARE(presenceText("DoNotDisturb"), QStringLiteral("Do not disturb"));
        QCOMPARE(presenceText("BeRightBack"), QStringLiteral("Be right back"));
        QCOMPARE(presenceText("InACall"), QStringLiteral("In a call"));
        QCOMPARE(presenceText("InAMeeting"), QStringLiteral("In a meeting"));
        QCOMPARE(presenceText("Presenting"), QStringLiteral("Presenting"));
        QCOMPARE(presenceText("Offline"), QStringLiteral("Offline"));
        QCOMPARE(presenceText("Away"), QStringLiteral("Away"));
    }

    void anUnknownStateIsShownAsItIs()
    {
        QCOMPARE(presenceText("OffWork"), QStringLiteral("OffWork"));
        QCOMPARE(presenceText(QString()), QString());
    }

    // --- the drawn icon ---------------------------------------------------------------------

    void theDotIsPaintedInTheCornerAtEverySize_data()
    {
        QTest::addColumn<QString>("availability");
        QTest::addColumn<QColor>("color");
        QTest::newRow("available") << "Available" << QColor(0x2e, 0xb8, 0x5c);
        QTest::newRow("away") << "Away" << QColor(0xf5, 0xb8, 0x00);
        QTest::newRow("offline") << "Offline" << QColor(0x9a, 0x9a, 0x9a);
        QTest::newRow("busy") << "Busy" << QColor(0xd9, 0x2d, 0x2d);
    }
    void theDotIsPaintedInTheCornerAtEverySize()
    {
        QFETCH(QString, availability);
        QFETCH(QColor, color);
        const QIcon icon = iconWithPresence(blackIcon(), availability);
        const QList<QSize> sizes = icon.availableSizes();
        QCOMPARE(sizes.size(), 6);
        for (const QSize &size : sizes) {
            const QImage image = icon.pixmap(size).toImage();
            const qreal s = size.width(), d = s * 0.30;
            QVERIFY2(close(pixel(image, s - d / 2, s - d / 2), color), qPrintable(QStringLiteral("%1 px, centre of the dot").arg(size.width())));
            QVERIFY2(close(pixel(image, 2, 2), Qt::black), "the rest of the icon is untouched");
            QVERIFY2(close(pixel(image, s / 2, 2), Qt::black), "the top edge is untouched");
        }
    }

    void doNotDisturbHasTheWhiteBar()
    {
        const QIcon icon = iconWithPresence(blackIcon(), "DoNotDisturb");
        const QImage image = icon.pixmap(QSize(128, 128)).toImage();
        const qreal d = 128 * 0.30, centre = 128 - d / 2;
        QVERIFY(close(pixel(image, centre, centre), Qt::white)); // the bar crosses the middle of the dot
        QVERIFY(close(pixel(image, centre, centre - d * 0.3), QColor(0xd9, 0x2d, 0x2d))); // red above it
        QVERIFY(close(pixel(image, centre, centre + d * 0.3), QColor(0xd9, 0x2d, 0x2d))); // and below it
    }

    void otherBusyStatesHaveNoBar()
    {
        const QImage image = iconWithPresence(blackIcon(), "Busy").pixmap(QSize(128, 128)).toImage();
        const qreal d = 128 * 0.30;
        QVERIFY(close(pixel(image, 128 - d / 2, 128 - d / 2), QColor(0xd9, 0x2d, 0x2d)));
    }

    void anUnknownPresenceLeavesTheIconAlone()
    {
        const QIcon base = blackIcon();
        const QIcon icon = iconWithPresence(base, QString());
        QCOMPARE(icon.cacheKey(), base.cacheKey());
    }

    void theDotHasAWhiteRingToStandOutOnDarkPanels()
    {
        const QImage image = iconWithPresence(blackIcon(), "Available").pixmap(QSize(128, 128)).toImage();
        const qreal d = 128 * 0.30;
        // Just outside the dot's left edge, on its vertical centre: the white outline.
        const QColor edge = pixel(image, 128 - d, 128 - d / 2);
        QVERIFY2(edge.red() > 128 && edge.green() > 128 && edge.blue() > 128, qPrintable(edge.name()));
    }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    PresenceTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_presence.moc"
