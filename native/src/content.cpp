#include "content.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTextDocumentFragment>
#include <QUrl>

namespace Content {

// Teams message bodies are HTML; show them as plain text for now. Parsing with
// QTextDocumentFragment decodes entities and line breaks and never loads remote resources.
QString htmlToText(const QString &html)
{
    QString text = QTextDocumentFragment::fromHtml(html).toPlainText();
    text.remove(QChar::ObjectReplacementCharacter); // inline images
    return text.trimmed();
}

namespace {
// Inline markup of one line, on text that is already HTML-escaped: `code`, **bold**, ~~strike~~,
// *italic* and _italic_. A marker must touch the text it wraps (so "2 * 3 * 4" stays as it is) and
// the single ones must not touch a word character outside (so snake_case_names stay as they are).
QString inlineMarkdown(const QString &escaped)
{
    static const QRegularExpression code(QStringLiteral("`([^`\\n]+)`"));
    static const QRegularExpression boldItalic(QStringLiteral("\\*\\*\\*(?=[^\\s*])(.+?)(?<=[^\\s*])\\*\\*\\*"));
    static const QRegularExpression bold(QStringLiteral("\\*\\*(?=\\S)(.+?)(?<=\\S)\\*\\*"));
    static const QRegularExpression strike(QStringLiteral("~~(?=\\S)(.+?)(?<=\\S)~~"));
    static const QRegularExpression italicStar(QStringLiteral("(?<![\\w*])\\*(?=[^\\s*])(.+?)(?<=[^\\s*])\\*(?![\\w*])"));
    static const QRegularExpression italicUnderscore(QStringLiteral("(?<!\\w)_(?=[^\\s_])(.+?)(?<=[^\\s_])_(?!\\w)"));
    // Code spans are set aside first, so what is inside them is not formatted.
    QStringList codes;
    QString text;
    qsizetype last = 0;
    for (auto it = code.globalMatch(escaped); it.hasNext() && codes.size() < 1000;) {
        const auto m = it.next();
        text += escaped.mid(last, m.capturedStart() - last) + QChar(char16_t(0xE000 + codes.size()));
        codes << m.captured(1);
        last = m.capturedEnd();
    }
    text += escaped.mid(last);
    text.replace(boldItalic, QStringLiteral("<b><i>\\1</i></b>"));
    text.replace(bold, QStringLiteral("<b>\\1</b>"));
    text.replace(strike, QStringLiteral("<s>\\1</s>"));
    text.replace(italicStar, QStringLiteral("<i>\\1</i>"));
    text.replace(italicUnderscore, QStringLiteral("<i>\\1</i>"));
    for (int i = 0; i < codes.size(); ++i) text.replace(QChar(char16_t(0xE000 + i)), QStringLiteral("<code>%1</code>").arg(codes[i]));
    // Markers that cross each other ("**a *b** c*") would give tags in the wrong order: then the
    // line is left as it was typed.
    static const QRegularExpression markup(QStringLiteral("<(/?)(b|i|s|code)>"));
    QStringList open;
    for (auto it = markup.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        if (m.captured(1).isEmpty()) open << m.captured(2);
        else if (open.isEmpty() || open.takeLast() != m.captured(2)) return escaped;
    }
    return open.isEmpty() ? text : escaped;
}
}

QString textToHtml(const QString &text)
{
    static const QRegularExpression bullet(QStringLiteral("^[-*] +(\\S.*)$"));
    static const QRegularExpression numbered(QStringLiteral("^\\d{1,3}[.)] +(\\S.*)$"));
    static const QRegularExpression quote(QStringLiteral("^> ?(.*)$"));
    const QStringList lines = text.trimmed().split(QLatin1Char('\n'));
    QString html;
    QStringList plain; // a run of ordinary lines: one paragraph, lines joined by <br>
    const auto flushPlain = [&] {
        while (!plain.isEmpty() && plain.first().trimmed().isEmpty()) plain.removeFirst();
        while (!plain.isEmpty() && plain.last().trimmed().isEmpty()) plain.removeLast();
        if (!plain.isEmpty()) {
            QStringList converted;
            for (const QString &l : std::as_const(plain)) converted << inlineMarkdown(l.toHtmlEscaped());
            html += QStringLiteral("<p>%1</p>").arg(converted.join(QStringLiteral("<br>")));
        }
        plain.clear();
    };
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString line = lines[i];
        if (line.trimmed().startsWith(QLatin1String("```"))) {
            flushPlain();
            QStringList code;
            for (++i; i < lines.size() && !lines[i].trimmed().startsWith(QLatin1String("```")); ++i) code << lines[i];
            html += QStringLiteral("<pre>%1</pre>").arg(code.join(QLatin1Char('\n')).toHtmlEscaped());
            continue;
        }
        QRegularExpressionMatch m;
        const bool isBullet = (m = bullet.match(line)).hasMatch();
        const bool isNumbered = !isBullet && (m = numbered.match(line)).hasMatch();
        if (isBullet || isNumbered) {
            flushPlain();
            const QRegularExpression &same = isBullet ? bullet : numbered;
            QString items;
            for (; i < lines.size(); ++i) {
                const auto item = same.match(lines[i]);
                if (!item.hasMatch()) break;
                items += QStringLiteral("<li>%1</li>").arg(inlineMarkdown(item.captured(1).toHtmlEscaped()));
            }
            --i;
            html += isBullet ? QStringLiteral("<ul>%1</ul>").arg(items) : QStringLiteral("<ol>%1</ol>").arg(items);
            continue;
        }
        if (quote.match(line).hasMatch()) {
            flushPlain();
            QStringList quoted;
            for (; i < lines.size(); ++i) {
                const auto q = quote.match(lines[i]);
                if (!q.hasMatch()) break;
                quoted << inlineMarkdown(q.captured(1).toHtmlEscaped());
            }
            --i;
            html += QStringLiteral("<blockquote>%1</blockquote>").arg(quoted.join(QStringLiteral("<br>")));
            continue;
        }
        plain << line;
    }
    flushPlain();
    return html;
}

