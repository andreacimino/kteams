// Sending a file through Microsoft Graph (src/fileuploader.cpp): upload session, chunks, share
// link and the file entry Teams expects, against a fake Graph server on loopback.
#include "fakeserver.h"
#include "fileuploader.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {
constexpr qint64 kChunkSize = 16 * 320 * 1024; // as in the uploader: 5 MiB

QByteArray json(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

QJsonObject driveItem(const QString &name = QStringLiteral("report.pdf"))
{
    return {{"id", "I1"}, {"name", name}, {"webUrl", "https://contoso.sharepoint.com/personal/me/report.pdf"},
            {"parentReference", QJsonObject{{"driveId", "D1"}}},
            {"sharepointIds", QJsonObject{{"listItemUniqueId", "uid-1"}, {"siteUrl", "https://contoso.sharepoint.com/personal/me"},
                                          {"listId", "list-1"}, {"siteId", "site-1"}, {"webId", "web-1"}}}};
}
}

class FileUploaderTest : public QObject
{
    Q_OBJECT
    FakeServer server;
    QTemporaryDir dir;
    FileUploader uploader;

    QString makeFile(const QString &name, const QByteArray &content)
    {
        const QString path = dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) QTest::qFail("cannot create the test file", __FILE__, __LINE__);
        f.write(content);
        return path;
    }
    // The routes of a healthy Graph: every step answers as the real one does.
    void healthyGraph()
    {
        server.respond = [this](const Request &r) -> QByteArray {
            const QString path = QUrl::fromPercentEncoding(r.path().toUtf8());
            if (path.endsWith("/createUploadSession")) return json({{"uploadUrl", server.base() + "/upload/abc"}});
            if (path.endsWith("/filesFolder")) return json({{"id", "F1"}, {"parentReference", QJsonObject{{"driveId", "D9"}}}});
            if (path == "/upload/abc") {
                // "bytes 0-9/10": the last chunk ends at total - 1 and gets the new item back.
                const QByteArray range = r.headers.value("content-range");
                const QList<QByteArray> parts = range.mid(6).split('/');
                const qint64 last = parts.value(0).split('-').value(1).toLongLong();
                return last + 1 == parts.value(1).toLongLong() ? json(driveItem()) : json({{"nextExpectedRanges", QJsonArray{"x"}}});
            }
            if (path.endsWith("/createLink")) return json({{"link", QJsonObject{{"webUrl", "https://contoso-my.sharepoint.com/:b:/g/share"}}}});
            if (path.startsWith("/drives/D1/items/I1")) return json(driveItem());
            return "{}";
        };
    }

