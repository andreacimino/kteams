#pragma once

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <KNotification>
#include <QProcess>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QString>
#include <QTimer>
#include "avatarcache.h"
#include "chatparser.h"
#include "fileuploader.h"
#include "mediacache.h"
#include "nativecall.h"
#include "demo.h"
#include "screencast.h"
#include "models.h"
#include "oauth.h"
#include "teamsclient.h"
#include "trouter.h"

class Backend : public QObject
{
    Q_OBJECT
public:
    enum class State { Starting, SignedOut, WaitingForCode, Connecting, Connected };
    Q_ENUM(State)

private:
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString userCode READ userCode NOTIFY deviceCodeChanged)
    Q_PROPERTY(QString verificationUrl READ verificationUrl NOTIFY deviceCodeChanged)
    Q_PROPERTY(ConversationModel* conversations READ conversations CONSTANT)
    // Same rows, filtered by setConversationFilter() (search box).
    Q_PROPERTY(ConversationFilter* filteredConversations READ filteredConversations CONSTANT)
    Q_PROPERTY(ChannelModel* channels READ channels CONSTANT)
    Q_PROPERTY(ActivityModel* activity READ activity CONSTANT)
    Q_PROPERTY(int activityUnread READ activityUnread NOTIFY activityChanged)
    Q_PROPERTY(MessageModel* messages READ messages CONSTANT)
    Q_PROPERTY(QString currentConversationId READ currentConversationId NOTIFY currentConversationChanged)
    Q_PROPERTY(QString currentConversationTitle READ currentConversationTitle NOTIFY currentConversationChanged)
    Q_PROPERTY(QString selfDisplayName READ selfDisplayName NOTIFY selfChanged)
    Q_PROPERTY(bool currentIsChannel READ currentIsChannel NOTIFY currentConversationChanged)
    // Search inside the open chat: the messages loaded so far, filtered by text.
    Q_PROPERTY(QSortFilterProxyModel* filteredMessages READ filteredMessages CONSTANT)
    Q_PROPERTY(QString chatFilter READ chatFilter WRITE setChatFilter NOTIFY chatFilterChanged)
    Q_PROPERTY(bool hasMoreHistory READ hasMoreHistory NOTIFY historyChanged)
    Q_PROPERTY(bool loadingOlder READ loadingOlder NOTIFY historyChanged)
    // Channels: the open thread (post + replies), newest first like `messages`.
    Q_PROPERTY(MessageModel* thread READ thread CONSTANT)
    Q_PROPERTY(CalendarModel* calendar READ calendar CONSTANT)
    // Bumped when profile pictures arrive, so bindings on avatarUrl() re-evaluate.
    Q_PROPERTY(int avatarRevision READ avatarRevision NOTIFY avatarsChanged)
    Q_PROPERTY(int mediaRevision READ mediaRevision NOTIFY mediaChanged)
    Q_PROPERTY(int presenceRevision READ presenceRevision NOTIFY presenceChanged)
    // "Mario sta scrivendo..." for the open conversation, or "".
    Q_PROPERTY(QString typingText READ typingText NOTIFY typingChanged)
    // True while the push channel (Trouter) is up: updates arrive instantly.
    Q_PROPERTY(bool realtime READ realtime NOTIFY realtimeChanged)
    Q_PROPERTY(bool uploading READ uploading NOTIFY uploadingChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY downloadingChanged)
    // Native calls (prototype): state text and whether a call is on.
    Q_PROPERTY(QString nativeCallState READ nativeCallState NOTIFY nativeCallChanged)
    Q_PROPERTY(bool nativeCallActive READ nativeCallActive NOTIFY nativeCallChanged)
    Q_PROPERTY(bool nativeCallIncoming READ nativeCallIncoming NOTIFY nativeCallChanged)
    // Video in the native call: our camera, and whether the other side is sending video.
    Q_PROPERTY(bool cameraOn READ cameraOn NOTIFY nativeCallChanged)
    Q_PROPERTY(bool remoteVideo READ remoteVideo NOTIFY nativeCallChanged)
    Q_PROPERTY(bool sharingOn READ sharingOn NOTIFY nativeCallChanged)
    Q_PROPERTY(bool remoteShare READ remoteShare NOTIFY nativeCallChanged)
    Q_PROPERTY(QString selfMriValue READ selfMri NOTIFY selfChanged)
    // Chats with unread messages (tray, task manager badge).
    Q_PROPERTY(int unreadCount READ unreadCount NOTIFY unreadCountChanged)
    // 0..1 while a file uploads, -1 otherwise.
    Q_PROPERTY(double uploadProgress READ uploadProgress NOTIFY uploadingChanged)
    // New chat: people found for the last searchPeople() query: [{mri, name, email}]
    Q_PROPERTY(QVariantList peopleResults READ peopleResults NOTIFY peopleResultsChanged)
    Q_PROPERTY(QString calendarTitle READ calendarTitle NOTIFY calendarChanged)
    Q_PROPERTY(int calendarWeekOffset READ calendarWeekOffset NOTIFY calendarChanged)
    // The chat of the call in progress (the open one for a call started from it, the meeting's
    // chat, the one-to-one chat with the caller): "" when there is none or no call.
    Q_PROPERTY(QString callChatId READ callChatId NOTIFY nativeCallChanged)
    Q_PROPERTY(QString callChatTitle READ callChatTitle NOTIFY nativeCallChanged)
    Q_PROPERTY(QString currentThreadId READ currentThreadId NOTIFY currentThreadChanged)

