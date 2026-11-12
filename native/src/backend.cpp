#include "backend.h"
#include "demo.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QPixmap>
#include <QDesktopServices>
#include <QDir>
#include <QImageReader>
#include <QNetworkReply>
#include <QSettings>
#include <QCoreApplication>
#include <QUrlQuery>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMimeData>
#include <QDateTime>
#include <QLocale>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextDocumentFragment>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <QTimer>
#include <QVideoSink>
#include <KNotification>
#include <unistd.h>
#include <algorithm>

namespace {
// Teams' activity feed (mentions, replies, reactions) is this conversation.
constexpr auto kActivityFeed = "48:notifications";
// When opening a conversation, load more pages until this many messages are visible
// (pages can be full of system events), at most kMaxAutoFillPages extra pages.
constexpr int kMinVisibleMessages = 15;
constexpr int kMaxAutoFillPages = 5;
// Incremental sync interval, while the window is active / in the background.
constexpr int kSyncActiveSecs = 5;
constexpr int kSyncBackgroundSecs = 30;
constexpr int kSyncActivePushedSecs = 30;
constexpr int kSyncBackgroundPushedSecs = 120;

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

struct ParsedContent {
    QString text;        // plain text (links shown by their label)
    QString richText;    // the same, escaped, with clickable links (labelled ones too)
    QStringList images;  // remote URLs of pasted pictures / GIFs
};

QString linkify(const QString &text);
QString sanitizeTeamsHtml(const QString &html);

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
            // Teams wraps mentions and formatting in spans; a mention has itemtype .../Mention.
            if (attrs.contains(QLatin1String("schema.skype.com/Mention"))) out += closing ? QStringLiteral("</b>") : QStringLiteral("<b>");
            else if (closing) out += QString();
        }
        // anything else: dropped, content kept
    }
    out += html.mid(last);
    // Mention spans close with </span>: pair the bold with the first closing span after it.
    static const QRegularExpression unclosedMention(QStringLiteral("<b>([^<]*)</span>"));
    out.replace(unclosedMention, QStringLiteral("<b>\\1</b>"));
    out.remove(QStringLiteral("</span>"));
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

Backend::Backend(QObject *parent) : QObject(parent)
{
    connect(&m_oauth, &OAuth::ready, this, [this](bool hasSession) {
        if (hasSession) connectChat();
        else setState(State::SignedOut);
    });
    connect(&m_oauth, &OAuth::deviceCodeReceived, this, [this](const QString &code, const QString &url, const QString &) {
        m_userCode = code;
        m_verificationUrl = url;
        emit deviceCodeChanged();
        setState(State::WaitingForCode);
    });
    connect(&m_oauth, &OAuth::signedIn, this, &Backend::connectChat);
    connect(&m_oauth, &OAuth::failed, this, &Backend::onAuthFailed);

    connect(&m_client, &TeamsClient::skypeTokenReady, this, &Backend::onSkypeToken);
    connect(&m_client, &TeamsClient::selfPropertiesFetched, this, &Backend::handleSelf);
    connect(&m_client, &TeamsClient::conversationsFetched, this, &Backend::handleConversations);
    connect(&m_client, &TeamsClient::messagesFetched, this, &Backend::handleMessages);
    connect(&m_client, &TeamsClient::messageSent, this, [this](const QString &cid) { m_client.fetchMessages(cid); });
    connect(&m_client, &TeamsClient::errorOccurred, this, &Backend::onClientError);
    connect(&m_client, &TeamsClient::chatsAndTeamsFetched, this, &Backend::handleChatsAndTeams);
    connect(&m_client, &TeamsClient::profilesFetched, this, &Backend::handleProfiles);
    connect(&m_client, &TeamsClient::calendarFetched, this, &Backend::handleCalendar);
    connect(&m_client, &TeamsClient::messageChanged, this, [this](const QString &cid) {
        if (cid == m_currentConversationId) m_client.fetchMessages(cid);
    });
    connect(&m_client, &TeamsClient::presenceFetched, this, &Backend::handlePresence);
    connect(&m_client, &TeamsClient::readHorizonsFetched, this, [this](const QString &cid, const QJsonObject &o) {
        if (cid != m_currentConversationId) return;
        // {"consumptionhorizons": [{"id": <MRI>, "consumptionhorizon": "<message id>;<time>;<client id>"}]}
        QList<qint64> others;
        const QString self = selfMri();
        for (const auto &v : o.value(QLatin1String("consumptionhorizons")).toArray()) {
            const QJsonObject h = v.toObject();
            if (h.value(QLatin1String("id")).toString() == self) continue;
            others << h.value(QLatin1String("consumptionhorizon")).toString().section(QLatin1Char(';'), 0, 0).toLongLong();
        }
        m_messages.setReadHorizons(others);
    });
    // Activity can be reported only once the device is registered.
    connect(&m_client, &TeamsClient::presenceEndpointRegistered, this, [this] {
        m_reportedActive = !(QGuiApplication::applicationState() == Qt::ApplicationActive || m_idleTimer.isActive());
        updateActivity();
    });
    connect(&m_client, &TeamsClient::usersFound, this, [this](const QString &, const QJsonArray &users) {
        QVariantList results;
        const QString self = selfMri();
        for (const auto &v : users) {
            QJsonObject u = v.toObject();
            if (u.contains(QLatin1String("nodeProfileData"))) u = u.value(QLatin1String("nodeProfileData")).toObject();
            const QString mri = u.value(QLatin1String("mri")).toString();
            if (mri.isEmpty() || mri == self) continue;
            const QString name = u.value(QLatin1String("displayName")).toString();
            if (!name.isEmpty()) {
                QString email = u.value(QLatin1String("email")).toString();
                if (email.isEmpty()) email = u.value(QLatin1String("userPrincipalName")).toString();
                m_people.insert(mri, {name, u.value(QLatin1String("givenName")).toString(), email,
                                      u.value(QLatin1String("jobTitle")).toString(), u.value(QLatin1String("department")).toString()});
            }
            results << QVariantMap{{QStringLiteral("mri"), mri}, {QStringLiteral("name"), name},
                                   {QStringLiteral("email"), u.value(QLatin1String("email")).toString()}};
        }
        m_peopleResults = results;
        emit peopleResultsChanged();
    });
    connect(&m_client, &TeamsClient::chatCreated, this, [this](const QString &id) {
        selectConversation(id, m_pendingChatTitle);
        emit openConversationRequested(id);
        m_client.fetchConversations(); // brings the new chat into the list
    });
    connect(&m_client, &TeamsClient::reactionFailed, this, [this](const QString &cid) {
        // Put back what the server has.
        if (cid == m_currentConversationId) m_client.fetchMessages(cid);
    });

    m_filteredConversations.setSourceModel(&m_conversations);
    m_filteredMessages.setSourceModel(&m_messages);
    m_filteredMessages.setFilterRole(MessageModel::ContentRole);
    m_filteredMessages.setFilterCaseSensitivity(Qt::CaseInsensitive);
    const auto recount = [this] {
        const int n = m_conversations.unreadCount();
        if (n == m_unreadCount) return;
        m_unreadCount = n;
        emit unreadCountChanged();
    };
    connect(&m_conversations, &QAbstractItemModel::dataChanged, this, recount);
    connect(&m_conversations, &QAbstractItemModel::rowsInserted, this, recount);
    connect(&m_conversations, &QAbstractItemModel::rowsRemoved, this, recount);
    connect(&m_conversations, &QAbstractItemModel::modelReset, this, recount);
    m_filteredConversations.setFilterRole(ConversationModel::TitleRole);
    m_filteredConversations.setFilterCaseSensitivity(Qt::CaseInsensitive);

    m_avatarBatch.setSingleShot(true);
    m_avatarBatch.setInterval(250);
    connect(&m_avatars, &AvatarCache::avatarReady, this, [this] { if (!m_avatarBatch.isActive()) m_avatarBatch.start(); });
    connect(&m_avatarBatch, &QTimer::timeout, this, [this] { ++m_avatarRevision; emit avatarsChanged(); });

    m_mediaBatch.setSingleShot(true);
    m_mediaBatch.setInterval(250);
    connect(&m_media, &MediaCache::mediaReady, this, [this] { if (!m_mediaBatch.isActive()) m_mediaBatch.start(); });
    connect(&m_mediaBatch, &QTimer::timeout, this, [this] { ++m_mediaRevision; emit mediaChanged(); });

    // Availability changes slowly: purple-teams polls it every 60 s too.
    m_presenceTimer.setInterval(std::chrono::seconds(60));
    connect(&m_presenceTimer, &QTimer::timeout, this, &Backend::pollPresence);

    connect(&m_client, &TeamsClient::imageUploading, this, [this](bool busy) { m_uploading = busy; emit uploadingChanged(); });
    connect(&m_files, &FileUploader::progress, this, [this](double f) { m_uploadProgress = f; emit uploadingChanged(); });
    connect(&m_files, &FileUploader::failed, this, [this](const QString &message) { setError(message); emit uploadingChanged(); });
    connect(&m_files, &FileUploader::uploaded, this, [this](const QJsonObject &entry) {
        emit uploadingChanged();
        // properties.files is a JSON array encoded as a string, as Teams sends it.
        const QString files = QString::fromUtf8(QJsonDocument(QJsonArray{entry}).toJson(QJsonDocument::Compact));
        m_client.sendMessage(m_uploadConversation, QString(), m_selfDisplayName, m_uploadReplyTo,
                             {{QStringLiteral("files"), files}});
    });
    connect(&m_nativeCall, &NativeCall::stateChanged, this, &Backend::nativeCallChanged);
    connect(&m_nativeCall, &NativeCall::activeChanged, this, &Backend::nativeCallChanged);
    connect(&m_nativeCall, &NativeCall::videoChanged, this, &Backend::nativeCallChanged);
    connect(&m_nativeCall, &NativeCall::stateChanged, this, [this] {
        // A video call: the camera goes on as soon as the media transport is up.
        if (m_cameraWhenConnected && m_nativeCall.mediaConnected()) { m_cameraWhenConnected = false; m_nativeCall.setCameraEnabled(true); }
        if (!m_nativeCall.isActive()) m_cameraWhenConnected = false;
    });
    // Screen sharing: the portal's stream goes into the call; the call ending drops it.
    connect(&m_screenCast, &ScreenCastPortal::ready, this, [this](int fd, unsigned node) {
        if (m_nativeCall.isActive()) m_nativeCall.startScreenShare(fd, node);
        else { ::close(fd); m_screenCast.stop(); }
        emit nativeCallChanged();
    });
    connect(&m_screenCast, &ScreenCastPortal::failed, this, [this](const QString &message) { setError(message); emit nativeCallChanged(); });
    connect(&m_screenCast, &ScreenCastPortal::closed, this, [this] { m_nativeCall.stopScreenShare(); emit nativeCallChanged(); });
    connect(&m_nativeCall, &NativeCall::activeChanged, this, [this] { if (!m_nativeCall.isActive()) m_screenCast.stop(); });
    connect(&m_trouter, &Trouter::callAgentRequest, this, [this](const QString &url, const QJsonObject &body) {
        m_nativeCall.handleCallback(url, body);
    });
    connect(&m_trouter, &Trouter::eventMessage, this, &Backend::handleEvent);
    connect(&m_trouter, &Trouter::presence, this, &Backend::handlePresence);
    connect(&m_trouter, &Trouter::incomingCall, this, [this](const QString &mri, const QString &name, const QJsonObject &notification) {
        const QString who = !name.isEmpty() ? name : m_people.value(mri).displayName;
        // Native ringing: attach to the call, play the ringtone, offer answer/decline.
        m_nativeCall.incoming({m_client.tokens().skypeToken, selfMri(), m_selfDisplayName, m_trouter.endpointId(), m_trouter.surl()}, notification);
        if (!m_nativeCall.isIncoming()) return;
        auto *n = new KNotification(QStringLiteral("incomingCall"), KNotification::Persistent);
        n->setComponentName(QStringLiteral("kteams"));
        m_ringNotification = n;
        n->setTitle(tr("Incoming call"));
        n->setText(tr("%1 is calling you").arg(who.isEmpty() ? tr("Someone") : who.toHtmlEscaped()));
        n->setIconName(QStringLiteral("phone"));
        setNotificationPicture(n, mri);
        n->setUrgency(KNotification::CriticalUrgency);
        KNotificationAction *answer = n->addDefaultAction(QStringLiteral("Rispondi"));
        connect(answer, &KNotificationAction::activated, this, [this] { m_nativeCall.accept(); });
        KNotificationAction *decline = n->addAction(QStringLiteral("Rifiuta"));
        connect(decline, &KNotificationAction::activated, this, [this] { m_nativeCall.reject(); });
        n->sendEvent();
    });
    connect(&m_nativeCall, &NativeCall::incomingChanged, this, [this] {
        if (!m_nativeCall.isIncoming() && m_ringNotification) m_ringNotification->close();
        emit nativeCallChanged();
    });
    connect(&m_trouter, &Trouter::connectedChanged, this, [this] { emit realtimeChanged(); scheduleSync(); });
    // KTEAMS_AUTOTEST=1: an unattended native test call (echo bot) once the push channel is
    // up, hung up after 45 s; the stats in the log tell whether audio flowed both ways.
    connect(&m_trouter, &Trouter::connectedChanged, this, [this](bool connected) {
        static bool done = false;
        if (!connected || done || !qEnvironmentVariableIsSet("KTEAMS_AUTOTEST")) return;
        done = true;
        QTimer::singleShot(std::chrono::seconds(3), this, [this] {
            qInfo() << "autotest: starting the native test call";
            startNativeTestCall();
            QTimer::singleShot(std::chrono::seconds(45), this, [this] { qInfo() << "autotest: hanging up"; hangUpNativeCall(); });
        });
    });
    m_eventSync.setSingleShot(true);
    m_eventSync.setInterval(300);
    connect(&m_eventSync, &QTimer::timeout, this, &Backend::sync);
    m_typingExpire.setSingleShot(true);
    m_typingExpire.setInterval(std::chrono::seconds(6));
    connect(&m_typingExpire, &QTimer::timeout, this, [this] { m_typingText.clear(); emit typingChanged(); });

    m_syncTimer.setSingleShot(true);
    connect(&m_syncTimer, &QTimer::timeout, this, &Backend::sync);
    // Away after this long without using the window (the timer runs while it is in the background).
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(std::chrono::minutes(5));
    connect(&m_idleTimer, &QTimer::timeout, this, &Backend::updateActivity);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState s) {
        if (s == Qt::ApplicationActive) m_idleTimer.stop();
        else m_idleTimer.start();
        updateActivity();
        if (s != Qt::ApplicationActive) return;
        // Back to the window: catch up now and mark the open chat as read.
        sync();
        markCurrentRead();
    });

    m_skypeRenewTimer.setSingleShot(true);
    connect(&m_skypeRenewTimer, &QTimer::timeout, this, &Backend::connectChat);

    m_cacheSaveTimer.setSingleShot(true);
    m_cacheSaveTimer.setInterval(std::chrono::seconds(3));
    connect(&m_cacheSaveTimer, &QTimer::timeout, this, &Backend::saveStateCache);
    loadStateCache();
    loadSearchIndex();
    m_indexTimeout.setSingleShot(true);
    m_indexTimeout.setInterval(std::chrono::seconds(8));
    connect(&m_indexTimeout, &QTimer::timeout, this, [this] { if (!m_indexQueue.isEmpty()) { m_indexQueue.removeFirst(); indexNext(); } });

    m_oauth.loadStoredSession();
}