// A link marker inside the plain text: one private-use character per <a href> found.
constexpr char16_t kLinkMarkerBase = 0xE000;

// Teams HTML -> plain text plus the images it shows. Teams emoji are <img> tags with the
// emoji in "alt" (purple-teams does the same swap); other images are listed separately.
ParsedContent parseContent(const QString &html)
{
    static const QRegularExpression imgTag(QStringLiteral("<img\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression attribute(QStringLiteral("\\b([a-zA-Z-]+)\\s*=\\s*\"([^\"]*)\""));
    ParsedContent result;
    QString withoutImages;
    qsizetype last = 0;
    for (auto it = imgTag.globalMatch(html); it.hasNext();) {
        const auto tag = it.next();
        withoutImages += html.mid(last, tag.capturedStart() - last);
        last = tag.capturedEnd();
        QHash<QString, QString> attrs;
        for (auto a = attribute.globalMatch(tag.captured()); a.hasNext();) {
            const auto m = a.next();
            attrs.insert(m.captured(1).toLower(), m.captured(2));
        }
        const QString src = QString(attrs.value(QStringLiteral("src"))).replace(QLatin1String("&amp;"), QLatin1String("&"));
        const bool emoji = attrs.value(QStringLiteral("itemtype")).endsWith(QLatin1String("/Emoji"))
            || QUrl(src).host() == QLatin1String("statics.teams.cdn.office.net");
        if (emoji) withoutImages += attrs.value(QStringLiteral("alt")); // still HTML-escaped, decoded below
        else if (src.startsWith(QLatin1String("https://"))) result.images << src;
    }
    withoutImages += html.mid(last);

    // Links whose label is not the address (<a href="...">issue 3039</a>): keep the address,
    // which plain text would lose. Only web and mail addresses become clickable.
    static const QRegularExpression anchor(QStringLiteral("<a\\b([^>]*)>(.*?)</a>"),
                                           QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    struct Link { QString href; QString label; };
    QList<Link> links;
    QString marked;
    last = 0;
    for (auto it = anchor.globalMatch(withoutImages); it.hasNext();) {
        const auto a = it.next();
        marked += withoutImages.mid(last, a.capturedStart() - last);
        last = a.capturedEnd();
        QString href;
        for (auto m = attribute.globalMatch(a.captured(1)); m.hasNext();) {
            const auto attr = m.next();
            if (attr.captured(1).compare(QLatin1String("href"), Qt::CaseInsensitive) == 0)
                href = htmlToText(attr.captured(2)); // decodes &amp; and friends
        }
        const QString scheme = QUrl(href).scheme().toLower();
        const bool usable = scheme == QLatin1String("https") || scheme == QLatin1String("http") || scheme == QLatin1String("mailto");
        if (!usable || links.size() >= 0x1000) { marked += a.captured(2); continue; }
        QString label = htmlToText(a.captured(2)).simplified();
        if (label.isEmpty()) label = href;
        marked += QChar(char16_t(kLinkMarkerBase + links.size()));
        links.append({href, label});
    }
    marked += withoutImages.mid(last);
    const QString text = htmlToText(marked);

    // Rich text: the message HTML itself (sanitized), images and emoji handled above.
    result.richText = sanitizeTeamsHtml(withoutImages);
    // Plain text (previews, editing, search): markers become the link labels.
    for (const QChar c : text) {
        const int index = c.unicode() - kLinkMarkerBase;
        if (index >= 0 && index < links.size()) result.text += links[index].label;
        else result.text += c;
    }
    if (result.richText.isEmpty()) result.richText = linkify(result.text);
    return result;
}

// Teams message HTML -> the rich-text subset Qt renders, with everything else removed:
// formatting, lists, code, quotes, tables and links (web/mail only); mentions in bold.
// Text and entities pass through untouched; unknown tags are dropped, their content kept.
QString sanitizeTeamsHtml(const QString &html)
{
    static const QSet<QString> allowed{QStringLiteral("b"), QStringLiteral("strong"), QStringLiteral("i"), QStringLiteral("em"),
        QStringLiteral("u"), QStringLiteral("s"), QStringLiteral("strike"), QStringLiteral("del"), QStringLiteral("br"), QStringLiteral("p"),
        QStringLiteral("div"), QStringLiteral("ul"), QStringLiteral("ol"), QStringLiteral("li"), QStringLiteral("pre"), QStringLiteral("code"),
        QStringLiteral("blockquote"), QStringLiteral("h1"), QStringLiteral("h2"), QStringLiteral("h3"), QStringLiteral("table"),
        QStringLiteral("tr"), QStringLiteral("td"), QStringLiteral("th"), QStringLiteral("hr"), QStringLiteral("sub"), QStringLiteral("sup")};
    static const QRegularExpression tag(QStringLiteral("<(/?)([a-zA-Z][a-zA-Z0-9]*)([^>]*)>"));
    static const QRegularExpression hrefAttr(QStringLiteral("\\bhref\\s*=\\s*\"([^\"]*)\""), QRegularExpression::CaseInsensitiveOption);
    QString out;
    qsizetype last = 0;
    bool mentionOpen = false; // a mention span became <b>: its closing span must close it
    for (auto it = tag.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        out += html.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        const bool closing = !m.captured(1).isEmpty();
        const QString name = m.captured(2).toLower();
        const QString attrs = m.captured(3);
        if (name == QLatin1String("a")) {
            if (closing) { out += QStringLiteral("</a>"); continue; }
            const auto h = hrefAttr.match(attrs);
            const QString href = h.hasMatch() ? htmlToText(h.captured(1)) : QString();
            const QString scheme = QUrl(href).scheme().toLower();
            if (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto"))
                out += QStringLiteral("<a href=\"%1\">").arg(href.toHtmlEscaped());
            else
                out += QStringLiteral("<a>"); // keeps the pairing with </a>, no link
        } else if (name == QLatin1String("at")) {
            out += closing ? QStringLiteral("</b>") : QStringLiteral("<b>"); // a mention
        } else if (allowed.contains(name)) {
            out += closing ? QStringLiteral("</%1>").arg(name) : (name == QLatin1String("br") || name == QLatin1String("hr")
                                                                       ? QStringLiteral("<%1>").arg(name) : QStringLiteral("<%1>").arg(name));
        } else if (name == QLatin1String("span")) {
            // Teams wraps mentions and formatting in spans; a mention has itemtype .../Mention
            // and closes with a plain </span>, which must close the bold too.
            if (!closing && attrs.contains(QLatin1String("schema.skype.com/Mention"))) { out += QStringLiteral("<b>"); mentionOpen = true; }
            else if (closing && mentionOpen) { out += QStringLiteral("</b>"); mentionOpen = false; }
        }
        // anything else: dropped, content kept
    }
    out += html.mid(last);
    if (mentionOpen) out += QStringLiteral("</b>");
    return out.trimmed();
}

// Plain text -> safe rich text with clickable links (everything else escaped).
QString linkify(const QString &text)
{
    static const QRegularExpression url(QStringLiteral("https?://[^\\s<>\"]+[^\\s<>\".,;:!?)\\]]"));
    QString out;
    qsizetype last = 0;
    for (auto it = url.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        out += text.mid(last, m.capturedStart() - last).toHtmlEscaped();
        const QString link = m.captured().toHtmlEscaped();
        out += QStringLiteral("<a href=\"%1\">%1</a>").arg(link);
        last = m.capturedEnd();
    }
    out += text.mid(last).toHtmlEscaped();
    out.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return out;
}

// properties.files is a JSON array encoded as a string: name, type and a link to open.
QVariantList parseFiles(const QJsonObject &properties)
{
    const QJsonValue v = properties.value(QLatin1String("files"));
    const QJsonArray files = v.isString() ? QJsonDocument::fromJson(v.toString().toUtf8()).array() : v.toArray();
    QVariantList result;
    for (const auto &f : files) {
        const QJsonObject o = f.toObject();
        const QJsonObject info = o.value(QLatin1String("fileInfo")).toObject();
        QString link = o.value(QLatin1String("objectUrl")).toString();
        if (link.isEmpty()) link = info.value(QLatin1String("shareUrl")).toString();
        if (link.isEmpty()) link = info.value(QLatin1String("fileUrl")).toString();
        if (!link.startsWith(QLatin1String("https://"))) link.clear();
        QString name = o.value(QLatin1String("fileName")).toString();
        if (name.isEmpty()) name = o.value(QLatin1String("title")).toString();
        QString share = info.value(QLatin1String("shareUrl")).toString();
        if (!share.startsWith(QLatin1String("https://"))) share = link;
        result << QVariantMap{{QStringLiteral("name"), name},
                              {QStringLiteral("type"), o.value(QLatin1String("fileType")).toString().toLower()},
                              {QStringLiteral("url"), link},
                              {QStringLiteral("shareUrl"), share}};
    }
    return result;
}

// Reaction key -> emoji. Classic keys by name; newer keys start with the code points,
// e.g. "1f602_facewithtearsofjoy" or "2764_heart".
QString emojiForKey(const QString &key)
{
    static const QHash<QString, QString> classic{
        {QStringLiteral("like"), QStringLiteral("👍")}, {QStringLiteral("heart"), QStringLiteral("❤️")},
        {QStringLiteral("laugh"), QStringLiteral("😆")}, {QStringLiteral("surprised"), QStringLiteral("😮")},
        {QStringLiteral("sad"), QStringLiteral("😢")},   {QStringLiteral("angry"), QStringLiteral("😡")}};
    if (const auto it = classic.constFind(key); it != classic.cend()) return *it;
    QString emoji;
    for (const QString &part : key.section(QLatin1Char('_'), 0, 0).split(QLatin1Char('-'))) {
        bool ok = false;
        const uint cp = part.toUInt(&ok, 16);
        if (!ok || cp < 0x20) return key;
        const char32_t c = char32_t(cp);
        emoji += QString::fromUcs4(&c, 1);
    }
    return emoji.isEmpty() ? key : emoji;
}

qint64 parseTime(const QString &iso)
{
    const QDateTime dt = QDateTime::fromString(iso, Qt::ISODateWithMs);
    return dt.isValid() ? dt.toMSecsSinceEpoch() : 0;
}

// Messages worth showing: text and rich text; not typing, call events, thread changes.
bool isChatMessage(const QString &type)
{
    return type == QLatin1String("Text") || type.startsWith(QLatin1String("RichText"));
}


QString uniqueFilePath(const QString &dir, const QString &fileName)
{
    // Never trust a name from the network as a path.
    QString safe = QFileInfo(fileName).fileName();
    if (safe.isEmpty() || safe.startsWith(QLatin1Char('.'))) safe = QStringLiteral("allegato") + safe;
    const QFileInfo base(safe);
    QString candidate = dir + QLatin1Char('/') + safe;
    for (int i = 1; QFile::exists(candidate); ++i)
        candidate = dir + QLatin1Char('/') + base.completeBaseName() + QStringLiteral(" (%1)").arg(i)
            + (base.suffix().isEmpty() ? QString() : QLatin1Char('.') + base.suffix());
    return candidate;
}

QString replyQuoteHtml(const QString &messageId, const QString &authorMri, const QString &authorName, const QString &preview)
{
    return QStringLiteral("<blockquote itemscope=\"\" itemtype=\"http://schema.skype.com/Reply\" itemid=\"%1\">"
                          "<strong itemprop=\"mri\" itemid=\"%2\">%3</strong><span itemprop=\"time\" itemid=\"%1\"></span>"
                          "<p itemprop=\"preview\">%4</p></blockquote>")
        .arg(messageId, authorMri.toHtmlEscaped(), authorName.toHtmlEscaped(), preview.toHtmlEscaped());
}

QString htmlToMarkdown(const QString &html)
{
    // The subset our own messages and the sanitizer produce: b, i, s, code, pre, lists, quotes, br, p, links.
    static const QRegularExpression tag(QStringLiteral("<(/?)([a-zA-Z][a-zA-Z0-9]*)([^>]*)>"));
    static const QRegularExpression hrefAttr(QStringLiteral("\\bhref\\s*=\\s*\"([^\"]*)\""), QRegularExpression::CaseInsensitiveOption);
    QString out;
    bool lineStart = true;
    int quoteDepth = 0;
    struct List { bool ordered; int count; };
    QList<List> lists;
    QString pendingLink;      // href of the <a> being read
    QString linkLabel;
    bool inLink = false;
    bool inPre = false;
    const auto put = [&](const QString &text) {
        if (inLink) { linkLabel += text; return; }
        if (text.isEmpty() || (lineStart && !inPre && !inLink && text.trimmed().isEmpty())) return; // no blank text at the start of a line
        if (lineStart && !inPre) out += QString(quoteDepth, QLatin1Char('>')).replace(QLatin1Char('>'), QStringLiteral("> "));
        lineStart = false;
        out += text;
    };
    const auto newline = [&] { if (!lineStart) { out += QLatin1Char('\n'); lineStart = true; } };
    qsizetype last = 0;
    const auto text = [&](const QString &raw) {
        if (raw.isEmpty()) return;
        // pre-wrap keeps the spaces and line breaks of the text node, entities decoded.
        QString decoded = QTextDocumentFragment::fromHtml(QStringLiteral("<span style=\"white-space:pre-wrap\">%1</span>").arg(raw)).toPlainText();
        if (!inPre) {
            static const QRegularExpression whitespace(QStringLiteral("\\s+"));
            decoded.replace(whitespace, QStringLiteral(" "));
        }
        put(decoded);
    };
    for (auto it = tag.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        text(html.mid(last, m.capturedStart() - last));
        last = m.capturedEnd();
        const bool closing = !m.captured(1).isEmpty();
        const QString name = m.captured(2).toLower();
        if (name == QLatin1String("b") || name == QLatin1String("strong")) put(QStringLiteral("**"));
        else if (name == QLatin1String("i") || name == QLatin1String("em")) put(QStringLiteral("*"));
        else if (name == QLatin1String("s") || name == QLatin1String("strike") || name == QLatin1String("del")) put(QStringLiteral("~~"));
        else if (name == QLatin1String("code") && !inPre) put(QStringLiteral("`"));
        else if (name == QLatin1String("pre")) {
            newline();
            if (!closing) { put(QStringLiteral("```")); newline(); inPre = true; }
            else { inPre = false; newline(); put(QStringLiteral("```")); newline(); }
        } else if (name == QLatin1String("br")) {
            if (inPre) out += QLatin1Char('\n'); else newline();
        } else if (name == QLatin1String("p") || name == QLatin1String("div")) {
            newline();
        } else if (name == QLatin1String("ul") || name == QLatin1String("ol")) {
            newline();
            if (!closing) lists.append({name == QLatin1String("ol"), 0}); else if (!lists.isEmpty()) lists.removeLast();
        } else if (name == QLatin1String("li")) {
            if (!closing) {
                newline();
                const bool ordered = !lists.isEmpty() && lists.last().ordered;
                if (!lists.isEmpty()) ++lists.last().count;
                put(ordered ? QStringLiteral("%1. ").arg(lists.last().count) : QStringLiteral("- "));
            } else newline();
        } else if (name == QLatin1String("blockquote")) {
            newline();
            quoteDepth = closing ? qMax(0, quoteDepth - 1) : quoteDepth + 1;
        } else if (name == QLatin1String("a")) {
            if (!closing) {
                const auto h = hrefAttr.match(m.captured(3));
                pendingLink = h.hasMatch() ? QTextDocumentFragment::fromHtml(h.captured(1)).toPlainText() : QString();
                inLink = true;
                linkLabel.clear();
            } else if (inLink) {
                inLink = false;
                const QString label = linkLabel.trimmed();
                put(pendingLink.isEmpty() || label == pendingLink ? label : QStringLiteral("%1 (%2)").arg(label, pendingLink));
            }
        }
    }
    text(html.mid(last));
    while (out.endsWith(QLatin1Char('\n'))) out.chop(1);
    return out;
}

}
