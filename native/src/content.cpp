#include "content.h"

#include <QDateTime>
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

QString textToHtml(const QString &text)
{
    QString html = text.trimmed().toHtmlEscaped();
    html.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return QStringLiteral("<p>%1</p>").arg(html);
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

}
