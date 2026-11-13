#include "teamsclient.h"

#include <QDateTime>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

namespace {
// Same values purple-teams sends (libteams.h, teams_connection.c).
constexpr auto kUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                            "Chrome/126.0.0.0 Safari/537.36 Edg/126.0.0.0 Teams/24165.1410.2974.6689/49";
constexpr auto kTargetTypes = "Passport|Skype|Lync|Thread|PSTN|Agent";
// purple-teams' fixed host for work accounts, used only until the real one is known.
constexpr auto kDefaultChatRegion = "apac";
}

TeamsClient::TeamsClient(QObject *parent) : QObject(parent) {}

void TeamsClient::setTokens(const TeamsTokens &tokens) { m_tokens = tokens; }

void TeamsClient::setHosts(const TeamsHosts &hosts) { m_hosts = hosts; }

QUrl TeamsClient::chatUrl(const QString &path) const
{
    QString base = m_tokens.chatServiceBase;
    if (base.isEmpty()) {
        const QString region = m_tokens.region.isEmpty() ? QString::fromLatin1(kDefaultChatRegion) : m_tokens.region;
        base = QStringLiteral("https://%1.ng.msg.teams.microsoft.com").arg(region);
    }
    return QUrl(base + path);
}

QNetworkRequest TeamsClient::makeChatRequest(const QUrl &url) const
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    if (!m_tokens.chatBearer.isEmpty())
        req.setRawHeader("Authorization", QByteArray("Bearer ") + m_tokens.chatBearer.toUtf8());
    else
        req.setRawHeader("X-Skypetoken", m_tokens.skypeToken.toUtf8());
    req.setRawHeader("X-Stratus-Caller", "skypeteams");
    req.setRawHeader("X-Stratus-Request", "abcd1234");
    req.setRawHeader("BehaviorOverride", "redirectAs404");
    const QString origin = url.host() == QLatin1String("teams.cloud.microsoft")
        ? QStringLiteral("https://teams.cloud.microsoft") : QStringLiteral("https://teams.microsoft.com");
    req.setRawHeader("Origin", origin.toLatin1());
    req.setRawHeader("Referer", (origin + QLatin1Char('/')).toLatin1());
    req.setRawHeader("Accept", "application/json; ver=1.0;");
    return req;
}

void TeamsClient::finish(const QString &context, QNetworkReply *reply, const Callback &onSuccess)
{
    reply->deleteLater();
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        QString detail = reply->errorString();
        if (!data.isEmpty()) detail += QStringLiteral(" — ") + QString::fromUtf8(data.left(200));
        emit errorOccurred(context, http, detail);
        return;
    }
    onSuccess(QJsonDocument::fromJson(data), reply);
}

void TeamsClient::chatGet(const QString &context, const QUrl &url, Callback onSuccess)
{
    if (!canFetch()) { emit errorOccurred(context, 0, QStringLiteral("missing chat token")); return; }
    QNetworkReply *reply = m_nam.get(makeChatRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, context, reply, onSuccess] { finish(context, reply, onSuccess); });
}

void TeamsClient::chatSend(const QString &context, const QByteArray &verb, const QUrl &url, const QJsonObject &body, Callback onSuccess)
{
    if (!canFetch()) { emit errorOccurred(context, 0, QStringLiteral("missing chat token")); return; }
    QNetworkRequest req = makeChatRequest(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = m_nam.sendCustomRequest(req, verb, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, context, reply, onSuccess] { finish(context, reply, onSuccess); });
}

