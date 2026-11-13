// The Qt Linguist files (translations/*.ts): complete, consistent, and in step with each other.
// A string changed in the code and not translated everywhere shows English to the user.
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include <QtTest>

namespace {
struct Message {
    QString context, source, translation, type; // type: "", "unfinished", "vanished", "obsolete"
};
struct TsFile {
    QString language;
    QList<Message> messages;
};

TsFile readTs(const QString &path)
{
    TsFile ts;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return ts;
    QXmlStreamReader xml(&f);
    QString context;
    Message current;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QString name = xml.name().toString();
            if (name == "TS") ts.language = xml.attributes().value("language").toString();
            else if (name == "name" && current.source.isNull() && context.isNull()) context = xml.readElementText();
            else if (name == "message") current = Message{context, QString(""), QString(""), QString()};
            else if (name == "source") current.source = xml.readElementText();
            else if (name == "translation") { current.type = xml.attributes().value("type").toString(); current.translation = xml.readElementText(); }
        } else if (xml.isEndElement()) {
            if (xml.name() == QLatin1String("message")) ts.messages.append(current);
            else if (xml.name() == QLatin1String("context")) context = QString();
        }
    }
    if (xml.hasError()) qWarning() << path << xml.errorString();
    return ts;
}

bool live(const Message &m) { return m.type != "vanished" && m.type != "obsolete"; }

QSet<QString> placeholders(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("%(?:n|[1-9])"));
    QSet<QString> found;
    for (auto it = re.globalMatch(text); it.hasNext();) found.insert(it.next().captured());
    return found;
}
}

class TranslationsTest : public QObject
{
    Q_OBJECT
    QMap<QString, TsFile> files; // file name -> content

private slots:
    void initTestCase()
    {
        const QDir dir(QStringLiteral(KTEAMS_TRANSLATIONS_DIR));
        for (const QString &name : dir.entryList({"kteams_*.ts"}, QDir::Files, QDir::Name)) files.insert(name, readTs(dir.filePath(name)));
        QVERIFY2(files.size() >= 11, "the translations folder was not found");
    }

    void everyFileDeclaresItsOwnLanguage()
    {
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            const QString expected = it.key().mid(QStringLiteral("kteams_").size()).chopped(3); // kteams_pt_BR.ts -> pt_BR
            QVERIFY2(it->language == expected || it->language.startsWith(expected), qPrintable(it.key() + " declares " + it->language));
        }
    }

    void nothingIsUnfinishedOrEmpty()
    {
        QStringList problems;
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            for (const Message &m : it->messages)
                if (live(m) && (m.type == "unfinished" || m.translation.trimmed().isEmpty()))
                    problems << QStringLiteral("%1: [%2] \"%3\"").arg(it.key(), m.context, m.source);
        QVERIFY2(problems.isEmpty(), qPrintable("untranslated:\n  " + problems.join("\n  ")));
    }

    void placeholdersAreTheSameInTheTranslation()
    {
        QStringList problems;
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            for (const Message &m : it->messages)
                if (live(m) && placeholders(m.source) != placeholders(m.translation))
                    problems << QStringLiteral("%1: \"%2\" -> \"%3\"").arg(it.key(), m.source, m.translation);
        QVERIFY2(problems.isEmpty(), qPrintable("placeholders differ:\n  " + problems.join("\n  ")));
    }

    void everyLanguageHasTheSameStringsAsItalian()
    {
        const auto keys = [](const TsFile &ts) {
            QSet<QString> k;
            for (const Message &m : ts.messages) if (live(m)) k.insert(m.context + "|" + m.source);
            return k;
        };
        const QSet<QString> reference = keys(files.value("kteams_it.ts"));
        QVERIFY(reference.size() > 200);
        QStringList problems;
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            const QSet<QString> k = keys(it.value());
            for (const QString &missing : reference - k) problems << it.key() + " lacks " + missing;
            for (const QString &extra : k - reference) problems << it.key() + " has an extra " + extra;
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join("\n  ")));
    }

    void theSameSourceHasTheSameTranslationInAContext()
    {
        // The code uses one translation per source string: two entries of one file for the same
        // context and source (QML and C++ locations) must agree.
        QStringList problems;
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            QHash<QString, QString> seen;
            for (const Message &m : it->messages) {
                if (!live(m)) continue;
                const QString key = m.context + "|" + m.source;
                if (seen.contains(key) && seen[key] != m.translation) problems << it.key() + ": " + m.source;
                seen[key] = m.translation;
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join("\n  ")));
    }
};

QTEST_GUILESS_MAIN(TranslationsTest)
#include "tst_translations.moc"
