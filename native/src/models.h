#pragma once

#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QDateTime>
#include <QStringList>
#include <QVariantList>
#include <QVector>
#include <QString>

struct ConversationItem {
    QString id;
    QString title;
    QString type;
    QString preview;        // text of the last message
    qint64 lastActivity = 0; // ms since epoch of the last message
    QString lastMessageId;
    bool unread = false;
    bool hasTopic = false;  // a title chosen by people; otherwise it is made from member names
    QString peerMri;        // one-to-one chats: the other person
    QString lastFromMri;    // who sent the last message
    bool pinned = false;    // Teams "favorite": kept at the top
    bool muted = false;     // Teams "alerts" off: no notifications
};

class ConversationModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles { IdRole = Qt::UserRole + 1, TitleRole, TypeRole, PreviewRole, LastActivityRole, UnreadRole, PeerRole, PinnedRole, MutedRole };
    explicit ConversationModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    // Rows are kept sorted by lastActivity, newest first.
    void setItems(const QVector<ConversationItem> &items);
    // Inserts or replaces the conversation with the same id, moving it to its sorted place.
    void upsert(const ConversationItem &item);
    const ConversationItem *find(const QString &id) const;
    void setUnread(const QString &id, bool unread);
    void setFlags(const QString &id, bool pinned, bool muted);
    void setTitle(const QString &id, const QString &title);
    void setPeer(const QString &id, const QString &peerMri);
    int unreadCount() const;
    void remove(const QString &id);
private:
    QVector<ConversationItem> m_items;
};

struct CalendarEvent {
    QString id;
    QString subject;
    QDateTime start;
    QDateTime end;
    QString location;
    QString organizer;
    QString chatId;   // the meeting chat, for Teams meetings
    QString showAs;   // Busy, Tentative, Free, Oof...
    QString joinUrl;  // Teams meeting link
    QString iCalUid;  // to find the same event in Microsoft Graph
    QString myResponse; // Accepted, Declined, TentativelyAccepted, NotResponded, Organizer...
    bool online = false;
};

// Events of one week, by start time; the "day" role is the section (a localized date).
class CalendarModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles { IdRole = Qt::UserRole + 1, SubjectRole, StartRole, EndRole, TimeRangeRole, DayRole,
                 LocationRole, OrganizerRole, ChatIdRole, OnlineRole, ShowAsRole, AllDayRole, NowRole, JoinUrlRole,
                 ICalUidRole, MyResponseRole };
    explicit CalendarModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setItems(const QVector<CalendarEvent> &items);
    const QVector<CalendarEvent> &items() const { return m_items; }
    void setMyResponse(const QString &iCalUid, const QString &response);
private:
    QVector<CalendarEvent> m_items;
};

// Chat list filter: title text and, optionally, unread chats only.
class ConversationFilter : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setUnreadOnly(bool on)
    {
        if (m_unreadOnly == on) return;
        beginFilterChange();
        m_unreadOnly = on;
        endFilterChange();
    }
protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        if (!QSortFilterProxyModel::filterAcceptsRow(row, parent)) return false;
        return !m_unreadOnly || sourceModel()->index(row, 0, parent).data(ConversationModel::UnreadRole).toBool();
    }
private:
    bool m_unreadOnly = false;
};

struct ActivityItem {
    QString id, type, subtype, who, whoMri, text, preview, where, threadId, messageId;
    qint64 timestamp = 0;
    bool read = false;
};

// The activity feed (mentions, replies, reactions), newest first.
class ActivityModel : public QAbstractListModel
{
    Q_OBJECT
public:
    void markAllRead();
    const QVector<ActivityItem> &items() const { return m_items; }
    enum Roles { IdRole = Qt::UserRole + 1, WhoRole, WhoMriRole, TextRole, PreviewRole, WhereRole, ThreadIdRole, MessageIdRole,
                 TimestampRole, ReadRole };
    explicit ActivityModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setItems(const QVector<ActivityItem> &items);
    int unreadCount() const;
private:
    QVector<ActivityItem> m_items;
};

struct ChannelItem {
    QString id;       // conversation id of the channel ("19:...@thread.tacv2")
    QString name;
    QString teamId;
    QString teamName;
    QString groupId;  // the team's Microsoft 365 group (for its SharePoint files)
    bool general = false;
    bool unread = false;
    qint64 lastActivity = 0;
};

// Channels of all the user's teams, grouped by team (ListView sections on "team").
class ChannelModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles { IdRole = Qt::UserRole + 1, NameRole, TeamRole, GeneralRole, UnreadRole, LastActivityRole };
    explicit ChannelModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setItems(const QVector<ChannelItem> &items);
    const ChannelItem *find(const QString &id) const;
    void updateActivity(const QString &id, qint64 lastActivity, bool unread);
private:
    QVector<ChannelItem> m_items;
};

struct MessageItem {
    QString id;
    QString clientMessageId;
    QString from;
    QString fromId;      // sender MRI
    QString content;     // plain text
    QString richText;    // the same text, escaped, with clickable links
    QStringList images;  // remote URLs of pictures in the message
    QVariantList files;  // [{name, type, url}]
    QVariantList reactions; // [{key, emoji, count, mine, names}]
    bool edited = false;
    bool deleted = false;
    bool mentionsMe = false;
    int seenCount = -1;  // own newest message only: how many others read it (-1: not shown)
    int seenTotal = 0;
    QString time;
    qint64 timestamp = 0;
    bool mine = false;
    QString rootId;      // channels: id of the post this message belongs to (itself for a post)
    int replyCount = 0;  // channels, posts only: replies loaded so far
};

// Rows are newest first, to match a ListView with verticalLayoutDirection: BottomToTop.
class MessageModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles { IdRole = Qt::UserRole + 1, FromRole, ContentRole, TimeRole, TimestampRole, MineRole, ReplyCountRole, FromIdRole,
                 RichTextRole, ImagesRole, FilesRole, ReactionsRole,
                 EditedRole, DeletedRole, MentionsMeRole, SeenCountRole, SeenTotalRole };
    explicit MessageModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setItems(const QVector<MessageItem> &items);
    // `newestFirst` is a fresh page of the same conversation: adds the messages not shown
    // yet at the top (newest end) and updates the content of the ones already shown.
    void merge(const QVector<MessageItem> &newestFirst);
    // An older history page (newest first): appended after the oldest row, skipping known ids.
    void appendOlder(const QVector<MessageItem> &newestFirst);
    void setReplyCount(const QString &id, int count);
    void setReactions(const QString &id, const QVariantList &reactions);
    // Read position (message id) of every other member: drives the "seen" line.
    void setReadHorizons(const QList<qint64> &horizons);
    const MessageItem *find(const QString &id) const;
    const QVector<MessageItem> &items() const { return m_items; }
private:
    void updateSeen();
    QVector<MessageItem> m_items;
    QList<qint64> m_horizons;
};
