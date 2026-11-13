#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>

class QNetworkReply;

struct TeamsTokens {
    // Teams v2 web authenticates chatsvc with a Bearer token; older clients (purple-teams)
    // with a skypetoken. The Bearer token is preferred when known.
    QString chatBearer;
    QString skypeToken;
    QString region;
    // Base URL of the chat service, without the trailing "/v1/...":
    // e.g. "https://emea.ng.msg.teams.microsoft.com" or "https://teams.cloud.microsoft/api/chatsvc/emea".
    QString chatServiceBase;
};

// Where the services other than the chat service live (the chat service's address is in TeamsTokens).
// The defaults are Microsoft's; the tests point them at a fake server.
struct TeamsHosts {
    QString teams = QStringLiteral("https://teams.microsoft.com");                     // sign-in, CSA, profiles, calendar, search
    QString presence = QStringLiteral("https://presence.teams.microsoft.com");
    QString media = QStringLiteral("https://as-prod.asyncgw.teams.microsoft.com");     // picture upload
};

// Chat service client. Endpoints and headers follow purple-teams (work account build):
// https://github.com/EionRobb/purple-teams (teams_messages.c, teams_connection.c).
class TeamsClient : public QObject
{
    Q_OBJECT
public:
    explicit TeamsClient(QObject *parent = nullptr);

    void setTokens(const TeamsTokens &tokens);
    TeamsTokens tokens() const { return m_tokens; }
    void setHosts(const TeamsHosts &hosts);
    bool canFetch() const { return !m_tokens.chatBearer.isEmpty() || !m_tokens.skypeToken.isEmpty(); }

    // Exchange an access token for api.spaces.skype.com for a skypetoken (authsvc).
    void fetchSkypeToken(const QString &accessTokenForSpaces);
    void fetchSelfProperties();
    // sinceMs > 0: only the conversations with activity after that time (incremental sync).
    void fetchConversations(qint64 sinceMs = 0);
    // Messages per history page.
    static constexpr int kHistoryPageSize = 50;
    // Newest page, or with beforeMs > 0 the page of messages older than that time.
    void fetchMessages(const QString &conversationId, qint64 beforeMs = 0);
    // With replyToId (channels): a reply in the thread of that post.
    void sendMessage(const QString &conversationId, const QString &html, const QString &displayName,
                     const QString &replyToId = QString(), const QJsonObject &properties = QJsonObject());
    // Chats with their members, and teams with their channels, in one call (chat service
    // aggregator). `csaBearer` is an access token for https://chatsvcagg.teams.microsoft.com.
    void fetchChatsAndTeams(const QString &csaBearer);
    // Names of users by MRI ("8:orgid:<guid>"). `spacesBearer` is the api.spaces.skype.com token.
    void fetchProfiles(const QStringList &mris, const QString &spacesBearer);
    // Calendar events between two local times (purple-teams: teams_check_calendar()).
    void fetchCalendar(const QDateTime &from, const QDateTime &to, const QString &spacesBearer);
    // Adds (or with add=false removes) the user's reaction `key` ("like", "heart", ...) on a message.
    void setReaction(const QString &conversationId, const QString &messageId, const QString &key, bool add);
    // Own messages. Not in purple-teams: PUT/DELETE on the message, to confirm on first use.
    void editMessage(const QString &conversationId, const QString &messageId, const QString &html);
    void deleteMessage(const QString &conversationId, const QString &messageId);
    // "Is typing" (purple-teams: teams_send_typing()).
    void sendTyping(const QString &conversationId);
    // Availability of users (purple-teams: teams_lookup_contact_statuses()).
    void fetchPresence(const QStringList &mris, const QString &presenceBearer);
    // Own presence (purple-teams, teams_messages.c / teams_contacts.c): register this device,
    // report activity, force an availability everywhere, publish a status note.
    void registerPresenceEndpoint(const QString &endpointId, const QString &presenceBearer);
    void reportActivity(const QString &endpointId, bool active, const QString &presenceBearer);
    void forceAvailability(const QString &availability, const QString &presenceBearer);
    void publishNote(const QString &message, const QString &presenceBearer);
    // People search for a new chat (purple-teams: teams_search_users_text()).
    void searchUsers(const QString &text, const QString &spacesBearer);
    // New chat with these people (purple-teams: teams_initiate_chat()).
    void createChat(const QStringList &memberMris, const QString &selfMri, bool oneToOne);
    // Uploads an image to the media service and sends it as a message (purple-teams:
    // teams_conversation_send_image()). replyToId: channel thread, as in sendMessage().
    void sendImage(const QString &conversationId, const QByteArray &data, const QString &fileName,
                   const QString &displayName, const QString &replyToId = QString());
    // How far each member has read (seen in Teams web traffic: threads/<id>/consumptionhorizons).
    void fetchReadHorizons(const QString &conversationId);
    void markRead(const QString &conversationId, const QString &lastMessageId, const QString &lastClientMessageId);
    // A conversation property as the Skype/Teams chat service keeps them: "favorite"
    // (pinned), "alerts" (notifications), "hidden"; values are the strings "true"/"false".
    void setConversationProperty(const QString &conversationId, const QString &name, const QString &value);
    // Group chats (purple-teams: teams_chat_set_topic/invite/kick): title and members.
    void setThreadTopic(const QString &threadId, const QString &topic);
    void addMember(const QString &threadId, const QString &mri);
    void removeMember(const QString &threadId, const QString &mri);

signals:
    void skypeTokenReady(const QString &skypeToken, const QString &region, int expiresInSecs);
    void selfPropertiesFetched(const QJsonObject &properties);
    void conversationsFetched(const QJsonArray &conversations, bool incremental);
    void messagesFetched(const QString &conversationId, const QJsonArray &messages, bool older);
    void messageSent(const QString &conversationId);
    void chatsAndTeamsFetched(const QJsonObject &summary);
    void profilesFetched(const QJsonArray &profiles);
    void calendarFetched(const QJsonArray &events);
    void reactionFailed(const QString &conversationId);
    void messageChanged(const QString &conversationId);
    void presenceFetched(const QJsonArray &presences);
    void usersFound(const QString &query, const QJsonArray &users);
    void chatCreated(const QString &conversationId);
    void imageUploading(bool busy);
    void presenceEndpointRegistered();
    void readHorizonsFetched(const QString &conversationId, const QJsonObject &response);
    // httpStatus is 0 for network errors.
    void errorOccurred(const QString &context, int httpStatus, const QString &message);

private:
    using Callback = std::function<void(const QJsonDocument &, QNetworkReply *)>;
    QUrl chatUrl(const QString &path) const;
    QNetworkRequest makeChatRequest(const QUrl &url) const;
    // teams.microsoft.com/api/{csa,mt}: Bearer token plus skypetoken (purple-teams, teams_connection.c).
    QNetworkRequest makeTeamsApiRequest(const QUrl &url, const QString &bearer) const;
    void presenceCall(const QString &context, const QByteArray &verb, const QString &path, const QJsonObject &body,
                      const QString &presenceBearer, std::function<void()> onSuccess = {});
    void chatGet(const QString &context, const QUrl &url, Callback onSuccess);
    void chatSend(const QString &context, const QByteArray &verb, const QUrl &url, const QJsonObject &body, Callback onSuccess);
    void finish(const QString &context, QNetworkReply *reply, const Callback &onSuccess);

    QNetworkAccessManager m_nam;
    TeamsTokens m_tokens;
    TeamsHosts m_hosts;
};