void TeamsClient::fetchSkypeToken(const QString &accessTokenForSpaces)
{
    // purple-teams: teams_login_get_api_skypetoken()
    QNetworkRequest req(QUrl(m_hosts.teams + QStringLiteral("/api/authsvc/v1.0/authz")));
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + accessTokenForSpaces.toUtf8());
    req.setRawHeader("Accept", "application/json; ver=1.0");
    QNetworkReply *reply = m_nam.post(req, QByteArray());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        finish(QStringLiteral("authsvc"), reply, [this](const QJsonDocument &doc, QNetworkReply *) {
            const QJsonObject obj = doc.object();
            const QString skypeToken = obj.contains(QLatin1String("tokens"))
                ? obj.value(QLatin1String("tokens")).toObject().value(QLatin1String("skypeToken")).toString()
                : obj.value(QLatin1String("skypeToken")).toObject().value(QLatin1String("skypetoken")).toString();
            if (skypeToken.isEmpty()) {
                emit errorOccurred(QStringLiteral("authsvc"), 0, QStringLiteral("answer without skypeToken"));
                return;
            }
            const QJsonObject tokens = obj.value(QLatin1String("tokens")).toObject();
            emit skypeTokenReady(skypeToken, obj.value(QLatin1String("region")).toString(),
                                 tokens.value(QLatin1String("expiresIn")).toInt());
        });
    });
}

void TeamsClient::fetchSelfProperties()
{
    // purple-teams: teams_get_self_details()
    chatGet(QStringLiteral("profile"), chatUrl(QStringLiteral("/v1/users/ME/properties")),
            [this](const QJsonDocument &doc, QNetworkReply *) { emit selfPropertiesFetched(doc.object()); });
}

void TeamsClient::fetchConversations(qint64 sinceMs)
{
    // purple-teams: teams_get_all_conversations_since(sa, since)
    QUrl url = chatUrl(QStringLiteral("/v1/users/ME/conversations"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("startTime"), QString::number(sinceMs));
    q.addQueryItem(QStringLiteral("pageSize"), QStringLiteral("100"));
    q.addQueryItem(QStringLiteral("view"), QStringLiteral("msnp24Equivalent"));
    q.addQueryItem(QStringLiteral("targetType"), QString::fromLatin1(kTargetTypes));
    url.setQuery(q);
    chatGet(QStringLiteral("conversazioni"), url, [this, sinceMs](const QJsonDocument &doc, QNetworkReply *) {
        emit conversationsFetched(doc.object().value(QLatin1String("conversations")).toArray(), sinceMs > 0);
    });
}

void TeamsClient::fetchMessages(const QString &conversationId, qint64 beforeMs)
{
    // purple-teams: teams_get_conversation_history_since(); older pages add endTime.
    const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(conversationId));
    QUrl url = chatUrl(QStringLiteral("/v1/users/ME/conversations/%1/messages").arg(enc));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("startTime"), QStringLiteral("0"));
    if (beforeMs > 0) q.addQueryItem(QStringLiteral("endTime"), QString::number(beforeMs));
    q.addQueryItem(QStringLiteral("pageSize"), QString::number(kHistoryPageSize));
    q.addQueryItem(QStringLiteral("view"), QStringLiteral("msnp24Equivalent"));
    q.addQueryItem(QStringLiteral("targetType"), QString::fromLatin1(kTargetTypes));
    url.setQuery(q);
    chatGet(QStringLiteral("messages"), url, [this, conversationId, beforeMs](const QJsonDocument &doc, QNetworkReply *) {
        emit messagesFetched(conversationId, doc.object().value(QLatin1String("messages")).toArray(), beforeMs > 0);
    });
}

void TeamsClient::sendMessage(const QString &conversationId, const QString &html, const QString &displayName,
                              const QString &replyToId, const QJsonObject &properties)
{
    // purple-teams: teams_send_message(). A channel reply goes to "<channel>;messageid=<post id>",
    // the same form as the conversationLink of replies.
    const QString target = replyToId.isEmpty() ? conversationId : conversationId + QStringLiteral(";messageid=") + replyToId;
    const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(target));
    QJsonObject body;
    body.insert(QStringLiteral("clientmessageid"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    body.insert(QStringLiteral("content"), html);
    body.insert(QStringLiteral("messagetype"), QStringLiteral("RichText/Html"));
    body.insert(QStringLiteral("contenttype"), QStringLiteral("text"));
    body.insert(QStringLiteral("imdisplayname"), displayName);
    if (!properties.isEmpty()) body.insert(QStringLiteral("properties"), properties);
    chatSend(QStringLiteral("invio"), "POST", chatUrl(QStringLiteral("/v1/users/ME/conversations/%1/messages").arg(enc)), body,
             [this, conversationId](const QJsonDocument &, QNetworkReply *) { emit messageSent(conversationId); });
}

QNetworkRequest TeamsClient::makeTeamsApiRequest(const QUrl &url, const QString &bearer) const
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + bearer.toUtf8());
    req.setRawHeader("X-Skypetoken", m_tokens.skypeToken.toUtf8());
    req.setRawHeader("Accept", "application/json");
    return req;
}

