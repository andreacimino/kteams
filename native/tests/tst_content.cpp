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

    // --- what the user types: Markdown -----------------------------------------------------------

    void markdownInlineFormatting_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::addColumn<QString>("html");
        QTest::newRow("bold") << "say **hello** now" << "<p>say <b>hello</b> now</p>";
        QTest::newRow("italic with stars") << "say *hello* now" << "<p>say <i>hello</i> now</p>";
        QTest::newRow("italic with underscores") << "say _hello_ now" << "<p>say <i>hello</i> now</p>";
        QTest::newRow("strike") << "~~gone~~" << "<p><s>gone</s></p>";
        QTest::newRow("code") << "run `ls -la` here" << "<p>run <code>ls -la</code> here</p>";
        QTest::newRow("bold and italic together") << "***both***" << "<p><b><i>both</i></b></p>";
        QTest::newRow("italic inside bold") << "**a *b* c**" << "<p><b>a <i>b</i> c</b></p>";
        QTest::newRow("several on one line") << "**a** and ~~b~~ and `c`" << "<p><b>a</b> and <s>b</s> and <code>c</code></p>";
        QTest::newRow("markers inside code are not formatting") << "`a*b*c` and `**x**`" << "<p><code>a*b*c</code> and <code>**x**</code></p>";
        QTest::newRow("markup in code is escaped") << "`<div>&`" << "<p><code>&lt;div&gt;&amp;</code></p>";
        QTest::newRow("markup around bold is escaped") << "**<b>x</b>**" << "<p><b>&lt;b&gt;x&lt;/b&gt;</b></p>";
    }
    void markdownInlineFormatting()
    {
        QFETCH(QString, typed);
        QFETCH(QString, html);
        QCOMPARE(textToHtml(typed), html);
    }

    void thingsThatLookLikeMarkersButAreNot_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::newRow("a product") << "2 * 3 * 4 = 24";
        QTest::newRow("stars inside words") << "5*3 and 2*4";
        QTest::newRow("snake case") << "use my_variable_name here";
        QTest::newRow("a path with underscores") << "see https://example.com/a_b_c/d_e_f";
        QTest::newRow("markers with spaces inside") << "** not bold ** and * not italic *";
        QTest::newRow("a single marker") << "a * b and c_ and ~ d";
        QTest::newRow("an empty pair") << "**** and ``";
    }
    void thingsThatLookLikeMarkersButAreNot()
    {
        QFETCH(QString, typed);
        const QString html = textToHtml(typed);
        QVERIFY2(!html.contains("<b>") && !html.contains("<i>") && !html.contains("<s>") && !html.contains("<code>"), qPrintable(html));
        QCOMPARE(html, "<p>" + typed.toHtmlEscaped() + "</p>");
    }

    void markersThatCrossAreLeftAsTyped()
    {
        QCOMPARE(textToHtml("**a *b** c*"), QStringLiteral("<p>**a *b** c*</p>"));
    }

    void bulletedAndNumberedLists()
    {
        QCOMPARE(textToHtml("- one\n- two\n- **three**"), QStringLiteral("<ul><li>one</li><li>two</li><li><b>three</b></li></ul>"));
        QCOMPARE(textToHtml("* one\n* two"), QStringLiteral("<ul><li>one</li><li>two</li></ul>"));
        QCOMPARE(textToHtml("1. one\n2. two\n10) ten"), QStringLiteral("<ol><li>one</li><li>two</li><li>ten</li></ol>"));
    }

    void aListBetweenParagraphs()
    {
        QCOMPARE(textToHtml("intro\n- a\n- b\noutro"), QStringLiteral("<p>intro</p><ul><li>a</li><li>b</li></ul><p>outro</p>"));
        QCOMPARE(textToHtml("- a\n\n- b"), QStringLiteral("<ul><li>a</li></ul><ul><li>b</li></ul>")); // an empty line ends a list
    }

    void aDashIsNotAListWithoutASpaceAndText()
    {
        QCOMPARE(textToHtml("-no list"), QStringLiteral("<p>-no list</p>"));
        QCOMPARE(textToHtml("- "), QStringLiteral("<p>-</p>")); // trimmed: a lone dash
        QCOMPARE(textToHtml("*word* is italic"), QStringLiteral("<p><i>word</i> is italic</p>")); // not a bullet
    }

    void quotes()
    {
        QCOMPARE(textToHtml("> first\n> **second**"), QStringLiteral("<blockquote>first<br><b>second</b></blockquote>"));
        QCOMPARE(textToHtml("before\n> quoted\nafter"), QStringLiteral("<p>before</p><blockquote>quoted</blockquote><p>after</p>"));
        QCOMPARE(textToHtml("a > b"), QStringLiteral("<p>a &gt; b</p>")); // only at the start of a line
    }

    void codeBlocks()
    {
        QCOMPARE(textToHtml("```\nint a = 1;\nif (a < 2) {}\n```"), QStringLiteral("<pre>int a = 1;\nif (a &lt; 2) {}</pre>"));
        QCOMPARE(textToHtml("text\n```cpp\nx\n```\nmore"), QStringLiteral("<p>text</p><pre>x</pre><p>more</p>")); // the language is ignored
        QCOMPARE(textToHtml("```\nnever closed\n**not bold**"), QStringLiteral("<pre>never closed\n**not bold**</pre>"));
    }

    void lineBreaksOfPlainTextAreAsBefore()
    {
        QCOMPARE(textToHtml("a\n\nb"), QStringLiteral("<p>a<br><br>b</p>"));
        QCOMPARE(textToHtml("  \n hello \n  "), QStringLiteral("<p>hello</p>"));
        QCOMPARE(textToHtml("one line"), QStringLiteral("<p>one line</p>"));
    }

    // --- and back, for editing ---------------------------------------------------------------------

    void htmlToMarkdownInline_data()
    {
        QTest::addColumn<QString>("html");
        QTest::addColumn<QString>("markdown");
        QTest::newRow("bold, italic, strike, code") << "<p>a <b>b</b> <i>c</i> <s>d</s> <code>e</code></p>" << "a **b** *c* ~~d~~ `e`";
        QTest::newRow("strong and em") << "<strong>x</strong> <em>y</em>" << "**x** *y*";
        QTest::newRow("a line break") << "<p>a<br>b</p>" << "a\nb";
        QTest::newRow("two paragraphs") << "<p>a</p><p>b</p>" << "a\nb";
        QTest::newRow("entities") << "<p>&lt;tag&gt; &amp; co</p>" << "<tag> & co";
        QTest::newRow("a link with a label") << "<a href=\"https://x.org/p?a=1&amp;b=2\">the page</a>" << "the page (https://x.org/p?a=1&b=2)";
        QTest::newRow("a link that is its own label") << "<a href=\"https://x.org\">https://x.org</a>" << "https://x.org";
        QTest::newRow("a mention") << "<p>hi <at id=\"0\">Bob</at></p>" << "hi Bob";
        QTest::newRow("unknown tags are dropped") << "<p>a <span class=\"x\">b</span></p>" << "a b";
        QTest::newRow("whitespace between blocks") << "<p>a</p>  \n  <p>b</p>" << "a\nb";
    }
    void htmlToMarkdownInline()
    {
        QFETCH(QString, html);
        QFETCH(QString, markdown);
        QCOMPARE(htmlToMarkdown(html), markdown);
    }

    void htmlToMarkdownBlocks()
    {
        QCOMPARE(htmlToMarkdown("<ul><li>x</li><li>y</li></ul>"), QStringLiteral("- x\n- y"));
        QCOMPARE(htmlToMarkdown("<ol><li>x</li><li>y</li></ol>"), QStringLiteral("1. x\n2. y"));
        QCOMPARE(htmlToMarkdown("<p>before</p><ul><li>x</li></ul><p>after</p>"), QStringLiteral("before\n- x\nafter"));
        QCOMPARE(htmlToMarkdown("<blockquote>q1<br>q2</blockquote>"), QStringLiteral("> q1\n> q2"));
        QCOMPARE(htmlToMarkdown("<pre>line1\nline &lt;2&gt;</pre>"), QStringLiteral("```\nline1\nline <2>\n```"));
        QCOMPARE(htmlToMarkdown("<pre>a</pre><p>b</p>"), QStringLiteral("```\na\n```\nb"));
        QCOMPARE(htmlToMarkdown(QString()), QString());
    }

    void editingDoesNotLoseTheFormatting_data()
    {
        QTest::addColumn<QString>("typed");
        QTest::newRow("inline") << "**bold** and *italic* and ~~strike~~ and `code`";
        QTest::newRow("bullets") << "- a\n- b";
        QTest::newRow("numbers") << "1. a\n2. b";
        QTest::newRow("quote") << "> quoted";
        QTest::newRow("code block") << "```\nx = 1\n```";
        QTest::newRow("plain lines") << "one\ntwo\nthree";
        QTest::newRow("a mix") << "intro\n- a\n- b\n> q\nend";
        QTest::newRow("accents and symbols") << "è così: 5 < 6 & \"quoted\"";
    }
    void editingDoesNotLoseTheFormatting()
    {
        QFETCH(QString, typed);
        // Typed -> HTML sent -> HTML shown -> text in the edit box: the same text, and sending it
        // again gives the same HTML.
        const QString html = textToHtml(typed);
        const QString back = htmlToMarkdown(html);
        QCOMPARE(back, typed);
        QCOMPARE(textToHtml(back), html);
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