void Backend::setState(State s) { if (m_state == s) return; m_state = s; emit stateChanged(); }

void Backend::setStatus(const QString &s)
{
    if (m_status == s) return;
    m_status = s;
    qInfo().noquote() << "status:" << s;
    emit statusMessageChanged();
}

void Backend::setError(const QString &e)
{
    if (!e.isEmpty()) qWarning().noquote() << "error:" << e;
    if (m_error == e) return;
    m_error = e;
    emit errorMessageChanged();
}

void Backend::startLogin()
{
    setError({});
    m_oauth.startDeviceLogin();
}

void Backend::cancelLogin()
{
    m_oauth.cancelDeviceLogin();
    setState(State::SignedOut);
}

void Backend::copyToClipboard(const QString &text)
{
    QGuiApplication::clipboard()->setText(text);
}

void Backend::connectChat()
{
    // Also used to renew the skypetoken: only the first time it changes what the UI shows.
    if (m_state != State::Connected) setState(State::Connecting);
    setStatus(QStringLiteral("Connessione..."));
    m_oauth.accessToken(QStringLiteral("https://api.spaces.skype.com"), [this](const QString &token) {
        m_client.fetchSkypeToken(token);
    });
}

void Backend::onSkypeToken(const QString &token, const QString &region, int expiresInSecs)
{
    TeamsTokens t = m_client.tokens();
    t.skypeToken = token;
    t.region = region;
    m_client.setTokens(t);
    m_media.setSkypeToken(token);
    m_trouter.setSkypeToken(token);
    // Renew 5 minutes early; authsvc normally grants about a day.
    if (expiresInSecs > 600) m_skypeRenewTimer.start(std::chrono::seconds(expiresInSecs - 300));

    if (m_state == State::Connected) return;
    setState(State::Connected);
    m_trouter.start(token, [this](std::function<void(const QString &)> cb) {
        m_oauth.accessToken(QStringLiteral("https://api.spaces.skype.com"), std::move(cb));
    });
    // This device counts as an active Teams client (otherwise you look offline).
    presenceToken([this](const QString &t) { m_client.registerPresenceEndpoint(m_trouter.endpointId(), t); });
    setError({});
    setStatus(tr("Loading profile..."));
    // Conversations are loaded once we know who "self" is (handleSelf), since
    // one-to-one chat titles depend on it.
    m_client.fetchSelfProperties();
}

void Backend::onAuthFailed(const QString &message, bool sessionLost)
{
    setError(message);
    if (sessionLost) {
        resetChatState();
        setState(State::SignedOut);
    } else if (m_state == State::WaitingForCode || m_state == State::Starting) {
        setState(State::SignedOut);
    }
}

void Backend::onClientError(const QString &context, int httpStatus, const QString &message)
{
    // A page fetched for the search index only: skip that chat quietly, go on with the next.
    if (!m_indexQueue.isEmpty() && message.contains(QString::fromLatin1(QUrl::toPercentEncoding(m_indexQueue.first())))) {
        m_indexQueue.removeFirst();
        indexNext();
        return;
    }
    // Without the profile the list still works, only one-to-one titles are less precise.
    if (context == QLatin1String("profile")) m_client.fetchConversations();
    // The device registration expired (purple-teams notes this too): register it again.
    if (context == QLatin1String("presence (activity)") && httpStatus == 404) {
        presenceToken([this](const QString &t) { m_client.registerPresenceEndpoint(m_trouter.endpointId(), t); });
        return;
    }
    // Names and teams: a dropped connection (no HTTP status) is worth another try.
    const bool peopleCall = context == QLatin1String("profiles") || context == QLatin1String("teams and chats");
    if (peopleCall && httpStatus == 0 && m_peopleRetries < 2) {
        ++m_peopleRetries;
        QTimer::singleShot(std::chrono::seconds(3 * m_peopleRetries), this, &Backend::loadPeople);
        return;
    }
    if (httpStatus == 401 && context != QLatin1String("authsvc")) {
        setStatus(tr("Chat session expired, renewing..."));
        connectChat();
        return;
    }
    if (m_state == State::Connecting) setState(State::SignedOut);
    setError(QStringLiteral("%1%2: %3")
                 .arg(context, httpStatus ? QStringLiteral(" (HTTP %1)").arg(httpStatus) : QString(), message));
}

void Backend::resetChatState()
{
    m_peopleLoaded = false;
    m_indexedAll = false;
    m_indexQueue.clear();
    m_searchIndex.clear();
    m_skypeRenewTimer.stop();
    m_syncTimer.stop();
    m_lastSyncMs = 0;
    m_chatMembers.clear();
    m_people.clear();
    m_channelIds.clear();
    m_channels.setItems({});
    m_calendar.setItems({});
    m_avatars.clear();
    m_media.clear();
    m_presenceTimer.stop();
    m_idleTimer.stop();
    m_reportedActive = false;
    m_presence.clear();
    m_trouter.stop();
    m_typingText.clear();
    m_lastTypingSent.clear();
    m_peopleResults.clear();
    m_client.setTokens({});
    m_conversations.setItems({});
    m_messages.setItems({});
    m_currentConversationId.clear();
    m_currentConversationTitle.clear();
    m_selfSkypeName.clear();
    m_selfDisplayName.clear();
    emit currentConversationChanged();
    emit selfChanged();
}

void Backend::setConversationFilter(const QString &text)
{
    m_filteredConversations.setFilterFixedString(text.trimmed());
}

void Backend::refresh()
{
    if (m_state != State::Connected) return;
    setStatus(QStringLiteral("Aggiornamento..."));
    m_client.fetchConversations();
    if (!m_currentConversationId.isEmpty()) m_client.fetchMessages(m_currentConversationId);
}

void Backend::logout()
{
    m_oauth.signOut();
    QDir(cacheDir()).removeRecursively(); // nothing of the account stays on disk
    resetChatState();
    setError({});
    setStatus({});
    setState(State::SignedOut);
}

void Backend::setChatFilter(const QString &text)
{
    if (m_chatFilter == text) return;
    m_chatFilter = text;
    m_filteredMessages.setFilterFixedString(text);
    emit chatFilterChanged();
}

void Backend::selectConversation(const QString &id, const QString &fallbackTitle)
{
    if (id != m_currentConversationId) setChatFilter(QString());
    if (id == m_currentConversationId) return;
    m_currentConversationId = id;
    if (const ChannelItem *ch = m_channels.find(id)) {
        m_currentConversationTitle = ch->teamName + QStringLiteral(" › ") + ch->name;
    } else {
        const ConversationItem *conv = m_conversations.find(id);
        m_currentConversationTitle = conv ? conv->title : (fallbackTitle.isEmpty() ? id : fallbackTitle);
    }
    m_messages.setItems({});
    loadMessagesCache(id);
    m_replies.clear();
    m_oldestLoadedMs = 0;
    m_hasMoreHistory = false;
    m_loadingOlder = false;
    m_autoFillPages = 0;
    emit historyChanged();
    closeThread();
    emit currentConversationChanged();
    setStatus(tr("Loading messages..."));
    m_client.fetchMessages(id);
    if (!m_channelIds.contains(id)) m_client.fetchReadHorizons(id);
}

void Backend::sendMessage(const QString &text)
{
    if (m_currentConversationId.isEmpty() || text.trimmed().isEmpty()) return;
    m_client.sendMessage(m_currentConversationId, quoteHtml() + textToHtml(text), m_selfDisplayName);
}

void Backend::handleSelf(const QJsonObject &props)
{
    // purple-teams: teams_got_self_details(); userDetails is a JSON object encoded as a string.
    m_selfSkypeName = props.value(QLatin1String("skypeName")).toString();
    const QJsonObject details = QJsonDocument::fromJson(props.value(QLatin1String("userDetails")).toString().toUtf8()).object();
    QString name = details.value(QLatin1String("name")).toString();
    if (name.isEmpty() || name == m_selfSkypeName) name = details.value(QLatin1String("upn")).toString();
    m_selfDisplayName = name;
    m_selfUpn = details.value(QLatin1String("upn")).toString();
    emit selfChanged();
    m_avatars.configure(selfMri(), m_client.tokens().region.isEmpty() ? QStringLiteral("apac") : m_client.tokens().region,
                        [this](std::function<void(const QString &)> cb) {
                            m_oauth.accessToken(QStringLiteral("https://api.spaces.skype.com"), std::move(cb));
                        });
    setStatus(tr("Loading conversations..."));
    m_client.fetchConversations();
}

bool Backend::isSelf(const QString &fromUrl) const
{
    // "from" is a contact URL ending in the MRI, e.g. ".../contacts/8:orgid:<guid>".
    return !m_selfSkypeName.isEmpty() && fromUrl.endsWith(m_selfSkypeName);
}