void TeamsClient::fetchChatsAndTeams(const QString &csaBearer)
{
    // purple-teams: teams_get_friend_list() -> /api/csa/api/v1/teams/users/me
    QUrl url(m_hosts.teams + QStringLiteral("/api/csa/api/v1/teams/users/me"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("isPrefetch"), QStringLiteral("false"));
    q.addQueryItem(QStringLiteral("enableMembershipSummary"), QStringLiteral("true"));
    url.setQuery(q);
    QNetworkReply *reply = m_nam.get(makeTeamsApiRequest(url, csaBearer));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        finish(QStringLiteral("teams and chats"), reply,
               [this](const QJsonDocument &doc, QNetworkReply *) { emit chatsAndTeamsFetched(doc.object()); });
    });
}

void TeamsClient::fetchProfiles(const QStringList &mris, const QString &spacesBearer)
{
    // purple-teams: teams_get_friend_profiles(); body is a JSON array of MRIs.
    const QString region = m_tokens.region.isEmpty() ? QString::fromLatin1(kDefaultChatRegion) : m_tokens.region;
    QUrl url(QStringLiteral("%1/api/mt/%2/beta/users/fetchShortProfile").arg(m_hosts.teams, region));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("isMailAddress"), QStringLiteral("false"));
    q.addQueryItem(QStringLiteral("canBeSmtpAddress"), QStringLiteral("false"));
    q.addQueryItem(QStringLiteral("enableGuest"), QStringLiteral("true"));
    q.addQueryItem(QStringLiteral("includeIBBarredUsers"), QStringLiteral("true"));
    q.addQueryItem(QStringLiteral("skypeTeamsInfo"), QStringLiteral("true"));
    q.addQueryItem(QStringLiteral("includeBots"), QStringLiteral("true"));
    url.setQuery(q);
    QNetworkRequest req = makeTeamsApiRequest(url, spacesBearer);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(QJsonArray::fromStringList(mris)).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        finish(QStringLiteral("profiles"), reply, [this](const QJsonDocument &doc, QNetworkReply *) {
            // purple-teams: teams_got_friend_profiles() reads "value", else "resolvedUsers"
            const QJsonObject o = doc.object();
            emit profilesFetched(o.contains(QLatin1String("value")) ? o.value(QLatin1String("value")).toArray()
                                                                    : o.value(QLatin1String("resolvedUsers")).toArray());
        });
    });
}

void TeamsClient::fetchCalendar(const QDateTime &from, const QDateTime &to, const QString &spacesBearer)
{
    // purple-teams uses "/api/mt/part/<partition>/v2.0/...", the web app here "/api/mt/<region>/v2.0/...".
    const QString region = m_tokens.region.isEmpty() ? QString::fromLatin1(kDefaultChatRegion) : m_tokens.region;
    QUrl url(QStringLiteral("%1/api/mt/%2/v2.0/me/calendars/default/calendarView").arg(m_hosts.teams, region));
    // "%Y-%m-%dT%H:%M:%S%z", as purple-teams sends it
    const auto stamp = [](const QDateTime &t) {
        const int offsetMin = t.offsetFromUtc() / 60;
        return t.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))
            + QStringLiteral("%1%2%3").arg(offsetMin < 0 ? QLatin1Char('-') : QLatin1Char('+'))
                  .arg(qAbs(offsetMin) / 60, 2, 10, QLatin1Char('0')).arg(qAbs(offsetMin) % 60, 2, 10, QLatin1Char('0'));
    };
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("StartDate"), QString::fromLatin1(QUrl::toPercentEncoding(stamp(from))));
    q.addQueryItem(QStringLiteral("EndDate"), QString::fromLatin1(QUrl::toPercentEncoding(stamp(to))));
    q.addQueryItem(QStringLiteral("shouldDecryptData"), QStringLiteral("true"));
    url.setQuery(q);
    QNetworkReply *reply = m_nam.get(makeTeamsApiRequest(url, spacesBearer));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        finish(QStringLiteral("calendario"), reply, [this](const QJsonDocument &doc, QNetworkReply *) {
            emit calendarFetched(doc.object().value(QLatin1String("value")).toArray());
        });
    });
}

