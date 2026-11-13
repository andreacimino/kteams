// Demo mode (KTEAMS_DEMO=1, src/demo.h): the models show made-up data in place of the real one.
#include "demo.h"
#include "models.h"

#include <QCoreApplication>
#include <QtTest>

class DemoTest : public QObject
{
    Q_OBJECT
private slots:
    void demoIsOnInThisProcess() { QVERIFY(Demo::enabled()); }

    void substitutionsAreDeterministicAndNeverEmpty()
    {
        QCOMPARE(Demo::name("Mario Rossi"), Demo::name("Mario Rossi"));
        QVERIFY(Demo::name("Mario Rossi") != "Mario Rossi");
        QVERIFY(!Demo::title("Team Alpha").isEmpty());
        QVERIFY(Demo::title("Team Alpha") != "Team Alpha");
        QVERIFY(Demo::text("secret message").contains(QLatin1Char(' ')));
        QCOMPARE(Demo::name(QString()), QString());
        QCOMPARE(Demo::text(QString()), QString());
        QVERIFY(Demo::pick("x", 5) >= 0 && Demo::pick("x", 5) < 5);
    }

    void differentInputsSpreadOverTheLists()
    {
        QSet<QString> names;
        for (int i = 0; i < 50; ++i) names.insert(Demo::name(QStringLiteral("person %1").arg(i)));
        QVERIFY(names.size() > 3);
    }

    void modelsHideRealContent()
    {
        MessageModel m;
        MessageItem item;
        item.id = "1"; item.from = "Mario Rossi"; item.content = "the real <secret>";
        item.richText = "<b>the real</b> secret"; item.images = {"https://x/photo.jpg"};
        item.files = {QVariantMap{{"name", "contract.pdf"}}};
        m.setItems({item});
        const QModelIndex row = m.index(0);
        QVERIFY(!row.data(MessageModel::ContentRole).toString().contains("secret"));
        QVERIFY(!row.data(MessageModel::RichTextRole).toString().contains("secret"));
        QVERIFY(!row.data(MessageModel::RichTextRole).toString().contains(QLatin1Char('<'))); // escaped
        QVERIFY(row.data(MessageModel::ImagesRole).toStringList().isEmpty());
        QVERIFY(row.data(MessageModel::FilesRole).toList().isEmpty());
        QVERIFY(row.data(MessageModel::FromRole).toString() != "Mario Rossi");

        CalendarModel cal;
        CalendarEvent e; e.id = "1"; e.subject = "Salary review"; e.location = "Room 4"; e.organizer = "Mario Rossi";
        cal.setItems({e});
        QVERIFY(cal.index(0).data(CalendarModel::SubjectRole).toString() != "Salary review");
        QCOMPARE(cal.index(0).data(CalendarModel::LocationRole).toString(), QString());
    }
};

int main(int argc, char **argv)
{
    qputenv("KTEAMS_DEMO", "1"); // before the first Demo::enabled() call, which caches it
    QCoreApplication app(argc, argv);
    DemoTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_demo.moc"
