// Teams message content: HTML to text, links, files, reactions (src/content.cpp).
#include "content.h"

#include <QJsonDocument>
#include <QtTest>

using namespace Content;

class ContentTest : public QObject
{
    Q_OBJECT
private slots:
    void htmlToTextDecodesEntitiesAndDropsTags()
    {
        QCOMPARE(htmlToText(QStringLiteral("<p>Hello &amp; <b>bye</b></p>")), QStringLiteral("Hello & bye"));
        QCOMPARE(htmlToText(QStringLiteral("  <div>a<br>b</div>  ")), QStringLiteral("a\nb"));
    }

    void textToHtmlEscapesAndKeepsLineBreaks()
    {
        QCOMPARE(textToHtml(QStringLiteral("a <b>\nc ")), QStringLiteral("<p>a &lt;b&gt;<br>c</p>"));
    }

    void parseContentSwapsTeamsEmojiForTheirAlt()
    {
        const ParsedContent p = parseContent(QStringLiteral(
            "<p>Hi <img itemtype=\"http://schema.skype.com/Emoji\" alt=\"😀\" src=\"https://statics.teams.cdn.office.net/x.png\"> there</p>"));
        QCOMPARE(p.text, QStringLiteral("Hi 😀 there"));
        QVERIFY(p.images.isEmpty());
        QVERIFY(!p.richText.contains(QLatin1String("<img")));
    }

    void parseContentListsPictures()
    {
        const ParsedContent p = parseContent(QStringLiteral(
            "<p>look</p><img src=\"https://eu-api.asm.skype.com/v1/objects/0-abc/views/imgo?a=1&amp;b=2\" alt=\"\">"));
        QCOMPARE(p.text, QStringLiteral("look"));
        QCOMPARE(p.images, QStringList{QStringLiteral("https://eu-api.asm.skype.com/v1/objects/0-abc/views/imgo?a=1&b=2")});
    }

    void parseContentKeepsLabelledLinks()
    {
        const ParsedContent p = parseContent(QStringLiteral(
            "<p>see <a href=\"https://example.com/issues/3039?a=1&amp;b=2\">issue 3039</a></p>"));
        QCOMPARE(p.text, QStringLiteral("see issue 3039"));
        QVERIFY(p.richText.contains(QLatin1String("<a href=\"https://example.com/issues/3039?a=1&amp;b=2\">issue 3039</a>")));
    }

    void parseContentRefusesNonWebLinks()
    {
        const ParsedContent p = parseContent(QStringLiteral("<p><a href=\"javascript:alert(1)\">click</a></p>"));
        QCOMPARE(p.text, QStringLiteral("click"));
        QVERIFY(!p.richText.contains(QLatin1String("javascript")));
        QVERIFY(p.richText.contains(QLatin1String("<a>click</a>")));
    }

    void parseContentWithoutMarkupPassesTextThrough()
    {
        // Teams wraps every address in <a> in rich bodies; bare "Text" messages go through linkify().
        const ParsedContent p = parseContent(QStringLiteral("go to https://example.com now"));
        QCOMPARE(p.text, QStringLiteral("go to https://example.com now"));
        QCOMPARE(p.richText, p.text);
        QVERIFY(p.images.isEmpty());
    }

    void sanitizeNestedMentionInsideFormatting()
    {
        QCOMPARE(sanitizeTeamsHtml(QStringLiteral("<i>cc <span itemtype=\"http://schema.skype.com/Mention\" itemid=\"0\">Eve</span></i> <span>plain</span>")),
                 QStringLiteral("<i>cc <b>Eve</b></i> plain"));
    }

    void sanitizeKeepsFormattingDropsUnknownTags()
    {
        QCOMPARE(sanitizeTeamsHtml(QStringLiteral("<div><b>bold</b> <script>bad()</script> <font color=\"red\">red</font></div>")),
                 QStringLiteral("<div><b>bold</b> bad() red</div>"));
    }

    void sanitizeTurnsMentionsIntoBoldAndClosesThem()
    {
        QCOMPARE(sanitizeTeamsHtml(QStringLiteral(
                     "<p>Hi <span itemscope=\"\" itemtype=\"http://schema.skype.com/Mention\" itemid=\"0\">Andrea</span> and <at id=\"1\">Bob</at> ok</p>")),
                 QStringLiteral("<p>Hi <b>Andrea</b> and <b>Bob</b> ok</p>"));
    }

    void sanitizeKeepsOnlyWebAndMailLinks()
    {
        QCOMPARE(sanitizeTeamsHtml(QStringLiteral("<a href=\"mailto:a@b.c\">mail</a> <a href=\"file:///etc/passwd\">no</a>")),
                 QStringLiteral("<a href=\"mailto:a@b.c\">mail</a> <a>no</a>"));
    }