void TeamsClient::setReaction(const QString &conversationId, const QString &messageId, const QString &key, bool add)
{
    // Not in purple-teams: the chat service's message property "emotions" (same properties
    // endpoint family as consumptionhorizon). Unverified on this tenant until first use.
    const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(conversationId));
    QUrl url = chatUrl(QStringLiteral("/v1/users/ME/conversations/%1/messages/%2/properties").arg(enc, messageId));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("name"), QStringLiteral("emotions"));
    url.setQuery(q);
    QJsonObject emotion{{QStringLiteral("key"), key}};
    if (add) emotion.insert(QStringLiteral("value"), QDateTime::currentMSecsSinceEpoch());
    const QJsonObject body{{QStringLiteral("emotions"), emotion}};
    if (!canFetch()) return;
    QNetworkRequest req = makeChatRequest(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = m_nam.sendCustomRequest(req, add ? "PUT" : "DELETE", QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, conversationId] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) return;
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        emit errorOccurred(QStringLiteral("reazione"), http, reply->errorString() + QStringLiteral(" — ") + QString::fromUtf8(reply->readAll().left(200)));
        emit reactionFailed(conversationId);
    });
}

void TeamsClient::editMessage(const QString &conversationId, const QString &messageId, const QString &html)
{
    const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(conversationId));
    const QJsonObject body{{QStringLiteral("content"), html},
                           {QStringLiteral("messagetype"), QStringLiteral("RichText/Html")},
                           {QStringLiteral("contenttype"), QStringLiteral("text")}};
    chatSend(QStringLiteral("edit"), "PUT", chatUrl(QStringLiteral("/v1/users/ME/conversations/%1/messages/%2").arg(enc, messageId)), body,
             [this, conversationId](const QJsonDocument &, QNetworkReply *) { emit messageChanged(conversationId); });
}

void TeamsClient::deleteMessage(const QString &conversationId, const QString &messageId)
{
    const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(conversationId));
    chatSend(QStringLiteral("eliminazione"), "DELETE", chatUrl(QStringLiteral("/v1/users/ME/conversations/%1/messages/%2").arg(enc, messageId)), {},
             [this, conversationId](const QJsonDocument &, QNetworkReply *) { emit messageChanged(conversationId); });
}

void TeamsClient::sendTyping(const QString &conversationId)
{
    const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(conversationId));
    const QJsonObject body{{QStringLiteral("messagetype"), QStringLiteral("Control/Typing")},
                           {QStringLiteral("contenttype"), QStringLiteral("Application/Message")},
                           {QStringLiteral("content"), QString()}};
    chatSend(QStringLiteral("typing"), "POST", chatUrl(QStringLiteral("/v1/users/ME/conversations/%1/messages").arg(enc)), body,
             [](const QJsonDocument &, QNetworkReply *) {});
}

void TeamsClient::fetchPresence(const QStringList &mris, const QString &presenceBearer)
{
    QJsonArray body;
    for (const QString &mri : mris) body.append(QJsonObject{{QStringLiteral("mri"), mri}});
    QNetworkRequest req(QUrl(m_hosts.presence + QStringLiteral("/v1/presence/getpresence/")));
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + presenceBearer.toUtf8());
    req.setRawHeader("Accept", "application/json");
    // Same client headers as purple-teams (teams_connection.c, presence host).
    req.setRawHeader("x-ms-client-user-agent", "Teams-V2-Desktop");
    req.setRawHeader("x-ms-correlation-id", "1");
    QNetworkReply *reply = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        finish(QStringLiteral("presence"), reply,
               [this](const QJsonDocument &doc, QNetworkReply *) { emit presenceFetched(doc.array()); });
    });
}