bool Backend::parseConversation(const QJsonObject &o, ConversationItem &it, bool &lastFromOther) const
{
    it.id = o.value(QLatin1String("id")).toString();
    if (it.id.isEmpty()) return false;
    // System conversations (notifications, call logs...), except personal notes.
    if (it.id.startsWith(QLatin1String("48:")) && it.id != QLatin1String("48:notes")) return false;

    const QJsonObject thread = o.value(QLatin1String("threadProperties")).toObject();
    const QJsonObject last = o.value(QLatin1String("lastMessage")).toObject();
    const QJsonObject props = o.value(QLatin1String("properties")).toObject();
    const bool oneToOne = thread.value(QLatin1String("uniquerosterthread")).toString() == QLatin1String("true")
        || thread.value(QLatin1String("productThreadType")).toString() == QLatin1String("OneToOneChat");
    const QString lastFrom = last.value(QLatin1String("from")).toString();
    it.lastFromMri = lastFrom.section(QLatin1Char('/'), -1);
    const bool chatMessage = isChatMessage(last.value(QLatin1String("messagetype")).toString());
    lastFromOther = chatMessage && !lastFrom.isEmpty() && !isSelf(lastFrom);

    it.type = oneToOne ? QStringLiteral("oneToOne") : thread.value(QLatin1String("threadType")).toString();
    it.title = thread.value(QLatin1String("topic")).toString();
    it.hasTopic = !it.title.isEmpty();
    if (it.title.isEmpty() && it.id == QLatin1String("48:notes")) it.title = tr("Personal notes");
    if (it.title.isEmpty()) it.title = titleFromMembers(it.id, oneToOne);
    if (oneToOne) it.peerMri = m_chatMembers.value(it.id).value(0);
    if (it.title.isEmpty() && oneToOne && lastFromOther) it.title = last.value(QLatin1String("imdisplayname")).toString();
    if (it.title.isEmpty()) it.title = oneToOne ? tr("Chat") : tr("Group chat");

    if (chatMessage) {
        const QString who = last.value(QLatin1String("imdisplayname")).toString();
        const ParsedContent parsed = parseContent(last.value(QLatin1String("content")).toString());
        QString text = parsed.text.simplified();
        const QVariantList files = parseFiles(last.value(QLatin1String("properties")).toObject());
        if (text.isEmpty() && !files.isEmpty()) text = QStringLiteral("📎 ") + files.first().toMap().value(QStringLiteral("name")).toString();
        if (text.isEmpty() && !parsed.images.isEmpty()) text = tr("📷 Picture");
        it.preview = (who.isEmpty() || oneToOne ? QString() : who + QStringLiteral(": ")) + text;
    }
    it.lastActivity = parseTime(last.value(QLatin1String("composetime")).toString());
    it.lastMessageId = last.value(QLatin1String("id")).toString();

    // Pinned ("favorite"), muted ("alerts" off) and hidden chats: properties of the
    // conversation. Logged once while the names are being confirmed against Teams.
    it.pinned = props.value(QLatin1String("favorite")).toVariant().toString() == QLatin1String("true");
    it.muted = props.value(QLatin1String("alerts")).toVariant().toString() == QLatin1String("false");
    static int logged = 0;
    if (logged < 3 && !props.isEmpty()) { ++logged; qInfo() << "conversation properties:" << props.keys(); }
    if (props.value(QLatin1String("hidden")).toVariant().toString() == QLatin1String("true")) return false;
    // consumptionhorizon = "<last read message id>;<time>;<clientmessageid>"; message ids grow over time.
    const QString horizon = props.value(QLatin1String("consumptionhorizon")).toString().section(QLatin1Char(';'), 0, 0);
    it.unread = lastFromOther && !horizon.isEmpty() && !it.lastMessageId.isEmpty()
        && it.lastMessageId.toLongLong() > horizon.toLongLong();
    return true;
}

void Backend::handleConversations(const QJsonArray &arr, bool incremental)
{
    if (!incremental) {
        QVector<ConversationItem> items;
        items.reserve(arr.size());
        for (const auto &v : arr) {
            ConversationItem it;
            bool fromOther = false;
            if (!parseConversation(v.toObject(), it, fromOther)) continue;
            m_lastSyncMs = qMax(m_lastSyncMs, it.lastActivity);
            if (m_channelIds.contains(it.id)) m_channels.updateActivity(it.id, it.lastActivity, it.unread);
            else items.push_back(it);
        }
        m_conversations.setItems(items);
        setStatus(QStringLiteral("%1 conversazioni").arg(items.size()));
        scheduleSync();
        m_cacheSaveTimer.start();
        // Names, members and teams once per session (the cache has them too, but the
        // presence polling and fresh members start from here).
        if (!m_peopleLoaded) { m_peopleLoaded = true; loadPeople(); }
        // Demo mode: a chat open from the start, for screenshots.
        if (Demo::enabled() && !items.isEmpty() && m_currentConversationId.isEmpty()) {
            QString first = items.first().id;
            for (const ConversationItem &c : std::as_const(items)) if (!c.preview.isEmpty() && c.type != QLatin1String("oneToOne")) { first = c.id; break; }
            QTimer::singleShot(std::chrono::seconds(2), this, [this, first] { emit openConversationRequested(first); });
        }
        m_client.fetchMessages(QString::fromLatin1(kActivityFeed));
        return;
    }

    // Only messages newer than what we had synced are news; the server also returns
    // conversations changed in other ways (read elsewhere, roster), possibly many we had
    // not loaded (the first load holds 100), and those must not notify.
    const qint64 newerThan = m_lastSyncMs;
    for (const auto &v : arr) {
        ConversationItem it;
        bool fromOther = false;
        if (!parseConversation(v.toObject(), it, fromOther)) continue;
        m_lastSyncMs = qMax(m_lastSyncMs, it.lastActivity);
        const bool reallyNew = it.lastActivity > newerThan;
        const bool watchingThis = it.id == m_currentConversationId && QGuiApplication::applicationState() == Qt::ApplicationActive;

        if (m_channelIds.contains(it.id)) {
            // Channels: no popup for every post (Teams only notifies mentions there).
            const ChannelItem *ch = m_channels.find(it.id);
            const bool changed = !ch || ch->lastActivity != it.lastActivity;
            const QJsonObject lastProps = v.toObject().value(QLatin1String("lastMessage")).toObject()
                                              .value(QLatin1String("properties")).toObject();
            if (changed && reallyNew && fromOther && !watchingThis && mentionsSelf(lastProps) && ch) {
                ConversationItem mention = it;
                mention.title = ch->teamName + QStringLiteral(" › ") + ch->name;
                notifyNewMessage(mention); // Teams notifies channel posts only when you are mentioned
            }
            m_channels.updateActivity(it.id, it.lastActivity, it.unread && !watchingThis);
            if (changed && it.id == m_currentConversationId) m_client.fetchMessages(it.id);
            continue;
        }

        const ConversationItem *old = m_conversations.find(it.id);
        const bool newMessage = !old || old->lastMessageId != it.lastMessageId;
        if (old && (it.title == tr("Chat") || it.title == tr("Group chat")))
            it.title = old->title; // the last message is ours: keep the name we already know
        const bool open = it.id == m_currentConversationId;
        const bool watching = open && QGuiApplication::applicationState() == Qt::ApplicationActive;
        if (watching) it.unread = false;
        if (!newMessage && old && old->unread == it.unread && old->preview == it.preview) continue;

        m_conversations.upsert(it);
        if (!newMessage) continue;
        if (open) m_client.fetchMessages(it.id);
        if (reallyNew && fromOther && !watching) notifyNewMessage(it);
    }
}

QVector<MessageItem> Backend::parseMessages(const QJsonArray &arr, qint64 *oldestRaw) const
{
    // Paging works on all messages (system events included): the next page ends before the oldest.
    QVector<MessageItem> items;
    items.reserve(arr.size());
    // The API returns newest first, which is the order MessageModel wants.
    for (const auto &v : arr) {
        const QJsonObject o = v.toObject();
        const qint64 ts = parseTime(o.value(QLatin1String("composetime")).toString(o.value(QLatin1String("originalarrivaltime")).toString()));
        if (oldestRaw && ts > 0 && (*oldestRaw == 0 || ts < *oldestRaw)) *oldestRaw = ts;

        const QString type = o.value(QLatin1String("messagetype")).toString();
        if (!isChatMessage(type)) continue;
        MessageItem m;
        m.id = o.value(QLatin1String("id")).toString();
        m.clientMessageId = o.value(QLatin1String("clientmessageid")).toString();
        m.rootId = o.value(QLatin1String("rootMessageId")).toString();
        const QString from = o.value(QLatin1String("from")).toString();
        m.mine = isSelf(from);
        m.fromId = from.section(QLatin1Char('/'), -1);
        m.from = o.value(QLatin1String("imdisplayname")).toString();
        if (m.from.isEmpty()) m.from = from.section(QLatin1Char('/'), -1);
        const QString content = o.value(QLatin1String("content")).toString();
        if (type == QLatin1String("Text")) {
            m.content = content;
        } else {
            const ParsedContent parsed = parseContent(content);
            m.richText = parsed.richText;
            m.content = parsed.text;
            m.images = parsed.images;
        }
        if (m.richText.isEmpty()) m.richText = linkify(m.content);
        m.files = parseFiles(o.value(QLatin1String("properties")).toObject());
        const QJsonObject props = o.value(QLatin1String("properties")).toObject();
        m.reactions = parseReactions(props);
        // purple-teams: "deletetime" / "edittime" (or skypeeditedid) in the message
        m.deleted = props.contains(QLatin1String("deletetime"));
        m.edited = !m.deleted && (props.contains(QLatin1String("edittime")) || o.contains(QLatin1String("skypeeditedid")));
        m.mentionsMe = mentionsSelf(props);
        if (m.deleted) {
            m.content = tr("This message was deleted.");
            m.richText = m.content.toHtmlEscaped();
            m.images.clear();
            m.files.clear();
        }
        m.time = o.value(QLatin1String("composetime")).toString(o.value(QLatin1String("originalarrivaltime")).toString());
        m.timestamp = ts;
        if (!m.content.isEmpty() || !m.images.isEmpty() || !m.files.isEmpty()) items.push_back(m);
    }
    return items;
}

void Backend::handleMessages(const QString &conversationId, const QJsonArray &arr, bool older)
{
    if (conversationId == QLatin1String(kActivityFeed)) { handleActivity(arr); return; }
    const bool current = conversationId == m_currentConversationId;
    const bool channel = current ? currentIsChannel() : m_channelIds.contains(conversationId);

    qint64 oldestRaw = 0;
    QVector<MessageItem> items = parseMessages(arr, &oldestRaw);
    if (!current) {
        // A page fetched for the search index only (no chat open on it).
        if (!channel) { m_searchIndex.insert(conversationId, items); saveMessagesCache(conversationId, items); }
        if (!m_indexQueue.isEmpty() && m_indexQueue.first() == conversationId) { m_indexQueue.removeFirst(); indexNext(); }
        return;
    }

    if (older || m_oldestLoadedMs == 0) {
        // No progress means the server ignored endTime: stop instead of looping.
        const bool progress = oldestRaw > 0 && (m_oldestLoadedMs == 0 || oldestRaw < m_oldestLoadedMs);
        if (progress) m_oldestLoadedMs = oldestRaw;
        m_hasMoreHistory = progress && arr.size() >= TeamsClient::kHistoryPageSize;
    }
    if (older) m_loadingOlder = false;
    emit historyChanged();

    // Channels: the conversation shows posts; replies go to their post's thread.
    if (channel) {
        QVector<MessageItem> posts;
        QSet<QString> touched;
        for (const auto &m : std::as_const(items)) {
            if (m.rootId.isEmpty() || m.rootId == m.id) { posts.push_back(m); continue; }
            auto &replies = m_replies[m.rootId];
            if (std::none_of(replies.cbegin(), replies.cend(), [&](const MessageItem &r) { return r.id == m.id; })) {
                replies.push_back(m);
                touched.insert(m.rootId);
            }
        }
        for (auto &p : posts) p.replyCount = int(m_replies.value(p.id).size());
        items = posts;
        for (const QString &postId : std::as_const(touched)) {
            auto &replies = m_replies[postId];
            std::sort(replies.begin(), replies.end(), [](const MessageItem &a, const MessageItem &b) { return a.timestamp > b.timestamp; });
            m_messages.setReplyCount(postId, int(replies.size()));
        }
        if (touched.contains(m_currentThreadId)) refreshThread();
    }

    if (older) m_messages.appendOlder(items);
    else if (m_messages.items().isEmpty() || m_messagesFromCache) m_messages.setItems(items);
    else m_messages.merge(items);
    m_messagesFromCache = false;
    if (!channel) {
        m_searchIndex.insert(conversationId, m_messages.items());
        if (!older) saveMessagesCache(conversationId, m_messages.items());
        if (!m_searchQuery.isEmpty()) runSearch();
    }

    if (m_hasMoreHistory && m_messages.rowCount() < kMinVisibleMessages && m_autoFillPages < kMaxAutoFillPages) {
        ++m_autoFillPages;
        loadOlder();
    }
    markCurrentRead();
}

void Backend::loadOlder()
{
    if (m_currentConversationId.isEmpty() || m_loadingOlder || !m_hasMoreHistory) return;
    m_loadingOlder = true;
    emit historyChanged();
    m_client.fetchMessages(m_currentConversationId, m_oldestLoadedMs);
}