private slots:
    void initTestCase() { QVERIFY(server.start()); QVERIFY(dir.isValid()); }

    void init()
    {
        server.reset();
        uploader.setGraphBase(server.base());
    }

    // --- a chat file -----------------------------------------------------------------------

    void aFileInAChatGoesToOneDriveAndGetsAnOrganizationLink()
    {
        healthyGraph();
        const QString path = makeFile("hello world.txt", "0123456789");
        QSignalSpy done(&uploader, &FileUploader::uploaded);
        QSignalSpy failed(&uploader, &FileUploader::failed);
        QSignalSpy progress(&uploader, &FileUploader::progress);
        uploader.upload(path, "graph-token");
        QVERIFY(uploader.busy());
        QVERIFY(done.wait(8000));
        QCOMPARE(failed.count(), 0);
        QVERIFY(!uploader.busy());
        QCOMPARE(server.requests.size(), 4);

        const Request &session = server.requests[0];
        QCOMPARE(session.method, QByteArray("POST"));
        QCOMPARE(QUrl::fromPercentEncoding(session.path().toUtf8()), QStringLiteral("/me/drive/root:/Microsoft Teams Chat Files/hello world.txt:/createUploadSession"));
        QCOMPARE(session.headers.value("authorization"), QByteArray("Bearer graph-token"));
        QCOMPARE(session.json().value("item").toObject().value("@microsoft.graph.conflictBehavior").toString(), QStringLiteral("rename"));

        const Request &chunk = server.requests[1];
        QCOMPARE(chunk.method, QByteArray("PUT"));
        QCOMPARE(chunk.path(), QStringLiteral("/upload/abc"));
        QCOMPARE(chunk.headers.value("content-range"), QByteArray("bytes 0-9/10"));
        QCOMPARE(chunk.headers.value("content-type"), QByteArray("application/octet-stream"));
        QVERIFY(!chunk.headers.contains("authorization")); // the upload URL is pre-authorized
        QCOMPARE(chunk.body, QByteArray("0123456789"));

        const Request &describe = server.requests[2];
        QCOMPARE(describe.method, QByteArray("GET"));
        QCOMPARE(describe.path(), QStringLiteral("/drives/D1/items/I1"));
        QVERIFY(describe.query("$select").contains("sharepointIds"));

        const Request &link = server.requests[3];
        QCOMPARE(link.path(), QStringLiteral("/drives/D1/items/I1/createLink"));
        QCOMPARE(link.json().value("type").toString(), QStringLiteral("edit"));
        QCOMPARE(link.json().value("scope").toString(), QStringLiteral("organization"));

        QCOMPARE(progress.first().at(0).toDouble(), 0.0);
        QCOMPARE(progress.last().at(0).toDouble(), 1.0);
    }

    void theFileEntryHasTheFieldsTeamsSends()
    {
        healthyGraph();
        QSignalSpy done(&uploader, &FileUploader::uploaded);
        uploader.upload(makeFile("Report.PDF", "x"), "t");
        QVERIFY(done.wait(8000));
        const QJsonObject entry = done[0].at(0).toJsonObject();
        QCOMPARE(entry.value("@type").toString(), QStringLiteral("http://schema.skype.com/File"));
        QCOMPARE(entry.value("version").toInt(), 2);
        QCOMPARE(entry.value("id").toString(), QStringLiteral("uid-1"));
        QCOMPARE(entry.value("itemid").toString(), QStringLiteral("uid-1"));
        QCOMPARE(entry.value("fileName").toString(), QStringLiteral("report.pdf")); // the name Graph gave back (renamed on conflict)
        QCOMPARE(entry.value("title").toString(), QStringLiteral("report.pdf"));
        QCOMPARE(entry.value("fileType").toString(), QStringLiteral("pdf"));
        QCOMPARE(entry.value("type").toString(), QStringLiteral("pdf"));
        QCOMPARE(entry.value("state").toString(), QStringLiteral("active"));
        QCOMPARE(entry.value("objectUrl").toString(), QStringLiteral("https://contoso.sharepoint.com/personal/me/report.pdf"));
        QCOMPARE(entry.value("permissionScope").toString(), QStringLiteral("Organization"));
        const QJsonObject info = entry.value("fileInfo").toObject();
        QCOMPARE(info.value("itemId").toString(), QStringLiteral("I1"));
        QCOMPARE(info.value("shareUrl").toString(), QStringLiteral("https://contoso-my.sharepoint.com/:b:/g/share"));
        QCOMPARE(info.value("siteUrl").toString(), QStringLiteral("https://contoso.sharepoint.com/personal/me"));
        const QJsonObject ids = entry.value("sharepointIds").toObject();
        QCOMPARE(ids.value("driveId").toString(), QStringLiteral("D1"));
        QCOMPARE(ids.value("listItemUniqueId").toString(), QStringLiteral("uid-1"));
        QCOMPARE(ids.value("webId").toString(), QStringLiteral("web-1"));
    }

    void bigFilesGoInChunksOfFiveMebibytes()
    {
        healthyGraph();
        const QByteArray content(kChunkSize + 1, 'z');
        QSignalSpy done(&uploader, &FileUploader::uploaded);
        QSignalSpy progress(&uploader, &FileUploader::progress);
        uploader.upload(makeFile("big.bin", content), "t");
        QVERIFY(done.wait(15000));
        QList<Request> chunks;
        for (const Request &r : std::as_const(server.requests)) if (r.method == "PUT") chunks << r;
        QCOMPARE(chunks.size(), 2);
        QCOMPARE(chunks[0].headers.value("content-range"), QByteArray("bytes 0-%1/%2").replace("%1", QByteArray::number(kChunkSize - 1)).replace("%2", QByteArray::number(kChunkSize + 1)));
        QCOMPARE(chunks[0].body.size(), kChunkSize);
        QCOMPARE(chunks[1].headers.value("content-range"), QByteArray("bytes %1-%1/%2").replace("%1", QByteArray::number(kChunkSize)).replace("%2", QByteArray::number(kChunkSize + 1)));
        QCOMPARE(chunks[1].body.size(), 1);
        QVERIFY(progress.size() >= 3);
        QVERIFY(progress[1].at(0).toDouble() > 0.99 && progress[1].at(0).toDouble() < 1.0); // after the first chunk
        QCOMPARE(progress.last().at(0).toDouble(), 1.0);
    }

    void anEmptyFileEndsInsteadOfHanging()
    {
        healthyGraph();
        QSignalSpy done(&uploader, &FileUploader::uploaded);
        QSignalSpy failed(&uploader, &FileUploader::failed);
        uploader.upload(makeFile("empty.txt", QByteArray()), "t");
        QTRY_VERIFY_WITH_TIMEOUT(done.count() + failed.count() == 1, 8000);
        QVERIFY(!uploader.busy());
        qInfo() << "empty file:" << (done.count() ? "uploaded" : failed[0].at(0).toString());
    }

    // --- a channel file ---------------------------------------------------------------------

    void aFileInAChannelGoesToTheChannelFolderWithoutAShareLink()
    {
        healthyGraph();
        QSignalSpy done(&uploader, &FileUploader::uploaded);
        uploader.upload(makeFile("notes.txt", "abc"), "t", "group/1", "19:chan@thread.tacv2");
        QVERIFY(done.wait(8000));
        QCOMPARE(server.requests.size(), 4); // folder, session, chunk, describe (no createLink)
        const Request &folder = server.requests[0];
        QCOMPARE(folder.method, QByteArray("GET"));
        QCOMPARE(folder.path(), QStringLiteral("/teams/group%2F1/channels/19%3Achan%40thread.tacv2/filesFolder"));
        QCOMPARE(QUrl::fromPercentEncoding(server.requests[1].path().toUtf8()), QStringLiteral("/drives/D9/items/F1:/notes.txt:/createUploadSession"));
        const QJsonObject entry = done[0].at(0).toJsonObject();
        QCOMPARE(entry.value("permissionScope").toString(), QString());
        QCOMPARE(entry.value("fileInfo").toObject().value("shareUrl").toString(), entry.value("objectUrl").toString());
    }

    // --- failures ---------------------------------------------------------------------------

    void anUnreadableFileFailsAtOnce()
    {
        QSignalSpy failed(&uploader, &FileUploader::failed);
        uploader.upload(dir.filePath("missing.txt"), "t");
        QCOMPARE(failed.count(), 1);
        QVERIFY(failed[0].at(0).toString().startsWith(QStringLiteral("Cannot read ")));
        QVERIFY(!uploader.busy());
        QVERIFY(server.requests.isEmpty());
    }

    void onlyOneUploadAtATime()
    {
        healthyGraph();
        QSignalSpy done(&uploader, &FileUploader::uploaded);
        QSignalSpy failed(&uploader, &FileUploader::failed);
        uploader.upload(makeFile("first.txt", "1"), "t");
        uploader.upload(makeFile("second.txt", "2"), "t");
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed[0].at(0).toString(), QStringLiteral("Another file is being uploaded."));
        QVERIFY(done.wait(8000)); // the first one is not disturbed
        QCOMPARE(done.count(), 1);
    }

    void failureMessagesCarryTheGraphError_data()
    {
        QTest::addColumn<QString>("failingPart");  // which request gets the error
        QTest::addColumn<bool>("channel");
        QTest::addColumn<QString>("prefix");
        QTest::newRow("channel folder") << "filesFolder" << true << "Channel folder not found: HTTP 500 Quota exceeded";
        QTest::newRow("upload session") << "createUploadSession" << false << "Upload not started: HTTP 500 Quota exceeded";
        QTest::newRow("chunk") << "upload" << false << "Upload interrupted: HTTP 500 Quota exceeded";
        QTest::newRow("describe") << "describe" << false << "File uploaded but not readable: HTTP 500 Quota exceeded";
        QTest::newRow("share link") << "createLink" << false << "File uploaded but not shared: HTTP 500 Quota exceeded";
    }
    void failureMessagesCarryTheGraphError()
    {
        QFETCH(QString, failingPart);
        QFETCH(bool, channel);
        QFETCH(QString, prefix);
        healthyGraph();
        // The request that gets the error: one stage of the upload.
        const auto isFailingStage = [&](const Request &r) {
            const QString path = QUrl::fromPercentEncoding(r.path().toUtf8());
            if (failingPart == "upload") return path == "/upload/abc";
            if (failingPart == "describe") return r.method == "GET" && path.startsWith("/drives/D1/items/I1");
            return path.endsWith(failingPart);
        };
        const auto healthy = server.respond;
        server.respond = [&](const Request &r) { return isFailingStage(r) ? json({{"error", QJsonObject{{"message", "Quota exceeded"}}}}) : healthy(r); };
        server.statusOf = [&](const Request &r) { return isFailingStage(r) ? 500 : 200; };
        QSignalSpy done(&uploader, &FileUploader::uploaded);
        QSignalSpy failed(&uploader, &FileUploader::failed);
        if (channel) uploader.upload(makeFile("f.txt", "x"), "t", "g", "c");
        else uploader.upload(makeFile("f.txt", "x"), "t");
        QVERIFY(failed.wait(8000));
        QCOMPARE(failed[0].at(0).toString(), prefix);
        QCOMPARE(done.count(), 0);
        QVERIFY(!uploader.busy()); // free for the next file
    }

    void anUploadResponseWithoutAnItemIsRejected()
    {
        healthyGraph();
        const auto healthy = server.respond;
        server.respond = [&](const Request &r) -> QByteArray { return r.path() == "/upload/abc" ? QByteArray("{}") : healthy(r); };
        QSignalSpy failed(&uploader, &FileUploader::failed);
        uploader.upload(makeFile("f.txt", "x"), "t");
        QVERIFY(failed.wait(8000));
        QCOMPARE(failed[0].at(0).toString(), QStringLiteral("Unexpected upload response."));
    }

    void afterAFailureTheNextUploadWorks()
    {
        server.status = 500;
        server.answer = R"({"error":{"message":"down"}})";
        QSignalSpy failed(&uploader, &FileUploader::failed);
        uploader.upload(makeFile("a.txt", "a"), "t");
        QVERIFY(failed.wait(8000));
        server.reset();
        healthyGraph();
        QSignalSpy done(&uploader, &FileUploader::uploaded);
        uploader.upload(makeFile("b.txt", "b"), "t");
        QVERIFY(done.wait(8000));
    }
};

QTEST_GUILESS_MAIN(FileUploaderTest)
#include "tst_fileuploader.moc"