void TeamsClient::presenceCall(const QString &context, const QByteArray &verb, const QString &path, const QJsonObject &body,
                               const QString &presenceBearer, std::function<void()> onSuccess)
{
    QNetworkRequest req(QUrl(m_hosts.presence + path));
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + presenceBearer.toUtf8());
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("x-ms-client-user-agent", "Teams-V2-Desktop");
    req.setRawHeader("x-ms-correlation-id", "1");
    QNetworkReply *reply = m_nam.sendCustomRequest(req, verb, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, context, onSuccess] {
        finish(context, reply, [onSuccess](const QJsonDocument &, QNetworkReply *) { if (onSuccess) onSuccess(); });
    });
}

void TeamsClient::registerPresenceEndpoint(const QString &endpointId, const QString &presenceBearer)
{
    // purple-teams: teams_set_endpoint_statusid() — "Transport" lets reportmyactivity work.
    presenceCall(QStringLiteral("presenza (dispositivo)"), "PUT", QStringLiteral("/v1/me/endpoints/"),
                 {{QStringLiteral("id"), endpointId}, {QStringLiteral("availability"), QStringLiteral("Available")},
                  {QStringLiteral("activity"), QStringLiteral("Available")}, {QStringLiteral("activityReporting"), QStringLiteral("Transport")},
                  {QStringLiteral("deviceType"), QStringLiteral("Desktop")}},
                 presenceBearer, [this] { emit presenceEndpointRegistered(); });
}

void TeamsClient::reportActivity(const QString &endpointId, bool active, const QString &presenceBearer)
{
    // purple-teams: teams_set_idle()
    presenceCall(QStringLiteral("presence (activity)"), "POST", QStringLiteral("/v1/me/reportmyactivity/"),
                 {{QStringLiteral("endpointId"), endpointId}, {QStringLiteral("isActive"), active}}, presenceBearer);
}

void TeamsClient::forceAvailability(const QString &availability, const QString &presenceBearer)
{
    // purple-teams: teams_set_global_statusid() — "sets our status everywhere"
    presenceCall(QStringLiteral("stato"), "PUT", QStringLiteral("/v1/me/forceavailability/"),
                 {{QStringLiteral("availability"), availability}}, presenceBearer);
}

void TeamsClient::publishNote(const QString &message, const QString &presenceBearer)
{
    // purple-teams: teams_set_mood_message() — the note is HTML.
    presenceCall(QStringLiteral("messaggio di stato"), "PUT", QStringLiteral("/v1/me/publishnote"),
                 {{QStringLiteral("message"), message.toHtmlEscaped()}, {QStringLiteral("expiry"), QStringLiteral("9999-12-31T00:00:00.000Z")}},
                 presenceBearer);
}

void TeamsClient::searchUsers(const QString &text, const QString &spacesBearer)
{
    const QString region = m_tokens.region.isEmpty() ? QString::fromLatin1(kDefaultChatRegion) : m_tokens.region;
    QUrl url(QStringLiteral("%1/api/mt/%2/beta/users/searchV2").arg(m_hosts.teams, region));
    QUrlQuery q;
    for (const auto &[k, v] : {std::pair{"includeDLs", "true"}, {"includeBots", "true"}, {"searchExternalContacts", "true"},
                               {"searchImplicitContacts", "false"}, {"enableGuest", "true"}, {"source", "newChat"},
                               {"skypeTeamsInfo", "true"}})
        q.addQueryItem(QString::fromLatin1(k), QString::fromLatin1(v));
    url.setQuery(q);
    QNetworkRequest req = makeTeamsApiRequest(url, spacesBearer);
    // purple-teams posts the search text as the raw body.
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    QNetworkReply *reply = m_nam.post(req, text.toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply, text] {
        finish(QStringLiteral("search"), reply, [this, text](const QJsonDocument &doc, QNetworkReply *) {
            // purple-teams: create_search_results() reads "results", "value" or "values"
            const QJsonObject o = doc.object();
            QJsonArray users = o.value(QLatin1String("results")).toArray();
            if (users.isEmpty()) users = o.value(QLatin1String("value")).toArray();
            if (users.isEmpty()) users = o.value(QLatin1String("values")).toArray();
            emit usersFound(text, users);
        });
    });
}