void Backend::openThread(const QString &postId)
{
    m_currentThreadId = postId;
    refreshThread();
    emit currentThreadChanged();
}

void Backend::closeThread()
{
    if (m_currentThreadId.isEmpty()) return;
    m_currentThreadId.clear();
    m_thread.setItems({});
    emit currentThreadChanged();
}

void Backend::refreshThread()
{
    if (m_currentThreadId.isEmpty()) return;
    // Newest first, the post itself last: shown bottom-to-top it reads post, then replies.
    QVector<MessageItem> items = m_replies.value(m_currentThreadId);
    for (const auto &m : m_messages.items()) {
        if (m.id != m_currentThreadId) continue;
        items.push_back(m);
        break;
    }
    m_thread.setItems(items);
}

void Backend::sendReply(const QString &text)
{
    if (m_currentConversationId.isEmpty() || m_currentThreadId.isEmpty() || text.trimmed().isEmpty()) return;
    m_client.sendMessage(m_currentConversationId, textToHtml(text), m_selfDisplayName, m_currentThreadId);
}

void Backend::markCurrentRead()
{
    if (QGuiApplication::applicationState() != Qt::ApplicationActive) return;
    if (!isUnread(m_currentConversationId) || m_messages.items().isEmpty()) return;
    const MessageItem &newest = m_messages.items().first();
    m_client.markRead(m_currentConversationId, newest.id, newest.clientMessageId);
    m_conversations.setUnread(m_currentConversationId, false);
    if (const ChannelItem *ch = m_channels.find(m_currentConversationId))
        m_channels.updateActivity(ch->id, ch->lastActivity, false);
}

void Backend::scheduleSync()
{
    if (m_state != State::Connected) return;
    // Quick while the user is looking, sparing in the background.
    const bool active = QGuiApplication::applicationState() == Qt::ApplicationActive;
    // With Trouter pushing changes, polling is only a safety net.
    const bool pushed = m_trouter.isConnected();
    const int secs = pushed ? (active ? kSyncActivePushedSecs : kSyncBackgroundPushedSecs)
                            : (active ? kSyncActiveSecs : kSyncBackgroundSecs);
    m_syncTimer.start(std::chrono::seconds(secs));
}

void Backend::sync()
{
    if (m_state != State::Connected) return;
    // A second of overlap: the server compares with its own clock and ms precision.
    m_client.fetchConversations(qMax<qint64>(m_lastSyncMs - 1000, 1));
    // Reactions and edits do not change the conversation list: refresh the open
    // conversation every few ticks while the user looks at it.
    if (++m_syncTicks % 3 == 0 && !m_currentConversationId.isEmpty()
        && QGuiApplication::applicationState() == Qt::ApplicationActive)
    {
        m_client.fetchMessages(m_currentConversationId);
        if (!currentIsChannel()) m_client.fetchReadHorizons(m_currentConversationId);
    }
    scheduleSync();
}

void Backend::notifyNewMessage(const ConversationItem &conv)
{
    // Our own events (data/kteams.notifyrc): popup and sound, user-configurable.
    const bool mention = conv.title.contains(QStringLiteral(" › ")); // channel mention (see handleConversations)
    if (conv.muted || doNotDisturb()) { qInfo() << "notify: skipped (muted or do not disturb)" << conv.title; return; }
    qInfo() << "notify:" << (mention ? "mention" : "message") << "from" << conv.title;
    auto *n = new KNotification(mention ? QStringLiteral("mention") : QStringLiteral("newMessage"), KNotification::CloseOnTimeout);
    n->setComponentName(QStringLiteral("kteams"));
    n->setTitle(conv.title);
    n->setText(conv.preview.toHtmlEscaped());
    n->setIconName(QStringLiteral("im-user"));
    setNotificationPicture(n, !conv.peerMri.isEmpty() ? conv.peerMri : conv.lastFromMri);
    const QString id = conv.id;
    KNotificationAction *open = n->addDefaultAction(tr("Open"));
    connect(open, &KNotificationAction::activated, this, [this, id] { emit openConversationRequested(id); });
    n->sendEvent();
}

void Backend::loadPeople()
{
    m_oauth.accessToken(QStringLiteral("https://chatsvcagg.teams.microsoft.com"),
                        [this](const QString &token) { m_client.fetchChatsAndTeams(token); });
}

void Backend::handleChatsAndTeams(const QJsonObject &summary)
{
    const QString self = selfMri();
    QStringList unknown;
    for (const auto &v : summary.value(QLatin1String("chats")).toArray()) {
        const QJsonObject chat = v.toObject();
        QStringList others;
        for (const auto &m : chat.value(QLatin1String("members")).toArray()) {
            const QString mri = m.toObject().value(QLatin1String("mri")).toString();
            if (mri.isEmpty() || mri == self) continue;
            others << mri;
            if (!m_people.contains(mri) && !unknown.contains(mri)) unknown << mri;
        }
        m_chatMembers.insert(chat.value(QLatin1String("id")).toString(), others);
    }
    parseTeams(summary.value(QLatin1String("teams")).toArray());
    if (unknown.isEmpty()) { applyMemberTitles(); return; }

    // purple-teams sends up to 900 per request; smaller batches give names sooner.
    constexpr int kBatch = 200;
    m_oauth.accessToken(QStringLiteral("https://api.spaces.skype.com"), [this, unknown](const QString &token) {
        for (int i = 0; i < unknown.size(); i += kBatch)
            m_client.fetchProfiles(unknown.mid(i, kBatch), token);
    });
}

void Backend::handleProfiles(const QJsonArray &profiles)
{
    for (const auto &v : profiles) {
        const QJsonObject p = v.toObject();
        const QString mri = p.value(QLatin1String("mri")).toString();
        if (mri.isEmpty()) continue;
        QString email = p.value(QLatin1String("email")).toString();
        if (email.isEmpty()) email = p.value(QLatin1String("userPrincipalName")).toString();
        m_people.insert(mri, {p.value(QLatin1String("displayName")).toString(), p.value(QLatin1String("givenName")).toString(), email,
                              p.value(QLatin1String("jobTitle")).toString(), p.value(QLatin1String("department")).toString()});
    }
    qInfo() << "people:" << m_people.size() << "names for" << m_chatMembers.size() << "chats";
    applyMemberTitles();
    m_cacheSaveTimer.start();
    if (!m_presenceTimer.isActive()) { pollPresence(); m_presenceTimer.start(); }
}

QString Backend::titleFromMembers(const QString &conversationId, bool oneToOne) const
{
    const auto members = m_chatMembers.constFind(conversationId);
    if (members == m_chatMembers.cend() || members->isEmpty()) return {};

    QStringList names;
    for (const QString &mri : *members) {
        const auto person = m_people.constFind(mri);
        if (person == m_people.cend() || person->displayName.isEmpty()) continue;
        if (oneToOne) return person->displayName;
        names << (person->givenName.isEmpty() ? person->displayName.section(QLatin1Char(' '), 0, 0) : person->givenName);
    }
    if (names.isEmpty()) return {};
    const qsizetype total = members->size();
    if (total > 3) return QStringLiteral("%1, %2, +%3").arg(names.value(0), names.value(1)).arg(total - 2);
    if (names.size() == 1) return names.first();
    return names.mid(0, names.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(" e ") + names.last();
}

void Backend::applyMemberTitles()
{
    for (auto it = m_chatMembers.cbegin(); it != m_chatMembers.cend(); ++it) {
        const ConversationItem *conv = m_conversations.find(it.key());
        if (!conv || conv->hasTopic) continue;
        if (conv->type == QLatin1String("oneToOne")) m_conversations.setPeer(it.key(), it.value().value(0));
        const QString title = titleFromMembers(it.key(), conv->type == QLatin1String("oneToOne"));
        if (title.isEmpty()) continue;
        m_conversations.setTitle(it.key(), title);
        if (it.key() == m_currentConversationId && title != m_currentConversationTitle) {
            m_currentConversationTitle = title;
            emit currentConversationChanged();
        }
    }
}

void Backend::parseTeams(const QJsonArray &teams)
{
    QVector<ChannelItem> items;
    QSet<QString> ids;
    for (const auto &tv : teams) {
        const QJsonObject team = tv.toObject();
        if (team.value(QLatin1String("isArchived")).toBool() || team.value(QLatin1String("isDeleted")).toBool()) continue;
        QVector<ChannelItem> channels;
        for (const auto &cv : team.value(QLatin1String("channels")).toArray()) {
            const QJsonObject ch = cv.toObject();
            if (ch.value(QLatin1String("isDeleted")).toBool() || ch.value(QLatin1String("isArchived")).toBool()) continue;
            if (ch.value(QLatin1String("isMember")).isBool() && !ch.value(QLatin1String("isMember")).toBool()) continue;
            ChannelItem c;
            c.id = ch.value(QLatin1String("id")).toString();
            c.general = ch.value(QLatin1String("isGeneral")).toBool();
            c.name = c.general ? QStringLiteral("Generale") : ch.value(QLatin1String("displayName")).toString();
            c.teamId = team.value(QLatin1String("id")).toString();
            c.teamName = team.value(QLatin1String("displayName")).toString();
            c.groupId = ch.value(QLatin1String("groupId")).toString();
            c.unread = ch.value(QLatin1String("isMessageRead")).isBool() && !ch.value(QLatin1String("isMessageRead")).toBool();
            const QJsonObject last = ch.value(QLatin1String("lastMessage")).toObject();
            c.lastActivity = parseTime(last.value(QLatin1String("composeTime")).toString(last.value(QLatin1String("composetime")).toString()));
            if (!c.id.isEmpty()) channels.push_back(c);
        }
        // As in Teams: General first, then by name.
        std::stable_sort(channels.begin(), channels.end(), [](const ChannelItem &a, const ChannelItem &b) {
            if (a.general != b.general) return a.general;
            return a.name.localeAwareCompare(b.name) < 0;
        });
        for (const auto &c : channels) { items.push_back(c); ids.insert(c.id); }
    }
    m_channelIds = ids;
    m_channels.setItems(items);
    // Channels also come with the conversation list: they belong in the Team view only.
    for (const QString &id : ids) m_conversations.remove(id);
}

bool Backend::isUnread(const QString &id) const
{
    if (const ChannelItem *ch = m_channels.find(id)) return ch->unread;
    const ConversationItem *conv = m_conversations.find(id);
    return conv && conv->unread;
}

void Backend::loadCalendarWeek(int weekOffset)
{
    m_calendarWeekOffset = weekOffset;
    const QDate today = QDate::currentDate();
    const QDate monday = today.addDays(1 - today.dayOfWeek()).addDays(7 * weekOffset);
    m_calendarFrom = QDateTime(monday, QTime(0, 0));
    const QDateTime to = m_calendarFrom.addDays(7);
    m_calendarTitle = QLocale().toString(monday, QStringLiteral("d MMM")) + QStringLiteral(" – ")
        + QLocale().toString(monday.addDays(6), QStringLiteral("d MMM yyyy"));
    emit calendarChanged();
    m_oauth.accessToken(QStringLiteral("https://api.spaces.skype.com"),
                        [this, to](const QString &token) { m_client.fetchCalendar(m_calendarFrom, to, token); });
}

void Backend::handleCalendar(const QJsonArray &events)
{
    QVector<CalendarEvent> items;
    items.reserve(events.size());
    for (const auto &v : events) {
        const QJsonObject e = v.toObject();
        CalendarEvent ev;
        ev.id = e.value(QLatin1String("objectId")).toString(e.value(QLatin1String("iCalUID")).toString());
        ev.subject = e.value(QLatin1String("subject")).toString();
        // "2026-11-12T14:00:00+01:00": the offset is part of the string.
        ev.start = QDateTime::fromString(e.value(QLatin1String("startTime")).toString(), Qt::ISODate);
        ev.end = QDateTime::fromString(e.value(QLatin1String("endTime")).toString(), Qt::ISODate);
        ev.location = e.value(QLatin1String("location")).toString();
        ev.organizer = e.value(QLatin1String("organizerName")).toString();
        ev.online = e.value(QLatin1String("isOnlineMeeting")).toBool();
        ev.chatId = e.value(QLatin1String("skypeTeamsDataObject")).toObject().value(QLatin1String("cid")).toString();
        ev.showAs = e.value(QLatin1String("showAs")).toString();
        ev.joinUrl = e.value(QLatin1String("skypeTeamsMeetingUrl")).toString();
        ev.iCalUid = e.value(QLatin1String("iCalUID")).toString();
        ev.myResponse = e.value(QLatin1String("myResponseType")).toString();
        if (ev.start.isValid()) items.push_back(ev);
    }
    std::sort(items.begin(), items.end(), [](const CalendarEvent &a, const CalendarEvent &b) { return a.start < b.start; });
    m_calendar.setItems(items);
    if (m_calendarWeekOffset == 0) scheduleReminders(items);
}

QVariantList Backend::parseReactions(const QJsonObject &properties) const
{
    // purple-teams (teams_messages.c): "emotions": [{"key": "like", "users": [{"mri", "time", "value"}]}]
    const QJsonValue v = properties.value(QLatin1String("emotions"));
    const QJsonArray emotions = v.isString() ? QJsonDocument::fromJson(v.toString().toUtf8()).array() : v.toArray();
    const QString self = selfMri();
    QVariantList result;
    for (const auto &e : emotions) {
        const QJsonObject o = e.toObject();
        const QJsonArray users = o.value(QLatin1String("users")).toArray();
        if (users.isEmpty()) continue;
        bool mine = false;
        QStringList names;
        for (const auto &u : users) {
            const QString mri = u.toObject().value(QLatin1String("mri")).toString();
            if (mri == self) { mine = true; names << QStringLiteral("Tu"); continue; }
            const QString name = m_people.value(mri).displayName;
            names << (name.isEmpty() ? tr("Someone") : name);
        }
        const QString key = o.value(QLatin1String("key")).toString();
        result << QVariantMap{{QStringLiteral("key"), key}, {QStringLiteral("emoji"), emojiForKey(key)},
                              {QStringLiteral("count"), int(users.size())}, {QStringLiteral("mine"), mine},
                              {QStringLiteral("names"), names.join(QStringLiteral(", "))}};
    }
    return result;
}

QVariantList Backend::quickReactions() const
{
    QVariantList list;
    for (const char *key : {"like", "heart", "laugh", "surprised", "sad", "angry"}) {
        const QString k = QString::fromLatin1(key);
        list << QVariantMap{{QStringLiteral("key"), k}, {QStringLiteral("emoji"), emojiForKey(k)}};
    }
    return list;
}

void Backend::toggleReaction(const QString &messageId, const QString &key)
{
    if (m_currentConversationId.isEmpty()) return;
    const MessageItem *m = m_messages.find(messageId);
    if (!m) m = m_thread.find(messageId);
    if (!m) return;

    // Update the screen right away; a failed request reloads the real state.
    QVariantList reactions = m->reactions;
    bool add = true;
    for (int i = 0; i < reactions.size(); ++i) {
        QVariantMap r = reactions[i].toMap();
        if (r.value(QStringLiteral("key")).toString() != key) continue;
        if (r.value(QStringLiteral("mine")).toBool()) {
            add = false;
            const int count = r.value(QStringLiteral("count")).toInt() - 1;
            if (count <= 0) { reactions.removeAt(i); }
            else {
                r[QStringLiteral("count")] = count;
                r[QStringLiteral("mine")] = false;
                QStringList names = r.value(QStringLiteral("names")).toString().split(QStringLiteral(", "));
                names.removeAll(QStringLiteral("Tu"));
                r[QStringLiteral("names")] = names.join(QStringLiteral(", "));
                reactions[i] = r;
            }
        } else {
            r[QStringLiteral("count")] = r.value(QStringLiteral("count")).toInt() + 1;
            r[QStringLiteral("mine")] = true;
            r[QStringLiteral("names")] = QStringLiteral("Tu, ") + r.value(QStringLiteral("names")).toString();
            reactions[i] = r;
        }
        break;
    }
    if (add && std::none_of(reactions.cbegin(), reactions.cend(),
                            [&](const QVariant &r) { return r.toMap().value(QStringLiteral("key")).toString() == key; }))
        reactions << QVariantMap{{QStringLiteral("key"), key}, {QStringLiteral("emoji"), emojiForKey(key)},
                                 {QStringLiteral("count"), 1}, {QStringLiteral("mine"), true}, {QStringLiteral("names"), QStringLiteral("Tu")}};

    m_messages.setReactions(messageId, reactions);
    m_thread.setReactions(messageId, reactions);
    m_client.setReaction(m_currentConversationId, messageId, key, add);
}

bool Backend::mentionsSelf(const QJsonObject &properties) const
{
    // "mentions": JSON array (often as a string) of {"mri": ..., "mentionType": ...}
    const QJsonValue v = properties.value(QLatin1String("mentions"));
    const QJsonArray mentions = v.isString() ? QJsonDocument::fromJson(v.toString().toUtf8()).array() : v.toArray();
    const QString self = selfMri();
    return std::any_of(mentions.cbegin(), mentions.cend(),
                       [&](const QJsonValue &m) { return m.toObject().value(QLatin1String("mri")).toString() == self; });
}

void Backend::pollPresence()
{
    if (m_state != State::Connected) return;
    QStringList mris;
    for (auto it = m_people.cbegin(); it != m_people.cend(); ++it)
        if (it.key().startsWith(QLatin1String("8:"))) mris << it.key(); // not phone numbers ("4:")
    mris << selfMri(); // own status, for the status menu
    // purple-teams batches 650 per request.
    m_oauth.accessToken(QStringLiteral("https://presence.teams.microsoft.com"), [this, mris](const QString &token) {
        for (int i = 0; i < mris.size(); i += 650) m_client.fetchPresence(mris.mid(i, 650), token);
    });
}

void Backend::editMessage(const QString &messageId, const QString &text)
{
    if (m_currentConversationId.isEmpty() || text.trimmed().isEmpty()) return;
    m_client.editMessage(m_currentConversationId, messageId, textToHtml(text));
}

void Backend::deleteMessage(const QString &messageId)
{
    if (m_currentConversationId.isEmpty()) return;
    m_client.deleteMessage(m_currentConversationId, messageId);
}

void Backend::userTyping()
{
    if (m_currentConversationId.isEmpty()) return;
    constexpr qint64 kTypingEveryMs = 5000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastTypingSent.value(m_currentConversationId) < kTypingEveryMs) return;
    m_lastTypingSent.insert(m_currentConversationId, now);
    m_client.sendTyping(m_currentConversationId);
}