public:
    explicit Backend(QObject *parent = nullptr);

    State state() const { return m_state; }
    QString statusMessage() const { return m_status; }
    QString errorMessage() const { return m_error; }
    QString userCode() const { return m_userCode; }
    QString verificationUrl() const { return m_verificationUrl; }
    ConversationModel* conversations() { return &m_conversations; }
    ConversationFilter* filteredConversations() { return &m_filteredConversations; }
    ChannelModel* channels() { return &m_channels; }
    ActivityModel* activity() { return &m_activity; }
    int activityUnread() const { return m_activity.unreadCount(); }
    MessageModel* messages() { return &m_messages; }
    QString currentConversationId() const { return m_currentConversationId; }
    QString currentConversationTitle() const { return Demo::title(m_currentConversationTitle); }
    QString selfDisplayName() const { return Demo::name(m_selfDisplayName); }
    bool currentIsChannel() const { return m_channelIds.contains(m_currentConversationId); }
    bool hasMoreHistory() const { return m_hasMoreHistory; }
    QSortFilterProxyModel *filteredMessages() { return &m_filteredMessages; }
    QString chatFilter() const { return m_chatFilter; }
    void setChatFilter(const QString &text);
    bool loadingOlder() const { return m_loadingOlder; }
    MessageModel* thread() { return &m_thread; }
    CalendarModel* calendar() { return &m_calendar; }
    int avatarRevision() const { return m_avatarRevision; }
    int mediaRevision() const { return m_mediaRevision; }
    int presenceRevision() const { return m_presenceRevision; }
    QString typingText() const { return Demo::enabled() && !m_typingText.isEmpty() ? tr("%1 is typing...").arg(Demo::name(m_typingText)) : m_typingText; }
    bool realtime() const { return m_trouter.isConnected(); }
    bool uploading() const { return m_uploading || m_files.busy(); }
    bool downloading() const { return m_downloading; }
    QString nativeCallState() const { return m_nativeCall.state(); }
    bool nativeCallActive() const { return m_nativeCall.isActive(); }
    bool nativeCallIncoming() const { return m_nativeCall.isIncoming(); }
    Q_INVOKABLE void acceptNativeCall() { m_nativeCall.accept(); }
    Q_INVOKABLE void rejectNativeCall() { m_nativeCall.reject(); }
    Q_INVOKABLE void startNativeTestCall();
    // Native audio call to the other person of the open one-to-one chat.
    Q_INVOKABLE void startNativeCall();
    Q_PROPERTY(bool canNativeCall READ canNativeCall NOTIFY currentConversationChanged)
    bool canNativeCall() const;
    Q_INVOKABLE void hangUpNativeCall() { m_nativeCall.hangUp(); }
    Q_INVOKABLE void muteNativeCall(bool muted) { m_nativeCall.setMuted(muted); }
    bool cameraOn() const { return m_nativeCall.cameraEnabled(); }
    bool remoteVideo() const { return m_nativeCall.remoteVideoActive(); }
    bool sharingOn() const { return m_nativeCall.screenShareEnabled() || m_screenCast.active(); }
    bool remoteShare() const { return m_nativeCall.remoteShareActive(); }
    Q_INVOKABLE void setCameraEnabled(bool on);
    // Screen sharing: the system asks which screen or window; stops with the call too.
    Q_INVOKABLE void startScreenShare();
    Q_INVOKABLE void stopScreenShare();
    // The QML VideoOutput sinks (VideoOutput.videoSink): remote video, camera preview, remote screen.
    Q_INVOKABLE void setVideoSinks(QObject *remote, QObject *local, QObject *share);
    int unreadCount() const { return m_unreadCount; }
    double uploadProgress() const { return m_files.busy() ? m_uploadProgress : -1; }
    QVariantList peopleResults() const { return m_peopleResults; }
    // "Available", "Busy", "DoNotDisturb", "Away", "BeRightBack", "Offline"... or "" if unknown.
    Q_INVOKABLE QString presenceOf(const QString &mri) const { return m_presence.value(mri); }
    // Local URL of an image shown in a message, or "" (then downloaded in the background).
    Q_INVOKABLE QString mediaUrl(const QString &remoteUrl) { return m_media.urlFor(remoteUrl); }
    // Local URL of a user's profile picture, or "" (then fetched in the background).
    Q_INVOKABLE QString avatarUrl(const QString &mri) { return m_avatars.urlFor(mri); }
    QString calendarTitle() const { return m_calendarTitle; }
    int calendarWeekOffset() const { return m_calendarWeekOffset; }
    QString currentThreadId() const { return m_currentThreadId; }
    QString callChatId() const;
    QString callChatTitle() const { return m_nativeCall.isActive() ? m_nativeCall.peerName() : QString(); }

    Q_INVOKABLE void startLogin();
    Q_INVOKABLE void cancelLogin();
    Q_INVOKABLE void copyToClipboard(const QString &text);
    Q_INVOKABLE void setConversationFilter(const QString &text);
    // Chat list filter: only chats with unread messages.
    Q_PROPERTY(bool unreadOnly READ unreadOnly WRITE setUnreadOnly NOTIFY unreadOnlyChanged)
    bool unreadOnly() const { return m_unreadOnly; }
    void setUnreadOnly(bool on);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void logout();
    // fallbackTitle: shown when the conversation is not in the list (e.g. an older meeting chat).
    Q_INVOKABLE void selectConversation(const QString &id, const QString &fallbackTitle = QString());
    Q_INVOKABLE void sendMessage(const QString &text);
    // Members of the open chat (for @mentions): [{mri, name}]
    Q_PROPERTY(QVariantList currentMembers READ currentMembers NOTIFY currentConversationChanged)
    QVariantList currentMembers() const;
    // mentions: [{mri, name}] of the "@Name" occurrences in text; inThread: channel reply.
    Q_INVOKABLE void sendMessageWithMentions(const QString &text, const QVariantList &mentions, bool inThread);
    // Loads the previous history page of the open conversation (scrolling up).
    Q_INVOKABLE void loadOlder();
    Q_INVOKABLE void openThread(const QString &postId);
    Q_INVOKABLE void closeThread();
    Q_INVOKABLE void sendReply(const QString &text);
    // Calendar, one week at a time: 0 = this week, -1 = last week, 1 = next week.
    Q_INVOKABLE void loadCalendarWeek(int weekOffset);
    // response: "accept", "decline" or "tentativelyAccept" (Microsoft Graph).
    Q_INVOKABLE void respondToEvent(const QString &iCalUid, const QString &response);
    // Adds the user's reaction `key` to a message of the open conversation, or removes it.
    Q_INVOKABLE void toggleReaction(const QString &messageId, const QString &key);
    // The six classic Teams reactions, for the picker: [{key, emoji}].
    Q_INVOKABLE QVariantList quickReactions() const;
    Q_INVOKABLE void editMessage(const QString &messageId, const QString &text);
    Q_INVOKABLE void deleteMessage(const QString &messageId);
    // Called while the user types; sends "is typing" at most every few seconds.
    Q_INVOKABLE void userTyping();
    Q_INVOKABLE void searchPeople(const QString &text);
    // Opens the one-to-one chat with this person, creating it if needed.
    Q_INVOKABLE void startChatWith(const QString &mri, const QString &name);
    // Images: a file chosen by the user, or the picture in the clipboard (Ctrl+V).
    // inThread: send as a reply in the open channel thread.
    Q_INVOKABLE void sendImageFile(const QUrl &file, bool inThread);
    Q_INVOKABLE bool clipboardHasImage() const;
    Q_INVOKABLE void sendClipboardImage(bool inThread);
    // Any file: uploaded to OneDrive / the channel's SharePoint folder and shared as Teams does.
    Q_INVOKABLE void sendFile(const QUrl &file, bool inThread);
    // The attachments staged in the composer, sent together: pictures inline, other files
    // through OneDrive/SharePoint one after the other. `urls`: file URLs.
    Q_INVOKABLE void sendAttachments(const QVariantList &urls, bool inThread);
    // The picture in the clipboard saved as a file (a day in the cache folder); empty URL on failure.
    Q_INVOKABLE QUrl stageClipboardImage();
    // Native video call: an audio call whose camera goes on once connected.
    Q_INVOKABLE void startNativeVideoCall();
    // Attachments: a shared file (from a message's files) or a picture, into ~/Downloads.
    Q_INVOKABLE void downloadFile(const QVariantMap &file);
    Q_INVOKABLE void saveImage(const QString &remoteUrl);
    Q_INVOKABLE void joinMeeting(const QString &joinUrl);
    // Chat management (Teams: favorite = pinned, alerts off = muted, hidden; group title and members).
    Q_INVOKABLE void pinChat(const QString &id, bool pinned);
    Q_INVOKABLE void muteChat(const QString &id, bool muted);
    Q_INVOKABLE void hideChat(const QString &id);
    Q_INVOKABLE void renameChat(const QString &id, const QString &title);
    Q_INVOKABLE void leaveChat(const QString &id);
    Q_INVOKABLE void addMembers(const QString &id, const QStringList &mris);
    // Activity feed: everything read; filter "" (all), "mention", "reaction", "reply".
    Q_INVOKABLE void markActivityRead();
    // Search in the messages this client has seen: the open chats' pages, the local cache
    // and, on the first search, the newest page of every chat (fetched one by one).
    Q_INVOKABLE void searchMessages(const QString &text);
    Q_PROPERTY(QVariantList messageResults READ messageResults NOTIFY messageResultsChanged)
    Q_PROPERTY(bool indexing READ indexing NOTIFY messageResultsChanged)
    QVariantList messageResults() const { return m_messageResults; }
    bool indexing() const { return !m_indexQueue.isEmpty(); }
    // Quoting a message in the next one sent (Teams "reply" blockquote).
    Q_INVOKABLE void setQuote(const QString &messageId);
    Q_INVOKABLE void clearQuote();
    // A Teams meeting in the calendar (Microsoft Graph); attendees: e-mail addresses.
    Q_INVOKABLE void createMeeting(const QString &subject, const QDateTime &start, const QDateTime &end, const QStringList &attendees);
    Q_PROPERTY(QString activityFilter READ activityFilter WRITE setActivityFilter NOTIFY activityChanged)
    // Demo mode helpers (screenshots): the tab to open at start (KTEAMS_START_TAB, 0-3), or -1.
    Q_PROPERTY(int startTab READ startTab CONSTANT)
    int startTab() const { return qEnvironmentVariableIsSet("KTEAMS_START_TAB") ? qEnvironmentVariableIntValue("KTEAMS_START_TAB") : -1; }
    Q_PROPERTY(QString quoteWho READ quoteWho NOTIFY quoteChanged)
    Q_PROPERTY(QString quoteText READ quoteText NOTIFY quoteChanged)
    QString activityFilter() const { return m_activityFilter; }
    void setActivityFilter(const QString &filter);
    QString quoteWho() const { return Demo::name(m_quoteWho); }
    QString quoteText() const { return Demo::text(m_quoteText); }
    // Own status: "Available", "Busy", "DoNotDisturb", "BeRightBack", "Away", "Offline".
    Q_INVOKABLE void setMyAvailability(const QString &availability);
    Q_INVOKABLE void setStatusMessage(const QString &message);