void TeamsClient::createChat(const QStringList &memberMris, const QString &selfMri, bool oneToOne)
{
    QJsonArray members;
    for (const QString &mri : memberMris) members.append(QJsonObject{{QStringLiteral("id"), mri}, {QStringLiteral("role"), QStringLiteral("User")}});
    members.append(QJsonObject{{QStringLiteral("id"), selfMri}, {QStringLiteral("role"), QStringLiteral("Admin")}});
    QJsonObject properties{{QStringLiteral("threadType"), QStringLiteral("chat")}, {QStringLiteral("chatFilesIndexId"), QStringLiteral("2")}};
    if (oneToOne) {
        properties.insert(QStringLiteral("fixedRoster"), QStringLiteral("true"));
        properties.insert(QStringLiteral("uniquerosterthread"), QStringLiteral("true"));
    }
    const QJsonObject body{{QStringLiteral("members"), members}, {QStringLiteral("properties"), properties}};
    chatSend(QStringLiteral("new chat"), "POST", chatUrl(QStringLiteral("/v1/threads")), body,
             [this](const QJsonDocument &doc, QNetworkReply *reply) {
                 // purple-teams reads "id"; the Location header names the thread too.
                 QString id = doc.object().value(QLatin1String("id")).toString();
                 if (id.isEmpty())
                     id = QUrl::fromPercentEncoding(reply->rawHeader("Location")).section(QLatin1Char('/'), -1);
                 if (!id.isEmpty()) emit chatCreated(id);
             });
}

void TeamsClient::sendImage(const QString &conversationId, const QByteArray &data, const QString &fileName,
                            const QString &displayName, const QString &replyToId)
{
    if (!canFetch() || m_tokens.skypeToken.isEmpty()) { emit errorOccurred(QStringLiteral("immagine"), 0, QStringLiteral("missing skypetoken")); return; }
    const QByteArray auth = "skype_token " + m_tokens.skypeToken.toUtf8();
    const QString base = m_hosts.media + QStringLiteral("/v1/objects/");

    const QJsonObject body{{QStringLiteral("type"), QStringLiteral("pish/image")},
                           {QStringLiteral("permissions"), QJsonObject{{conversationId, QJsonArray{QStringLiteral("read")}}}},
                           {QStringLiteral("filename"), fileName},
                           {QStringLiteral("sharingMode"), QStringLiteral("Inline")}};
    QNetworkRequest create{QUrl(base)};
    create.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    create.setRawHeader("Authorization", auth);
    create.setRawHeader("x-ms-client-version", "49/25113001312");
    create.setRawHeader("Accept", "application/json");
    emit imageUploading(true);
    QNetworkReply *reply = m_nam.post(create, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [=, this] {
        reply->deleteLater();
        const QString id = QJsonDocument::fromJson(reply->readAll()).object().value(QLatin1String("id")).toString();
        if (reply->error() != QNetworkReply::NoError || id.isEmpty()) {
            emit imageUploading(false);
            emit errorOccurred(QStringLiteral("immagine"), reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->errorString());
            return;
        }
        QNetworkRequest put{QUrl(base + QString::fromLatin1(QUrl::toPercentEncoding(id)) + QStringLiteral("/content/imgpsh"))};
        put.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
        put.setRawHeader("Authorization", auth);
        put.setRawHeader("Referer", "https://teams.microsoft.com/");
        put.setRawHeader("Accept", "*/*");
        QNetworkReply *upload = m_nam.put(put, data);
        connect(upload, &QNetworkReply::finished, this, [=, this] {
            upload->deleteLater();
            emit imageUploading(false);
            if (upload->error() != QNetworkReply::NoError) {
                emit errorOccurred(QStringLiteral("immagine"), upload->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), upload->errorString());
                return;
            }
            // purple-teams: teams_conversation_send_image_part2_cb()
            const QString url = QStringLiteral("https://as-api.asm.skype.com/v1/objects/%1/views/imgo").arg(QString::fromLatin1(QUrl::toPercentEncoding(id)));
            const QString html = QStringLiteral("<p><img itemscope=\"image\" style=\"vertical-align:bottom\" src=\"%1\" alt=\"image\" "
                                                "itemtype=\"http://schema.skype.com/AMSImage\" height=\"250\" id=\"%2\" itemid=\"%2\" "
                                                "href=\"%1\" target-src=\"%1\"></p>").arg(url, id.toHtmlEscaped());
            sendMessage(conversationId, html, displayName, replyToId);
        });
    });
}