void Backend::searchPeople(const QString &text)
{
    if (text.trimmed().isEmpty()) {
        m_peopleResults.clear();
        emit peopleResultsChanged();
        return;
    }
    const QString query = text.trimmed();
    m_oauth.accessToken(QStringLiteral("https://api.spaces.skype.com"),
                        [this, query](const QString &token) { m_client.searchUsers(query, token); });
}

void Backend::startChatWith(const QString &mri, const QString &name)
{
    // An existing one-to-one chat with this person: just open it.
    for (auto it = m_chatMembers.cbegin(); it != m_chatMembers.cend(); ++it) {
        const ConversationItem *conv = m_conversations.find(it.key());
        if (conv && conv->type == QLatin1String("oneToOne") && it.value() == QStringList{mri}) {
            selectConversation(it.key());
            emit openConversationRequested(it.key());
            return;
        }
    }
    m_pendingChatTitle = name;
    m_client.createChat({mri}, selfMri(), true);
}

void Backend::handlePresence(const QJsonArray &list)
{
    // purple-teams: teams_got_contact_statuses(): [{mri, presence: {availability, activity, ...}}]
    bool changed = false;
    bool selfSeen = false;
    for (const auto &v : list) {
        const QJsonObject o = v.toObject();
        const QString mri = o.value(QLatin1String("mri")).toString();
        const QString availability = o.value(QLatin1String("presence")).toObject().value(QLatin1String("availability")).toString();
        if (mri == selfMri()) {
            // Own state: logged whole (no secrets in it) while the start-up value is being understood.
            selfSeen = true;
            qInfo() << "own presence:" << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)).left(400);
        }
        if (mri.isEmpty() || availability.isEmpty() || m_presence.value(mri) == availability) continue;
        m_presence.insert(mri, availability);
        changed = true;
    }
    if (!selfSeen && !list.isEmpty()) qInfo() << "own presence: not in the answer (" << list.size() << "entries)";
    if (changed) { ++m_presenceRevision; emit presenceChanged(); }
}

void Backend::handleEvent(const QJsonObject &event)
{
    // purple-teams: teams_process_event_message()
    const QString type = event.value(QLatin1String("resourceType")).toString();
    const QJsonObject resource = event.value(QLatin1String("resource")).toObject();
    // ".../conversations/<id>" -> "<id>"
    const QString conversationId = resource.value(QLatin1String("conversationLink")).toString().section(QLatin1Char('/'), -1)
                                       .section(QLatin1Char(';'), 0, 0);

    if (type == QLatin1String("NewMessage") && conversationId == QLatin1String(kActivityFeed)) {
        m_client.fetchMessages(QString::fromLatin1(kActivityFeed));
        return;
    }
    if (type == QLatin1String("NewMessage")) {
        const QString messageType = resource.value(QLatin1String("messagetype")).toString();
        if (messageType == QLatin1String("Control/Typing") || messageType == QLatin1String("Control/ClearTyping")) {
            if (conversationId != m_currentConversationId || isSelf(resource.value(QLatin1String("from")).toString())) return;
            if (messageType == QLatin1String("Control/ClearTyping")) {
                m_typingExpire.stop();
                m_typingText.clear();
            } else {
                const QString who = resource.value(QLatin1String("imdisplayname")).toString();
                m_typingText = tr("%1 is typing...").arg(who.isEmpty() ? tr("Someone") : who);
                m_typingExpire.start();
            }
            emit typingChanged();
            return;
        }
        // A real message: whoever was typing has sent it.
        if (conversationId == m_currentConversationId && !m_typingText.isEmpty()) {
            m_typingExpire.stop();
            m_typingText.clear();
            emit typingChanged();
        }
        m_eventSync.start();
    } else if (type == QLatin1String("MessageUpdate") || type == QLatin1String("ConversationUpdate")
               || type == QLatin1String("ThreadUpdate")) {
        // Edits, deletions, reactions, renames: refresh what is shown.
        if (conversationId == m_currentConversationId) {
            m_client.fetchMessages(conversationId);
            if (!currentIsChannel()) m_client.fetchReadHorizons(conversationId); // someone read the chat
        }
        m_eventSync.start();
    }
}

namespace {
constexpr qint64 kMaxImageUploadBytes = 20 * 1024 * 1024;
}

void Backend::sendImageData(const QByteArray &data, const QString &fileName, bool inThread)
{
    if (m_currentConversationId.isEmpty() || data.isEmpty()) return;
    if (data.size() > kMaxImageUploadBytes) {
        setError(tr("Picture too large (20 MB at most)."));
        return;
    }
    m_client.sendImage(m_currentConversationId, data, fileName, m_selfDisplayName, inThread ? m_currentThreadId : QString());
}

void Backend::sendImageFile(const QUrl &file, bool inThread)
{
    QFile f(file.toLocalFile());
    if (!f.open(QIODevice::ReadOnly)) {
        setError(tr("Cannot read %1").arg(file.toLocalFile()));
        return;
    }
    if (f.size() > kMaxImageUploadBytes) {
        setError(tr("Picture too large (20 MB at most)."));
        return;
    }
    sendImageData(f.readAll(), QFileInfo(f).fileName(), inThread);
}

bool Backend::clipboardHasImage() const
{
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    return mime && mime->hasImage();
}

void Backend::sendClipboardImage(bool inThread)
{
    const QImage image = QGuiApplication::clipboard()->image();
    if (image.isNull()) return;
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    sendImageData(png, QStringLiteral("immagine.png"), inThread);
}

void Backend::sendFile(const QUrl &file, bool inThread)
{
    if (m_currentConversationId.isEmpty() || m_files.busy()) return;
    m_uploadConversation = m_currentConversationId;
    m_uploadReplyTo = inThread ? m_currentThreadId : QString();
    const ChannelItem *channel = m_channels.find(m_currentConversationId);
    const QString groupId = channel ? channel->groupId : QString();
    const QString channelId = channel ? channel->id : QString();
    if (channel && groupId.isEmpty()) { setError(tr("Unknown team for this channel: cannot upload the file.")); return; }
    const QString path = file.toLocalFile();
    m_uploadProgress = 0;
    emit uploadingChanged();
    m_oauth.accessToken(QStringLiteral("https://graph.microsoft.com"), [this, path, groupId, channelId](const QString &token) {
        m_files.upload(path, token, groupId, channelId);
        emit uploadingChanged();
    });
}

void Backend::startNativeVideoCall()
{
    m_cameraWhenConnected = true;
    startNativeCall();
}

