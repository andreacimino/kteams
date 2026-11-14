// DesktopIntegration's start-at-login entry: autostart files written under the app's earlier
// ids are moved to the current one. QStandardPaths runs in test mode (~/.qttest), so the
// user's real autostart folder is never touched.
#include "desktopintegration.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QtTest>

namespace {
QString autostartDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/autostart/");
}
QString entryPath(const QString &id) { return autostartDir() + id + QStringLiteral(".desktop"); }
QString currentEntry() { return entryPath(QString::fromLatin1(DesktopIntegration::kDesktopId)); }
}

class DesktopIntegrationTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void init()
    {
        QDir(autostartDir()).removeRecursively();
        QVERIFY(QDir().mkpath(autostartDir()));
    }
    void cleanupTestCase() { QDir(autostartDir()).removeRecursively(); }

    void legacyAutostartEntryIsMovedToTheCurrentId_data()
    {
        QTest::addColumn<QString>("legacyId");
        // Written by hand, not taken from kLegacyDesktopIds: dropping an id there must fail here.
        QTest::newRow("ac-teams-native") << QStringLiteral("org.ac.teams.native");
        QTest::newRow("0.1.0") << QStringLiteral("org.kteams.kteams");
    }
    void legacyAutostartEntryIsMovedToTheCurrentId()
    {
        QFETCH(QString, legacyId);
        QFile legacy(entryPath(legacyId));
        QVERIFY(legacy.open(QIODevice::WriteOnly));
        legacy.write("[Desktop Entry]\nName=KTeams\nType=Application\n");
        legacy.close();

        DesktopIntegration integration;

        QVERIFY(!QFile::exists(entryPath(legacyId)));
        QVERIFY(integration.startAtLogin());
        QFile current(currentEntry());
        QVERIFY(current.open(QIODevice::ReadOnly));
        QVERIFY(current.readAll().contains("Icon=io.github.andreacimino.kteams\n"));
    }

    void noEntryIsCreatedWithoutALegacyOne()
    {
        DesktopIntegration integration;

        QVERIFY(!integration.startAtLogin());
        QVERIFY(!QFile::exists(currentEntry()));
    }

    void switchingOffRemovesTheEntry()
    {
        DesktopIntegration integration;
        integration.setStartAtLogin(true);
        QVERIFY(QFile::exists(currentEntry()));

        integration.setStartAtLogin(false);

        QVERIFY(!QFile::exists(currentEntry()));
    }
};

int main(int argc, char *argv[])
{
    // The tray's menu is a QMenu: a widgets application is needed.
    QApplication app(argc, argv);
    DesktopIntegrationTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_desktopintegration.moc"