void TeamsClient::fetchReadHorizons(const QString &conversationId)
{
    const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(conversationId));
    chatGet(QStringLiteral("letture"), chatUrl(QStringLiteral("/v1/threads/%1/consumptionhorizons").arg(enc)),
            [this, conversationId](const QJsonDocument &doc, QNetworkReply *) { emit readHorizonsFetched(conversationId, doc.object()); });
}

void TeamsClient::markRead(const QString &conversationId, const QString &lastMessageId, const QString &lastClientMessageId)
{
    // purple-teams: PUT .../properties?name=consumptionhorizon {"consumptionhorizon":"<id>;<now ms>;<clientmessageid>"}
    const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(conversationId));
    QUrl url = chatUrl(QStringLiteral("/v1/users/ME/conversations/%1/properties").arg(enc));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("name"), QStringLiteral("consumptionhorizon"));
    url.setQuery(q);
    QJsonObject body;
    body.insert(QStringLiteral("consumptionhorizon"),
                QStringLiteral("%1;%2;%3").arg(lastMessageId).arg(QDateTime::currentMSecsSinceEpoch()).arg(lastClientMessageId));
    chatSend(QStringLiteral("read"), "PUT", url, body, [](const QJsonDocument &, QNetworkReply *) {});
}

void TeamsClient::setConversationProperty(const QString &conversationId, const QString &name, const QString &value)
{
    QUrl url = chatUrl(QStringLiteral("/v1/users/ME/conversations/%1/properties").arg(QString::fromLatin1(QUrl::toPercentEncoding(conversationId))));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("name"), name);
    url.setQuery(q);
    chatSend(QStringLiteral("property ") + name, "PUT", url, QJsonObject{{name, value}}, [](const QJsonDocument &, QNetworkReply *) {});
}

void TeamsClient::setThreadTopic(const QString &threadId, const QString &topic)
{
    QUrl url = chatUrl(QStringLiteral("/v1/threads/%1/properties").arg(QString::fromLatin1(QUrl::toPercentEncoding(threadId))));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("name"), QStringLiteral("topic"));
    url.setQuery(q);
    chatSend(QStringLiteral("topic"), "PUT", url, QJsonObject{{QStringLiteral("topic"), topic}}, [](const QJsonDocument &, QNetworkReply *) {});
}

void TeamsClient::addMember(const QString &threadId, const QString &mri)
{
    const QUrl url = chatUrl(QStringLiteral("/v1/threads/%1/members/%2").arg(QString::fromLatin1(QUrl::toPercentEncoding(threadId)),
                                                                             QString::fromLatin1(QUrl::toPercentEncoding(mri))));
    chatSend(QStringLiteral("add member"), "PUT", url, QJsonObject{{QStringLiteral("role"), QStringLiteral("User")}}, [](const QJsonDocument &, QNetworkReply *) {});
}

void TeamsClient::removeMember(const QString &threadId, const QString &mri)
{
    const QUrl url = chatUrl(QStringLiteral("/v1/threads/%1/members/%2").arg(QString::fromLatin1(QUrl::toPercentEncoding(threadId)),
                                                                             QString::fromLatin1(QUrl::toPercentEncoding(mri))));
    chatSend(QStringLiteral("remove member"), "DELETE", url, QJsonObject(), [](const QJsonDocument &, QNetworkReply *) {});
}