void Backend::joinMeeting(const QString &joinUrl)
{
    // https://teams.microsoft.com/l/meetup-join/19%3ameeting_...%40thread.v2/0?context={"Tid":...,"Oid":...}
    const QUrl url(joinUrl);
    const QStringList parts = url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const int at = parts.indexOf(QStringLiteral("meetup-join"));
    const QString thread = at >= 0 ? parts.value(at + 1) : QString();
    const QJsonObject context = QJsonDocument::fromJson(QUrlQuery(url).queryItemValue(QStringLiteral("context"), QUrl::FullyDecoded).toUtf8()).object();
    const QString tenant = context.value(QLatin1String("Tid")).toString(), organizer = context.value(QLatin1String("Oid")).toString();
    if (!thread.startsWith(QLatin1String("19:")) || tenant.isEmpty() || organizer.isEmpty()) {
        setError(tr("Meeting link not recognized."));
        return;
    }
    if (m_trouter.surl().isEmpty()) { setError(tr("Push channel not connected: try again in a few seconds.")); return; }
    if (m_nativeCall.isActive()) { setError(tr("A call is already in progress.")); return; }
    QString subject;
    for (const CalendarEvent &ev : m_calendar.items()) if (ev.joinUrl == joinUrl) { subject = ev.subject; break; }
    m_nativeCall.joinMeeting({m_client.tokens().skypeToken, selfMri(), m_selfDisplayName, m_trouter.endpointId(), m_trouter.surl()},
                             thread, tenant, organizer, subject);
}

void Backend::presenceToken(std::function<void(const QString &)> cb)
{
    m_oauth.accessToken(QStringLiteral("https://presence.teams.microsoft.com"), std::move(cb));
}

void Backend::updateActivity()
{
    // Active while the window is in use; inactive after kIdleMinutes away (Teams shows "Away").
    const bool active = QGuiApplication::applicationState() == Qt::ApplicationActive || m_idleTimer.isActive();
    if (active == m_reportedActive || m_state != State::Connected) return;
    m_reportedActive = active;
    m_trouter.sendActivity(active);
    presenceToken([this, active](const QString &token) { m_client.reportActivity(m_trouter.endpointId(), active, token); });
}

void Backend::setMyAvailability(const QString &availability)
{
    static const QStringList allowed{QStringLiteral("Available"), QStringLiteral("Busy"), QStringLiteral("DoNotDisturb"),
                                     QStringLiteral("BeRightBack"), QStringLiteral("Away"), QStringLiteral("Offline")};
    if (!allowed.contains(availability)) return;
    presenceToken([this, availability](const QString &token) {
        m_client.forceAvailability(availability, token);
        QTimer::singleShot(std::chrono::seconds(2), this, &Backend::pollPresence); // show the new state
    });
}

void Backend::setStatusMessage(const QString &message)
{
    presenceToken([this, message](const QString &token) { m_client.publishNote(message.trimmed(), token); });
}

namespace {
// "report.xlsx" -> "report (1).xlsx" while the name is taken.
QString freeDownloadPath(const QString &fileName)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QDir().mkpath(dir);
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
}

void Backend::notifyDownloaded(const QString &path)
{
    auto *n = new KNotification(QStringLiteral("downloadDone"), KNotification::CloseOnTimeout);
    n->setComponentName(QStringLiteral("kteams"));
    n->setTitle(tr("Download finished"));
    n->setText(QFileInfo(path).fileName().toHtmlEscaped());
    n->setIconName(QStringLiteral("document-save"));
    KNotificationAction *open = n->addAction(tr("Open"));
    connect(open, &KNotificationAction::activated, this, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
    KNotificationAction *folder = n->addAction(tr("Show in folder"));
    connect(folder, &KNotificationAction::activated, this,
            [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())); });
    n->sendEvent();
}

void Backend::downloadFile(const QVariantMap &file)
{
    const QString link = file.value(QStringLiteral("shareUrl")).toString();
    if (!link.startsWith(QLatin1String("https://"))) { setError(tr("This file has no downloadable link.")); return; }
    const QString name = file.value(QStringLiteral("name")).toString();
    // Graph "access shared items": shares/u!<base64url of the link, no padding>/driveItem/content
    const QString shareId = QStringLiteral("u!") + QString::fromLatin1(link.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    m_downloading = true;
    emit downloadingChanged();
    m_oauth.accessToken(QStringLiteral("https://graph.microsoft.com"), [this, shareId, name](const QString &token) {
        QNetworkRequest req(QUrl(QStringLiteral("https://graph.microsoft.com/v1.0/shares/%1/driveItem/content").arg(shareId)));
        req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        // Graph answers with a redirect to a pre-authorized download URL: follow it ourselves,
        // without sending the token there.
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        QNetworkReply *reply = m_downloads.get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, name] {
            reply->deleteLater();
            const QUrl location = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
            const auto save = [this, name](QNetworkReply *r) {
                m_downloading = false;
                emit downloadingChanged();
                if (r->error() != QNetworkReply::NoError) {
                    setError(tr("Download failed: %1").arg(r->errorString()));
                    return;
                }
                const QString path = freeDownloadPath(name);
                QFile out(path);
                if (!out.open(QIODevice::WriteOnly)) { setError(tr("Cannot write %1").arg(path)); return; }
                out.write(r->readAll());
                out.close();
                notifyDownloaded(path);
            };
            if (location.isValid() && location.scheme() == QLatin1String("https")) {
                QNetworkReply *content = m_downloads.get(QNetworkRequest(location));
                connect(content, &QNetworkReply::finished, this, [content, save] { content->deleteLater(); save(content); });
            } else {
                save(reply);
            }
        });
    });
}

void Backend::saveImage(const QString &remoteUrl)
{
    const QString local = m_media.urlFor(remoteUrl);
    if (local.isEmpty()) { setError(tr("Picture not downloaded yet.")); return; }
    const QString source = QUrl(local).toLocalFile();
    // Cached files have no extension: take it from the image itself.
    const QString format = QString::fromLatin1(QImageReader::imageFormat(source));
    const QString path = freeDownloadPath(QStringLiteral("immagine.") + (format.isEmpty() ? QStringLiteral("png") : format));
    if (!QFile::copy(source, path)) { setError(tr("Cannot save the picture.")); return; }
    notifyDownloaded(path);
}

void Backend::startNativeTestCall()
{
    m_nativeCall.startTestCall({m_client.tokens().skypeToken, selfMri(), m_selfDisplayName, m_trouter.endpointId(), m_trouter.surl()});
}

bool Backend::canNativeCall() const
{
    // One-to-one and group chats (not channels): everyone else in the chat gets called.
    const ConversationItem *conv = m_conversations.find(m_currentConversationId);
    return conv && !m_channelIds.contains(conv->id) && !m_chatMembers.value(conv->id).isEmpty();
}

void Backend::setVideoSinks(QObject *remote, QObject *local, QObject *share)
{
    m_nativeCall.setVideoSinks(qobject_cast<QVideoSink *>(remote), qobject_cast<QVideoSink *>(local), qobject_cast<QVideoSink *>(share));
}

void Backend::setCameraEnabled(bool on)
{
    if (on && !m_nativeCall.mediaConnected()) { setError(tr("The camera turns on once the call is connected.")); return; }
    m_nativeCall.setCameraEnabled(on);
}

void Backend::startScreenShare()
{
    if (!m_nativeCall.isActive() || sharingOn()) return;
    if (!m_nativeCall.mediaConnected()) { setError(tr("Sharing starts once the call is connected.")); return; }
    m_screenCast.start();
    emit nativeCallChanged();
}

void Backend::stopScreenShare()
{
    m_screenCast.stop();
    m_nativeCall.stopScreenShare();
    emit nativeCallChanged();
}

void Backend::startNativeCall()
{
    const ConversationItem *conv = m_conversations.find(m_currentConversationId);
    const QStringList members = conv ? m_chatMembers.value(conv->id) : QStringList();
    if (members.isEmpty()) { setError(tr("Open a chat with people to call them.")); return; }
    m_nativeCall.startCall({m_client.tokens().skypeToken, selfMri(), m_selfDisplayName, m_trouter.endpointId(), m_trouter.surl()},
                           members, conv->id, conv->title);
}

void Backend::setNotificationPicture(KNotification *n, const QString &mri)
{
    const QString local = m_avatars.urlFor(mri);
    if (local.isEmpty()) return;
    const QPixmap picture(QUrl(local).toLocalFile());
    if (!picture.isNull()) n->setPixmap(picture.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

QVariantList Backend::currentMembers() const
{
    QVariantList list;
    for (const QString &mri : m_chatMembers.value(m_currentConversationId)) {
        const QString name = m_people.value(mri).displayName;
        if (!name.isEmpty()) list << QVariantMap{{QStringLiteral("mri"), mri}, {QStringLiteral("name"), Demo::name(name)}};
    }
    return list;
}

void Backend::sendMessageWithMentions(const QString &text, const QVariantList &mentions, bool inThread)
{
    if (m_currentConversationId.isEmpty() || text.trimmed().isEmpty()) return;
    if (inThread && m_currentThreadId.isEmpty()) return;
    // Teams: "<at id=\"N\">Name</at>" in the HTML, properties.mentions (JSON string) with the MRIs.
    QString html = text.trimmed().toHtmlEscaped();
    QJsonArray mentionList;
    int id = 0;
    for (const QVariant &v : mentions) {
        const QVariantMap m = v.toMap();
        const QString name = m.value(QStringLiteral("name")).toString().toHtmlEscaped();
        const QString mri = m.value(QStringLiteral("mri")).toString();
        if (name.isEmpty() || mri.isEmpty() || !html.contains(QLatin1Char('@') + name)) continue;
        html.replace(QLatin1Char('@') + name, QStringLiteral("<at id=\"%1\">%2</at>").arg(id).arg(name));
        mentionList.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("mri"), mri},
                                       {QStringLiteral("mentionType"), QStringLiteral("person")}, {QStringLiteral("displayName"), name}});
        ++id;
    }
    html.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    QJsonObject properties;
    if (!mentionList.isEmpty())
        properties.insert(QStringLiteral("mentions"), QString::fromUtf8(QJsonDocument(mentionList).toJson(QJsonDocument::Compact)));
    m_client.sendMessage(m_currentConversationId, quoteHtml() + QStringLiteral("<p>%1</p>").arg(html), m_selfDisplayName,
                         inThread ? m_currentThreadId : QString(), properties);
}

void Backend::handleActivity(const QJsonArray &arr)
{
    QVector<ActivityItem> items;
    for (const auto &v : arr) {
        const QJsonObject o = v.toObject();
        const QJsonObject props = o.value(QLatin1String("properties")).toObject();
        const QJsonValue av = props.value(QLatin1String("activity"));
        const QJsonObject a = av.isString() ? QJsonDocument::fromJson(av.toString().toUtf8()).object() : av.toObject();
        if (a.isEmpty()) continue;
        ActivityItem it;
        it.id = o.value(QLatin1String("id")).toString();
        it.type = a.value(QLatin1String("activityType")).toString();
        it.subtype = a.value(QLatin1String("activitySubtype")).toString();
        it.who = a.value(QLatin1String("sourceUserImDisplayName")).toString();
        it.whoMri = a.value(QLatin1String("sourceUserId")).toString();
        it.preview = htmlToText(a.value(QLatin1String("messagePreview")).toString()).simplified();
        it.threadId = a.value(QLatin1String("sourceThreadId")).toString();
        it.messageId = a.value(QLatin1String("sourceMessageId")).toVariant().toString();
        it.timestamp = parseTime(a.value(QLatin1String("activityTimestamp")).toString(o.value(QLatin1String("composetime")).toString()));
        it.read = props.value(QLatin1String("isread")).toVariant().toBool();
        // What happened, in words.
        const QString t = it.type.toLower();
        if (t.contains(QLatin1String("mention"))) it.text = tr("mentioned you");
        else if (t.contains(QLatin1String("reaction"))) it.text = tr("reacted %1 to your message").arg(emojiForKey(it.subtype));
        else if (t.contains(QLatin1String("reply"))) it.text = tr("replied");
        else if (t.contains(QLatin1String("missedcall"))) it.text = tr("missed call");
        else if (t.contains(QLatin1String("voicemail"))) it.text = tr("voicemail");
        else it.text = it.type;
        // Where: a known chat or channel name, if we have it.
        if (const ChannelItem *ch = m_channels.find(it.threadId)) it.where = ch->teamName + QStringLiteral(" › ") + ch->name;
        else if (const ConversationItem *c = m_conversations.find(it.threadId)) it.where = c->type == QLatin1String("oneToOne") ? QString() : c->title;
        items.push_back(it);
    }
    // New unread entries since the last look (not on the first load): a notification each,
    // unless do-not-disturb.
    for (const ActivityItem &it : std::as_const(items)) {
        if (m_knownActivity.contains(it.id)) continue;
        m_knownActivity.insert(it.id);
        if (!m_activityLoaded || it.read || doNotDisturb()) continue;
        auto *n = new KNotification(QStringLiteral("activity"), KNotification::CloseOnTimeout);
        n->setComponentName(QStringLiteral("kteams"));
        n->setTitle(it.who + QLatin1Char(' ') + it.text + (it.where.isEmpty() ? QString() : QStringLiteral(" · ") + it.where));
        n->setText(it.preview.toHtmlEscaped());
        n->setIconName(QStringLiteral("notifications"));
        setNotificationPicture(n, it.whoMri);
        const QString thread = it.threadId;
        KNotificationAction *open = n->addDefaultAction(tr("Open"));
        connect(open, &KNotificationAction::activated, this, [this, thread] { if (!thread.isEmpty()) emit openConversationRequested(thread); });
        n->sendEvent();
    }
    m_activityLoaded = true;
    m_allActivity = items;
    setActivityFilter(m_activityFilter); // applies the filter and emits activityChanged
}

