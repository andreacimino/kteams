#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

// Teams message content: HTML bodies to plain and rich text, files and reactions from the
// message properties. Pure functions, with no network and no state (see tests/tst_content.cpp).
namespace Content {

struct ParsedContent {
    QString text;        // plain text (links shown by their label)
    QString richText;    // the same, escaped, with clickable links (labelled ones too)
    QStringList images;  // remote URLs of pasted pictures / GIFs
};

// Teams HTML -> plain text, rich text and the images it shows.
ParsedContent parseContent(const QString &html);
// Teams message HTML -> the rich-text subset Qt renders (formatting, lists, links, mentions in bold).
QString sanitizeTeamsHtml(const QString &html);
// Plain text -> safe rich text with clickable links (everything else escaped).
QString linkify(const QString &text);
// HTML -> plain text (entities decoded, inline images removed).
QString htmlToText(const QString &html);
// Plain text -> the HTML Teams expects for a message body.
QString textToHtml(const QString &text);
// properties.files (a JSON array, often encoded as a string) -> [{name, type, url, shareUrl}].
QVariantList parseFiles(const QJsonObject &properties);
// Reaction key ("like", "1f602_facewithtearsofjoy") -> emoji; unknown keys come back as they are.
QString emojiForKey(const QString &key);
// ISO 8601 time -> ms since the epoch, 0 when unparsable.
qint64 parseTime(const QString &iso);
// Messages worth showing: text and rich text; not typing, call events, thread changes.
bool isChatMessage(const QString &type);

// A file name from the network -> a path in `dir` that does not exist yet: only the last
// component is used (never a path), a hidden name gets a prefix, "name (1).ext" when taken.
QString uniqueFilePath(const QString &dir, const QString &fileName);
// Teams' reply quote: a blockquote of schema.skype.com/Reply naming the message, its author
// (MRI and name) and a preview; Teams clients show it as a quoted bubble.
QString replyQuoteHtml(const QString &messageId, const QString &authorMri, const QString &authorName, const QString &preview);

}