signals:
    void stateChanged();
    void statusMessageChanged();
    void errorMessageChanged();
    void deviceCodeChanged();
    void currentConversationChanged();
    void historyChanged();
    void currentThreadChanged();
    void calendarChanged();
    void avatarsChanged();
    void mediaChanged();
    void presenceChanged();
    void typingChanged();
    void realtimeChanged();
    void uploadingChanged();
    void downloadingChanged();
    void nativeCallChanged();
    void unreadCountChanged();
    void unreadOnlyChanged();
    void activityChanged();
    void quoteChanged();
    void messageResultsChanged();
    void chatFilterChanged();
    void peopleResultsChanged();
    void selfChanged();
    // A notification was clicked: the UI should show this conversation.
    void openConversationRequested(const QString &id);

private:
    void setState(State s);
    void setStatus(const QString &s);
    void setError(const QString &e);
    void connectChat();
    void onSkypeToken(const QString &token, const QString &region, int expiresInSecs);
    void onClientError(const QString &context, int httpStatus, const QString &message);
    void onAuthFailed(const QString &message, bool sessionLost);
    void resetChatState();
    void handleSelf(const QJsonObject &props);
    void handleConversations(const QJsonArray &arr, bool incremental);
    void handleMessages(const QString &conversationId, const QJsonArray &arr, bool older);
    void refreshThread();
    void handleCalendar(const QJsonArray &events);
    void scheduleReminders(const QVector<CalendarEvent> &events);
    QSet<QString> m_reminded;
    void pollPresence();
    void pollOwnPresence(); // just our own status, for after an activity change
    // First poll now, then every minute (a second call only polls again).
    void startPresencePolling();
    void presenceToken(std::function<void(const QString &)> cb);
    void updateActivity();
    void handlePresence(const QJsonArray &list);
    void handleEvent(const QJsonObject &event);
    QString selfMri() const { return QStringLiteral("8:") + m_selfSkypeName; }
    // Who we are and who the others are, for the parsers in chatparser.h.
    ChatParser::SelfContext selfContext() const { return {m_selfSkypeName, m_people, m_chatMembers}; }
    void loadPeople();
    void handleChatsAndTeams(const QJsonObject &summary);
    void handleProfiles(const QJsonArray &profiles);
    void applyMemberTitles();
    void markCurrentRead();
    bool isUnread(const QString &id) const;
    void parseTeams(const QJsonArray &teams);
    void handleActivity(const QJsonArray &arr);
    // Local cache (chats, channels, people, recent messages): instant start, offline reading.
    QString cacheDir() const;
    void saveStateCache();
    void loadStateCache();
    void saveMessagesCache(const QString &conversationId, const QVector<MessageItem> &items);
    bool loadMessagesCache(const QString &conversationId);
    QTimer m_cacheSaveTimer;
    bool m_messagesFromCache = false;
    void scheduleSync();
    void sync();
    void notifyNewMessage(const ConversationItem &conv);
    // The person's cached profile picture on a notification, when there is one.
    void setNotificationPicture(KNotification *n, const QString &mri);

    OAuth m_oauth;
    TeamsClient m_client;
    QTimer m_skypeRenewTimer;
    QTimer m_syncTimer;
    qint64 m_lastSyncMs = 0; // newest activity seen, for incremental sync
    using Person = ChatParser::Person;
    // Profile card: {mri, name, email, jobTitle, department, presence}
    Q_INVOKABLE QVariantMap personInfo(const QString &mri) const;
    // Opens (or creates) the one-to-one chat with this person.
    Q_INVOKABLE void openChatWith(const QString &mri) { startChatWith(mri, m_people.value(mri).displayName); }
    QHash<QString, QStringList> m_chatMembers; // conversation id -> MRIs of the other members
    QHash<QString, Person> m_people;          // MRI -> names
    ConversationModel m_conversations;
    ConversationFilter m_filteredConversations;
    QSortFilterProxyModel m_filteredMessages;
    QString m_chatFilter;
    bool m_unreadOnly = false;
    ChannelModel m_channels;
    ActivityModel m_activity;
    QSet<QString> m_channelIds;
    MessageModel m_messages;
    MessageModel m_thread;
    QString m_currentThreadId;
    QHash<QString, QVector<MessageItem>> m_replies; // channel post id -> replies, newest first
    qint64 m_oldestLoadedMs = 0;  // oldest message of any type loaded, for the next history page
    bool m_hasMoreHistory = false;
    bool m_loadingOlder = false;
    int m_autoFillPages = 0;
    int m_peopleRetries = 0;
    int m_syncTicks = 0;
    QHash<QString, QString> m_presence; // MRI -> availability
    QTimer m_presenceTimer;
    int m_presenceRevision = 0;
    QHash<QString, qint64> m_lastTypingSent; // conversation -> ms
    QVariantList m_peopleResults;
    QString m_pendingChatTitle;  // name for the chat being created
    Trouter m_trouter;
    QTimer m_eventSync;     // coalesces pushed events into one sync
    QTimer m_typingExpire;
    QString m_typingText;
    bool m_uploading = false;
    FileUploader m_files;
    double m_uploadProgress = 0;
    struct QueuedFile { QUrl file; QString conversationId; QString replyTo; };
    QList<QueuedFile> m_fileQueue;  // files waiting for the one being uploaded
    bool m_fileSending = false;     // an upload was started and has not ended yet
    void startNextFile();
    QString m_uploadConversation;  // where the file being uploaded goes
    QString m_uploadReplyTo;
    QNetworkAccessManager m_downloads;
    bool m_downloading = false;
    NativeCall m_nativeCall;
    bool m_cameraWhenConnected = false;
    bool m_peopleLoaded = false;
    QHash<QString, QVector<MessageItem>> m_searchIndex; // conversation -> newest messages
    QVariantList m_messageResults;
    QStringList m_indexQueue;
    QString m_searchQuery;
    QTimer m_indexTimeout;
    bool m_indexedAll = false;
    void indexNext();
    void loadSearchIndex();
    void runSearch();
    QString m_activityFilter;
    QVector<ActivityItem> m_allActivity;
    QSet<QString> m_knownActivity;   // ids seen: a new unread one is notified
    bool m_activityLoaded = false;
    QString m_quoteId, m_quoteMri, m_quoteWho, m_quoteText;
    bool doNotDisturb() const { return m_presence.value(selfMri()) == QLatin1String("DoNotDisturb"); }
    QString quoteHtml();
    ScreenCastPortal m_screenCast;
    QPointer<KNotification> m_ringNotification;
    void notifyDownloaded(const QString &path);
    bool m_systemIdle = false; // no keyboard or mouse input on the desktop for a while (KIdleTime)
    int m_idleTimeoutId = 0;
    bool m_reportedActive = false;
    int m_unreadCount = 0;
    void sendImageData(const QByteArray &data, const QString &fileName, bool inThread);
    CalendarModel m_calendar;
    AvatarCache m_avatars;
    QTimer m_avatarBatch;  // many pictures arrive together: one refresh for a batch
    int m_avatarRevision = 0;
    MediaCache m_media;
    QTimer m_mediaBatch;
    int m_mediaRevision = 0;
    QDateTime m_calendarFrom;
    QString m_calendarTitle;
    int m_calendarWeekOffset = 0;
    State m_state = State::Starting;
    QString m_status;
    QString m_error;
    QString m_userCode;
    QString m_verificationUrl;
    QString m_currentConversationId;
    QString m_currentConversationTitle;
    QString m_selfSkypeName;
    QString m_selfDisplayName;
    QString m_selfUpn;
};