void Backend::setActivityFilter(const QString &filter)
{
    m_activityFilter = filter;
    QVector<ActivityItem> shown;
    for (const ActivityItem &it : std::as_const(m_allActivity))
        if (filter.isEmpty() || it.type.contains(filter, Qt::CaseInsensitive)) shown.push_back(it);
    m_activity.setItems(shown);
    emit activityChanged();
}

void Backend::markActivityRead()
{
    const auto &items = m_activity.items();
    if (items.isEmpty()) return;
    // The feed is a conversation like the others: its consumption horizon marks everything read.
    m_client.markRead(QString::fromLatin1(kActivityFeed), items.first().id, items.first().id);
    for (ActivityItem &it : m_allActivity) it.read = true;
    m_activity.markAllRead();
    emit activityChanged();
}

void Backend::pinChat(const QString &id, bool pinned)
{
    const ConversationItem *conv = m_conversations.find(id);
    if (!conv) return;
    m_client.setConversationProperty(id, QStringLiteral("favorite"), pinned ? QStringLiteral("true") : QStringLiteral("false"));
    m_conversations.setFlags(id, pinned, conv->muted);
}

void Backend::muteChat(const QString &id, bool muted)
{
    const ConversationItem *conv = m_conversations.find(id);
    if (!conv) return;
    m_client.setConversationProperty(id, QStringLiteral("alerts"), muted ? QStringLiteral("false") : QStringLiteral("true"));
    m_conversations.setFlags(id, conv->pinned, muted);
}

void Backend::hideChat(const QString &id)
{
    m_client.setConversationProperty(id, QStringLiteral("hidden"), QStringLiteral("true"));
    m_conversations.remove(id);
    if (m_currentConversationId == id) selectConversation(QString());
}

void Backend::renameChat(const QString &id, const QString &title)
{
    if (title.trimmed().isEmpty()) return;
    m_client.setThreadTopic(id, title.trimmed());
    m_conversations.setTitle(id, title.trimmed());
}

void Backend::leaveChat(const QString &id)
{
    m_client.removeMember(id, selfMri());
    m_conversations.remove(id);
    if (m_currentConversationId == id) selectConversation(QString());
}

void Backend::addMembers(const QString &id, const QStringList &mris)
{
    for (const QString &mri : mris) if (!mri.isEmpty()) m_client.addMember(id, mri);
    QTimer::singleShot(std::chrono::seconds(2), this, &Backend::loadPeople); // members and names again
}

void Backend::setQuote(const QString &messageId)
{
    for (const MessageItem &m : m_messages.items()) {
        if (m.id != messageId) continue;
        m_quoteId = m.id;
        m_quoteMri = m.fromId;
        m_quoteWho = m.from;
        m_quoteText = m.content.simplified().left(200);
        emit quoteChanged();
        return;
    }
}

void Backend::clearQuote()
{
    if (m_quoteId.isEmpty()) return;
    m_quoteId.clear(); m_quoteMri.clear(); m_quoteWho.clear(); m_quoteText.clear();
    emit quoteChanged();
}

QString Backend::quoteHtml()
{
    // Teams' reply quote: a blockquote of schema.skype.com/Reply naming the message, the
    // author and a preview; Teams clients show it as a quoted bubble.
    if (m_quoteId.isEmpty()) return {};
    const QString html = QStringLiteral("<blockquote itemscope=\"\" itemtype=\"http://schema.skype.com/Reply\" itemid=\"%1\">"
                                        "<strong itemprop=\"mri\" itemid=\"%2\">%3</strong><span itemprop=\"time\" itemid=\"%1\"></span>"
                                        "<p itemprop=\"preview\">%4</p></blockquote>")
                             .arg(m_quoteId, m_quoteMri.toHtmlEscaped(), m_quoteWho.toHtmlEscaped(), m_quoteText.toHtmlEscaped());
    clearQuote();
    return html;
}

void Backend::createMeeting(const QString &subject, const QDateTime &start, const QDateTime &end, const QStringList &attendees)
{
    if (subject.trimmed().isEmpty() || !start.isValid() || !end.isValid() || end <= start) { setError(tr("Meeting: invalid title or times.")); return; }
    m_oauth.accessToken(QStringLiteral("https://graph.microsoft.com"), [this, subject, start, end, attendees](const QString &token) {
        const QString tz = QString::fromLatin1(QTimeZone::systemTimeZoneId());
        QJsonArray people;
        for (const QString &a : attendees) {
            const QString address = a.trimmed();
            if (address.contains(QLatin1Char('@')))
                people.append(QJsonObject{{QStringLiteral("emailAddress"), QJsonObject{{QStringLiteral("address"), address}}}, {QStringLiteral("type"), QStringLiteral("required")}});
        }
        const QJsonObject body{
            {QStringLiteral("subject"), subject.trimmed()},
            {QStringLiteral("start"), QJsonObject{{QStringLiteral("dateTime"), start.toString(Qt::ISODate)}, {QStringLiteral("timeZone"), tz}}},
            {QStringLiteral("end"), QJsonObject{{QStringLiteral("dateTime"), end.toString(Qt::ISODate)}, {QStringLiteral("timeZone"), tz}}},
            {QStringLiteral("isOnlineMeeting"), true},
            {QStringLiteral("onlineMeetingProvider"), QStringLiteral("teamsForBusiness")},
            {QStringLiteral("attendees"), people}};
        QNetworkRequest req(QUrl(QStringLiteral("https://graph.microsoft.com/v1.0/me/events")));
        req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        QNetworkReply *reply = m_downloads.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError) {
                qWarning() << "createMeeting HTTP" << http << reply->readAll().left(300);
                setError(tr("Meeting not created (HTTP %1)").arg(http));
                return;
            }
            loadCalendarWeek(m_calendarWeekOffset);
        });
    });
}

void Backend::setUnreadOnly(bool on)
{
    if (m_unreadOnly == on) return;
    m_unreadOnly = on;
    m_filteredConversations.setUnreadOnly(on);
    emit unreadOnlyChanged();
}

void Backend::scheduleReminders(const QVector<CalendarEvent> &events)
{
    // One notification a few minutes before each upcoming event of this week, once.
    constexpr int kMinutesBefore = 5;
    const QDateTime now = QDateTime::currentDateTime();
    for (const CalendarEvent &ev : events) {
        if (ev.id.isEmpty() || m_reminded.contains(ev.id) || ev.myResponse == QLatin1String("Declined")) continue;
        const qint64 msecs = now.msecsTo(ev.start.addSecs(-kMinutesBefore * 60));
        if (msecs < 0 || msecs > 7LL * 24 * 3600 * 1000) continue;
        m_reminded.insert(ev.id);
        const QString subject = ev.subject, joinUrl = ev.joinUrl, chatId = ev.chatId;
        const QString when = QLocale().toString(ev.start.toLocalTime().time(), QLocale::ShortFormat);
        QTimer::singleShot(std::chrono::milliseconds(msecs), this, [this, subject, joinUrl, chatId, when] {
            auto *n = new KNotification(QStringLiteral("reminder"), KNotification::Persistent);
            n->setComponentName(QStringLiteral("kteams"));
            n->setTitle(tr("In %1 minutes: %2").arg(kMinutesBefore).arg(subject.isEmpty() ? tr("meeting") : subject));
            n->setText(tr("Starts at %1").arg(when));
            n->setIconName(QStringLiteral("view-calendar"));
            if (!joinUrl.isEmpty()) {
                KNotificationAction *join = n->addDefaultAction(QStringLiteral("Partecipa"));
                connect(join, &KNotificationAction::activated, this, [this, joinUrl] { joinMeeting(joinUrl); });
            }
            if (!chatId.isEmpty()) {
                KNotificationAction *chat = n->addAction(tr("Open chat"));
                connect(chat, &KNotificationAction::activated, this, [this, chatId, subject] {
                    selectConversation(chatId, subject);
                    emit openConversationRequested(chatId);
                });
            }
            n->sendEvent();
        });
    }
}