    void linkify_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("html");
        QTest::newRow("url with trailing comma") << "see https://example.com/a?b=1, ok"
            << "see <a href=\"https://example.com/a?b=1\">https://example.com/a?b=1</a>, ok";
        QTest::newRow("markup is escaped") << "<b>&</b>" << "&lt;b&gt;&amp;&lt;/b&gt;";
        QTest::newRow("line breaks") << "a\nb" << "a<br>b";
        QTest::newRow("url in parentheses") << "(http://x.org/p)" << "(<a href=\"http://x.org/p\">http://x.org/p</a>)";
    }
    void linkify()
    {
        QFETCH(QString, text);
        QFETCH(QString, html);
        QCOMPARE(Content::linkify(text), html);
    }

    void parseFilesReadsTheStringEncodedArray()
    {
        const QJsonObject props{{QStringLiteral("files"), QStringLiteral(
            "[{\"fileName\":\"a.pdf\",\"fileType\":\"PDF\",\"objectUrl\":\"https://x/a.pdf\",\"fileInfo\":{\"shareUrl\":\"https://x/s\"}}]")}};
        const QVariantList files = parseFiles(props);
        QCOMPARE(files.size(), 1);
        const QVariantMap f = files.first().toMap();
        QCOMPARE(f.value(QStringLiteral("name")).toString(), QStringLiteral("a.pdf"));
        QCOMPARE(f.value(QStringLiteral("type")).toString(), QStringLiteral("pdf"));
        QCOMPARE(f.value(QStringLiteral("url")).toString(), QStringLiteral("https://x/a.pdf"));
        QCOMPARE(f.value(QStringLiteral("shareUrl")).toString(), QStringLiteral("https://x/s"));
    }

    void parseFilesFallsBackToTitleAndFileUrlAndDropsInsecureLinks()
    {
        const QJsonObject props{{QStringLiteral("files"), QJsonArray{
            QJsonObject{{QStringLiteral("title"), QStringLiteral("notes.txt")},
                        {QStringLiteral("fileInfo"), QJsonObject{{QStringLiteral("fileUrl"), QStringLiteral("https://x/n")}}}},
            QJsonObject{{QStringLiteral("fileName"), QStringLiteral("bad")},
                        {QStringLiteral("objectUrl"), QStringLiteral("http://insecure/bad")}}}}};
        const QVariantList files = parseFiles(props);
        QCOMPARE(files.size(), 2);
        QCOMPARE(files[0].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("notes.txt"));
        QCOMPARE(files[0].toMap().value(QStringLiteral("url")).toString(), QStringLiteral("https://x/n"));
        QCOMPARE(files[0].toMap().value(QStringLiteral("shareUrl")).toString(), QStringLiteral("https://x/n"));
        QCOMPARE(files[1].toMap().value(QStringLiteral("url")).toString(), QString());
        QVERIFY(parseFiles({}).isEmpty());
    }

    void emojiForKey_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("emoji");
        QTest::newRow("classic like") << "like" << "👍";
        QTest::newRow("classic heart") << "heart" << "❤️";
        QTest::newRow("code point") << "1f602_facewithtearsofjoy" << "😂";
        QTest::newRow("sequence") << "2764-fe0f_heart" << QString::fromUtf8("❤️");
        QTest::newRow("unknown stays") << "custom" << "custom";
        QTest::newRow("empty") << "" << "";
    }
    void emojiForKey()
    {
        QFETCH(QString, key);
        QFETCH(QString, emoji);
        QCOMPARE(Content::emojiForKey(key), emoji);
    }

    void parseTimeReadsIsoWithMilliseconds()
    {
        QCOMPARE(parseTime(QStringLiteral("1970-01-01T00:00:01.500Z")), qint64(1500));
        QCOMPARE(parseTime(QStringLiteral("2026-11-13T07:00:00Z")), QDateTime(QDate(2026, 11, 13), QTime(7, 0), QTimeZone::UTC).toMSecsSinceEpoch());
        QCOMPARE(parseTime(QStringLiteral("not a date")), qint64(0));
        QCOMPARE(parseTime(QString()), qint64(0));
    }

    void isChatMessage_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<bool>("shown");
        QTest::newRow("text") << "Text" << true;
        QTest::newRow("rich text") << "RichText/Html" << true;
        QTest::newRow("media card") << "RichText/Media_Card" << true;
        QTest::newRow("call event") << "Event/Call" << false;
        QTest::newRow("typing") << "Control/Typing" << false;
        QTest::newRow("member added") << "ThreadActivity/AddMember" << false;
    }
    void isChatMessage()
    {
        QFETCH(QString, type);
        QFETCH(bool, shown);
        QCOMPARE(Content::isChatMessage(type), shown);
    }
};

QTEST_MAIN(ContentTest)
#include "tst_content.moc"