void Backend::respondToEvent(const QString &iCalUid, const QString &response)
{
    // Microsoft Graph: find the event by iCalUId, then POST .../accept | decline | tentativelyAccept.
    static const QHash<QString, QString> actions{{QStringLiteral("accept"), QStringLiteral("Accepted")},
                                                 {QStringLiteral("decline"), QStringLiteral("Declined")},
                                                 {QStringLiteral("tentativelyAccept"), QStringLiteral("TentativelyAccepted")}};
    if (iCalUid.isEmpty() || !actions.contains(response)) return;
    m_oauth.accessToken(QStringLiteral("https://graph.microsoft.com"), [this, iCalUid, response](const QString &token) {
        QUrl url(QStringLiteral("https://graph.microsoft.com/v1.0/me/events"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("$filter"), QStringLiteral("iCalUId eq '%1'").arg(QString(iCalUid).replace(QLatin1Char('\''), QStringLiteral("''"))));
        q.addQueryItem(QStringLiteral("$select"), QStringLiteral("id"));
        url.setQuery(q);
        QNetworkRequest req(url);
        req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        QNetworkReply *find = m_downloads.get(req);
        connect(find, &QNetworkReply::finished, this, [this, find, token, iCalUid, response] {
            find->deleteLater();
            const QJsonArray values = QJsonDocument::fromJson(find->readAll()).object().value(QLatin1String("value")).toArray();
            const QString id = values.isEmpty() ? QString() : values.first().toObject().value(QLatin1String("id")).toString();
            if (find->error() != QNetworkReply::NoError || id.isEmpty()) { setError(tr("Event not found in the calendar (Graph).")); return; }
            QNetworkRequest post(QUrl(QStringLiteral("https://graph.microsoft.com/v1.0/me/events/%1/%2").arg(id, response)));
            post.setRawHeader("Authorization", "Bearer " + token.toUtf8());
            post.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            QNetworkReply *reply = m_downloads.post(post, QByteArray("{\"sendResponse\":true}"));
            connect(reply, &QNetworkReply::finished, this, [this, reply, iCalUid, response] {
                reply->deleteLater();
                if (reply->error() != QNetworkReply::NoError) {
                    setError(tr("Replying to the invitation failed (HTTP %1)").arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
                    return;
                }
                m_calendar.setMyResponse(iCalUid, actions.value(response));
            });
        });
    });
}

// ---- local cache -------------------------------------------------------------------------

QString Backend::cacheDir() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/state");
    QDir().mkpath(dir + QStringLiteral("/messages"));
    return dir;
}

namespace {
QJsonObject conversationToJson(const ConversationItem &c)
{
    return {{QStringLiteral("id"), c.id}, {QStringLiteral("title"), c.title}, {QStringLiteral("type"), c.type},
            {QStringLiteral("preview"), c.preview}, {QStringLiteral("lastActivity"), c.lastActivity},
            {QStringLiteral("lastMessageId"), c.lastMessageId}, {QStringLiteral("unread"), c.unread},
            {QStringLiteral("hasTopic"), c.hasTopic}, {QStringLiteral("peerMri"), c.peerMri}, {QStringLiteral("lastFromMri"), c.lastFromMri}};
}
ConversationItem conversationFromJson(const QJsonObject &o)
{
    ConversationItem c;
    c.id = o.value(QLatin1String("id")).toString(); c.title = o.value(QLatin1String("title")).toString();
    c.type = o.value(QLatin1String("type")).toString(); c.preview = o.value(QLatin1String("preview")).toString();
    c.lastActivity = o.value(QLatin1String("lastActivity")).toVariant().toLongLong();
    c.lastMessageId = o.value(QLatin1String("lastMessageId")).toString(); c.unread = o.value(QLatin1String("unread")).toBool();
    c.hasTopic = o.value(QLatin1String("hasTopic")).toBool(); c.peerMri = o.value(QLatin1String("peerMri")).toString();
    c.lastFromMri = o.value(QLatin1String("lastFromMri")).toString();
    return c;
}
QJsonObject messageToJson(const MessageItem &m)
{
    return {{QStringLiteral("id"), m.id}, {QStringLiteral("clientMessageId"), m.clientMessageId}, {QStringLiteral("from"), m.from},
            {QStringLiteral("fromId"), m.fromId}, {QStringLiteral("content"), m.content}, {QStringLiteral("richText"), m.richText},
            {QStringLiteral("images"), QJsonArray::fromStringList(m.images)}, {QStringLiteral("files"), QJsonArray::fromVariantList(m.files)},
            {QStringLiteral("reactions"), QJsonArray::fromVariantList(m.reactions)}, {QStringLiteral("time"), m.time},
            {QStringLiteral("timestamp"), m.timestamp}, {QStringLiteral("mine"), m.mine}, {QStringLiteral("edited"), m.edited},
            {QStringLiteral("deleted"), m.deleted}, {QStringLiteral("mentionsMe"), m.mentionsMe}};
}
MessageItem messageFromJson(const QJsonObject &o)
{
    MessageItem m;
    m.id = o.value(QLatin1String("id")).toString(); m.clientMessageId = o.value(QLatin1String("clientMessageId")).toString();
    m.from = o.value(QLatin1String("from")).toString(); m.fromId = o.value(QLatin1String("fromId")).toString();
    m.content = o.value(QLatin1String("content")).toString(); m.richText = o.value(QLatin1String("richText")).toString();
    for (const auto &i : o.value(QLatin1String("images")).toArray()) m.images << i.toString();
    m.files = o.value(QLatin1String("files")).toArray().toVariantList(); m.reactions = o.value(QLatin1String("reactions")).toArray().toVariantList();
    m.time = o.value(QLatin1String("time")).toString(); m.timestamp = o.value(QLatin1String("timestamp")).toVariant().toLongLong();
    m.mine = o.value(QLatin1String("mine")).toBool(); m.edited = o.value(QLatin1String("edited")).toBool();
    m.deleted = o.value(QLatin1String("deleted")).toBool(); m.mentionsMe = o.value(QLatin1String("mentionsMe")).toBool();
    return m;
}
QString cacheFileName(const QString &conversationId)
{
    return QString::fromLatin1(QCryptographicHash::hash(conversationId.toUtf8(), QCryptographicHash::Sha1).toHex()) + QStringLiteral(".json");
}
}

void Backend::saveStateCache()
{
    if (m_state != State::Connected) return;
    QJsonArray conversations;
    for (int i = 0; i < m_conversations.rowCount(); ++i)
        if (const ConversationItem *c = m_conversations.find(m_conversations.index(i).data(ConversationModel::IdRole).toString()))
            conversations.append(conversationToJson(*c));
    QJsonObject people;
    for (auto it = m_people.cbegin(); it != m_people.cend(); ++it)
        people.insert(it.key(), QJsonObject{{QStringLiteral("name"), it->displayName}, {QStringLiteral("given"), it->givenName}, {QStringLiteral("email"), it->email},
                                            {QStringLiteral("jobTitle"), it->jobTitle}, {QStringLiteral("department"), it->department}});
    QJsonObject members;
    for (auto it = m_chatMembers.cbegin(); it != m_chatMembers.cend(); ++it) members.insert(it.key(), QJsonArray::fromStringList(it.value()));
    QJsonArray channels;
    for (int i = 0; i < m_channels.rowCount(); ++i) {
        const QModelIndex idx = m_channels.index(i);
        channels.append(QJsonObject{{QStringLiteral("id"), idx.data(ChannelModel::IdRole).toString()}, {QStringLiteral("name"), idx.data(ChannelModel::NameRole).toString()},
                                    {QStringLiteral("team"), idx.data(ChannelModel::TeamRole).toString()}, {QStringLiteral("general"), idx.data(ChannelModel::GeneralRole).toBool()},
                                    {QStringLiteral("unread"), idx.data(ChannelModel::UnreadRole).toBool()}, {QStringLiteral("lastActivity"), idx.data(ChannelModel::LastActivityRole).toLongLong()},
                                    {QStringLiteral("groupId"), m_channels.find(idx.data(ChannelModel::IdRole).toString())->groupId}});
    }
    const QJsonObject state{{QStringLiteral("version"), 1}, {QStringLiteral("self"), m_selfSkypeName}, {QStringLiteral("selfName"), m_selfDisplayName},
                            {QStringLiteral("conversations"), conversations}, {QStringLiteral("people"), people},
                            {QStringLiteral("members"), members}, {QStringLiteral("channels"), channels}};
    QFile f(cacheDir() + QStringLiteral("/state.json"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(state).toJson(QJsonDocument::Compact));
}

void Backend::loadStateCache()
{
    QFile f(cacheDir() + QStringLiteral("/state.json"));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject state = QJsonDocument::fromJson(f.readAll()).object();
    if (state.value(QLatin1String("version")).toInt() != 1) return;
    m_selfSkypeName = state.value(QLatin1String("self")).toString();
    m_selfDisplayName = state.value(QLatin1String("selfName")).toString();
    const QJsonObject people = state.value(QLatin1String("people")).toObject();
    for (auto it = people.begin(); it != people.end(); ++it) {
        const QJsonObject p = it.value().toObject();
        m_people.insert(it.key(), {p.value(QLatin1String("name")).toString(), p.value(QLatin1String("given")).toString(), p.value(QLatin1String("email")).toString(),
                                   p.value(QLatin1String("jobTitle")).toString(), p.value(QLatin1String("department")).toString()});
    }
    const QJsonObject members = state.value(QLatin1String("members")).toObject();
    for (auto it = members.begin(); it != members.end(); ++it) {
        QStringList list;
        for (const auto &m : it.value().toArray()) list << m.toString();
        m_chatMembers.insert(it.key(), list);
    }
    QVector<ConversationItem> conversations;
    for (const auto &v : state.value(QLatin1String("conversations")).toArray()) conversations.push_back(conversationFromJson(v.toObject()));
    m_conversations.setItems(conversations);
    QVector<ChannelItem> channels;
    for (const auto &v : state.value(QLatin1String("channels")).toArray()) {
        const QJsonObject o = v.toObject();
        ChannelItem c;
        c.id = o.value(QLatin1String("id")).toString(); c.name = o.value(QLatin1String("name")).toString(); c.teamName = o.value(QLatin1String("team")).toString();
        c.general = o.value(QLatin1String("general")).toBool(); c.unread = o.value(QLatin1String("unread")).toBool();
        c.lastActivity = o.value(QLatin1String("lastActivity")).toVariant().toLongLong(); c.groupId = o.value(QLatin1String("groupId")).toString();
        channels.push_back(c);
        m_channelIds.insert(c.id);
    }
    m_channels.setItems(channels);
    if (!conversations.isEmpty()) {
        emit selfChanged();
        setStatus(tr("Saved data: %1 conversations (refreshing)").arg(conversations.size()));
    }
}

namespace {
// A cache file: {"conversation": id, "messages": [...]} (older files: the bare array).
QVector<MessageItem> readMessagesFile(const QString &path, QString *conversationId = nullptr)
{
    QVector<MessageItem> items;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return items;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    const QJsonArray arr = doc.isArray() ? doc.array() : doc.object().value(QLatin1String("messages")).toArray();
    if (conversationId) *conversationId = doc.isObject() ? doc.object().value(QLatin1String("conversation")).toString() : QString();
    for (const auto &v : arr) items.push_back(messageFromJson(v.toObject()));
    return items;
}
}

void Backend::saveMessagesCache(const QString &conversationId, const QVector<MessageItem> &items)
{
    constexpr int kKeep = 60;
    QJsonArray arr;
    for (int i = 0; i < qMin(kKeep, int(items.size())); ++i) arr.append(messageToJson(items[i]));
    QFile f(cacheDir() + QStringLiteral("/messages/") + cacheFileName(conversationId));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(QJsonObject{{QStringLiteral("conversation"), conversationId}, {QStringLiteral("messages"), arr}}).toJson(QJsonDocument::Compact));
}

bool Backend::loadMessagesCache(const QString &conversationId)
{
    const QVector<MessageItem> items = readMessagesFile(cacheDir() + QStringLiteral("/messages/") + cacheFileName(conversationId));
    if (items.isEmpty()) return false;
    m_messages.setItems(items);
    m_messagesFromCache = true;
    return true;
}

void Backend::loadSearchIndex()
{
    const QDir dir(cacheDir() + QStringLiteral("/messages"));
    for (const QString &name : dir.entryList(QDir::Files)) {
        QString id;
        const QVector<MessageItem> items = readMessagesFile(dir.filePath(name), &id);
        if (!id.isEmpty() && !items.isEmpty() && !m_searchIndex.contains(id)) m_searchIndex.insert(id, items);
    }
}

void Backend::searchMessages(const QString &text)
{
    m_searchQuery = text.trimmed();
    if (m_searchQuery.isEmpty()) { m_messageResults.clear(); emit messageResultsChanged(); return; }
    runSearch();
    // First search of the session: the newest page of every chat not seen yet, one at a time.
    if (!m_indexedAll && m_indexQueue.isEmpty() && m_state == State::Connected) {
        m_indexedAll = true;
        for (int i = 0; i < m_conversations.rowCount(); ++i) {
            const QString id = m_conversations.data(m_conversations.index(i), ConversationModel::IdRole).toString();
            if (!m_searchIndex.contains(id) && !m_channelIds.contains(id) && id != m_currentConversationId
                && !id.startsWith(QLatin1String("19:meeting_"))) m_indexQueue << id;
        }
        indexNext();
    }
}

void Backend::indexNext()
{
    m_indexTimeout.stop();
    emit messageResultsChanged(); // "indexing" and the count
    if (m_indexQueue.isEmpty()) { if (!m_searchQuery.isEmpty()) runSearch(); return; }
    m_client.fetchMessages(m_indexQueue.first());
    // A page that never comes (error, throttling) must not stop the rest.
    m_indexTimeout.start();
}

void Backend::runSearch()
{
    QVector<QPair<qint64, QVariantMap>> hits;
    const QString q = m_searchQuery;
    const auto scan = [&](const QString &conversationId, const QVector<MessageItem> &items) {
        const ConversationItem *conv = m_conversations.find(conversationId);
        for (const MessageItem &m : items) {
            if (m.deleted || !m.content.contains(q, Qt::CaseInsensitive)) continue;
            hits.append({m.timestamp, QVariantMap{{QStringLiteral("conversationId"), conversationId},
                                                  {QStringLiteral("title"), Demo::title(conv ? conv->title : QString())},
                                                  {QStringLiteral("who"), Demo::name(m.from)}, {QStringLiteral("fromMri"), m.fromId},
                                                  {QStringLiteral("text"), Demo::text(m.content.simplified())},
                                                  {QStringLiteral("timestamp"), m.timestamp}, {QStringLiteral("messageId"), m.id}}});
        }
    };
    for (auto it = m_searchIndex.cbegin(); it != m_searchIndex.cend(); ++it) scan(it.key(), it.value());
    if (!m_currentConversationId.isEmpty() && !m_searchIndex.contains(m_currentConversationId) && !currentIsChannel())
        scan(m_currentConversationId, m_messages.items());
    std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    m_messageResults.clear();
    for (int i = 0; i < qMin(100, int(hits.size())); ++i) m_messageResults << hits[i].second;
    emit messageResultsChanged();
}

QVariantMap Backend::personInfo(const QString &mri) const
{
    const Person p = m_people.value(mri);
    if (Demo::enabled())
        return {{QStringLiteral("mri"), mri}, {QStringLiteral("name"), Demo::name(p.displayName)}, {QStringLiteral("email"), QStringLiteral("name@example.com")},
                {QStringLiteral("jobTitle"), QStringLiteral("Product manager")}, {QStringLiteral("department"), QStringLiteral("Engineering")},
                {QStringLiteral("presence"), m_presence.value(mri)}};
    return {{QStringLiteral("mri"), mri}, {QStringLiteral("name"), p.displayName}, {QStringLiteral("email"), p.email},
            {QStringLiteral("jobTitle"), p.jobTitle}, {QStringLiteral("department"), p.department},
            {QStringLiteral("presence"), m_presence.value(mri)}};
}
