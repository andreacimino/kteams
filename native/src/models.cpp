#include "models.h"
#include "demo.h"

#include <QHash>
#include <QLocale>
#include <QSet>
#include <algorithm>

ConversationModel::ConversationModel(QObject *parent) : QAbstractListModel(parent) {}

int ConversationModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_items.size()); }

QVariant ConversationModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) return {};
    const auto &it = m_items[index.row()];
    switch (role) {
    case IdRole: return it.id;
    case TitleRole: return Demo::title(it.title);
    case TypeRole: return it.type;
    case PreviewRole: return Demo::text(it.preview);
    case LastActivityRole: return QVariant::fromValue(it.lastActivity);
    case PinnedRole: return it.pinned;
    case MutedRole: return it.muted;
    case UnreadRole: return it.unread;
    case PeerRole: return it.peerMri;
    }
    return {};
}

QHash<int, QByteArray> ConversationModel::roleNames() const
{
    return {{IdRole, "id"}, {TitleRole, "title"}, {TypeRole, "type"}, {PreviewRole, "preview"},
            {LastActivityRole, "lastActivity"}, {UnreadRole, "unread"}, {PeerRole, "peer"}, {PinnedRole, "pinned"}, {MutedRole, "muted"}};
}

void ConversationModel::setItems(const QVector<ConversationItem> &items)
{
    beginResetModel();
    m_items = items;
    std::stable_sort(m_items.begin(), m_items.end(), [](const ConversationItem &a, const ConversationItem &b) {
        return a.pinned != b.pinned ? a.pinned : a.lastActivity > b.lastActivity; });
    endResetModel();
}

void ConversationModel::upsert(const ConversationItem &item)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id != item.id) continue;
        beginRemoveRows({}, i, i);
        m_items.removeAt(i);
        endRemoveRows();
        break;
    }
    // Pinned chats stay on top, each group newest first.
    const auto pos = std::find_if(m_items.cbegin(), m_items.cend(), [&](const ConversationItem &it) {
        return it.pinned != item.pinned ? item.pinned : it.lastActivity < item.lastActivity; });
    const int row = int(pos - m_items.cbegin());
    beginInsertRows({}, row, row);
    m_items.insert(row, item);
    endInsertRows();
}

const ConversationItem *ConversationModel::find(const QString &id) const
{
    for (const auto &it : m_items)
        if (it.id == id) return &it;
    return nullptr;
}

void ConversationModel::setFlags(const QString &id, bool pinned, bool muted)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id != id) continue;
        if (m_items[i].pinned == pinned && m_items[i].muted == muted) return;
        ConversationItem it = m_items[i];
        it.pinned = pinned;
        it.muted = muted;
        upsert(it); // the row may move: pinned chats sit on top
        return;
    }
}

void ConversationModel::setUnread(const QString &id, bool unread)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id != id || m_items[i].unread == unread) continue;
        m_items[i].unread = unread;
        const QModelIndex idx = index(i);
        emit dataChanged(idx, idx, {UnreadRole});
    }
}

void ConversationModel::setTitle(const QString &id, const QString &title)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id != id || m_items[i].title == title) continue;
        m_items[i].title = title;
        emit dataChanged(index(i), index(i), {TitleRole});
    }
}

void ConversationModel::setPeer(const QString &id, const QString &peerMri)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id != id || m_items[i].peerMri == peerMri) continue;
        m_items[i].peerMri = peerMri;
        emit dataChanged(index(i), index(i), {PeerRole});
    }
}

int ConversationModel::unreadCount() const
{
    return int(std::count_if(m_items.cbegin(), m_items.cend(), [](const ConversationItem &c) { return c.unread; }));
}

void ConversationModel::remove(const QString &id)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id != id) continue;
        beginRemoveRows({}, i, i);
        m_items.removeAt(i);
        endRemoveRows();
        return;
    }
}

ChannelModel::ChannelModel(QObject *parent) : QAbstractListModel(parent) {}

int ChannelModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_items.size()); }

QVariant ChannelModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) return {};
    const auto &it = m_items[index.row()];
    switch (role) {
    case IdRole: return it.id;
    case NameRole: return Demo::title(it.name);
    case TeamRole: return Demo::title(it.teamName);
    case GeneralRole: return it.general;
    case UnreadRole: return it.unread;
    case LastActivityRole: return QVariant::fromValue(it.lastActivity);
    }
    return {};
}

QHash<int, QByteArray> ChannelModel::roleNames() const
{
    return {{IdRole, "id"}, {NameRole, "name"}, {TeamRole, "team"}, {GeneralRole, "general"},
            {UnreadRole, "unread"}, {LastActivityRole, "lastActivity"}};
}

void ChannelModel::setItems(const QVector<ChannelItem> &items) { beginResetModel(); m_items = items; endResetModel(); }

const ChannelItem *ChannelModel::find(const QString &id) const
{
    for (const auto &it : m_items)
        if (it.id == id) return &it;
    return nullptr;
}

void ChannelModel::updateActivity(const QString &id, qint64 lastActivity, bool unread)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id != id) continue;
        if (m_items[i].lastActivity == lastActivity && m_items[i].unread == unread) return;
        m_items[i].lastActivity = lastActivity;
        m_items[i].unread = unread;
        emit dataChanged(index(i), index(i), {UnreadRole, LastActivityRole});
        return;
    }
}

MessageModel::MessageModel(QObject *parent) : QAbstractListModel(parent) {}

int MessageModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_items.size()); }

QVariant MessageModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) return {};
    const auto &it = m_items[index.row()];
    switch (role) {
    case IdRole: return it.id;
    case FromRole: return Demo::name(it.from);
    case ContentRole: return Demo::text(it.content);
    case TimeRole: return it.time;
    case TimestampRole: return QVariant::fromValue(it.timestamp);
    case MineRole: return it.mine;
    case ReplyCountRole: return it.replyCount;
    case FromIdRole: return it.fromId;
    case RichTextRole: return Demo::enabled() ? Demo::text(it.content).toHtmlEscaped() : it.richText;
    case ImagesRole: return Demo::enabled() ? QStringList() : it.images;
    case FilesRole: return Demo::enabled() ? QVariantList() : it.files;
    case ReactionsRole: return it.reactions;
    case EditedRole: return it.edited;
    case DeletedRole: return it.deleted;
    case MentionsMeRole: return it.mentionsMe;
    case SeenCountRole: return it.seenCount;
    case SeenTotalRole: return it.seenTotal;
    }
    return {};
}

QHash<int, QByteArray> MessageModel::roleNames() const
{
    return {{IdRole, "id"}, {FromRole, "from"}, {ContentRole, "content"}, {TimeRole, "time"},
            {TimestampRole, "timestamp"}, {MineRole, "mine"}, {ReplyCountRole, "replyCount"}, {FromIdRole, "fromId"},
            {RichTextRole, "richText"}, {ImagesRole, "images"}, {FilesRole, "files"}, {ReactionsRole, "reactions"},
            {EditedRole, "edited"}, {DeletedRole, "deleted"}, {MentionsMeRole, "mentionsMe"},
            {SeenCountRole, "seenCount"}, {SeenTotalRole, "seenTotal"}};
}

void MessageModel::setItems(const QVector<MessageItem> &items)
{
    beginResetModel();
    m_items = items;
    if (items.isEmpty()) m_horizons.clear(); // another conversation
    endResetModel();
    updateSeen();
}

void MessageModel::merge(const QVector<MessageItem> &newestFirst)
{
    QHash<QString, int> shown;
    for (int i = 0; i < m_items.size(); ++i) shown.insert(m_items[i].id, i);

    QVector<MessageItem> fresh;
    for (const auto &m : newestFirst) {
        const auto it = shown.constFind(m.id);
        if (it == shown.cend()) {
            // Only messages newer than what is shown; older ones belong to history paging.
            if (m_items.isEmpty() || m.timestamp >= m_items.first().timestamp) fresh.push_back(m);
            continue;
        }
        auto &shownItem = m_items[*it];
        if (shownItem.content != m.content || shownItem.edited != m.edited || shownItem.deleted != m.deleted) {
            const QVariantList reactions = shownItem.reactions;
            const int replies = shownItem.replyCount;
            shownItem = m;
            shownItem.reactions = reactions; // compared just below
            shownItem.replyCount = replies;
            emit dataChanged(index(*it), index(*it));
        }
        if (m_items[*it].reactions != m.reactions) {
            m_items[*it].reactions = m.reactions;
            emit dataChanged(index(*it), index(*it), {ReactionsRole});
        }
    }
    if (fresh.isEmpty()) { updateSeen(); return; }
    beginInsertRows({}, 0, int(fresh.size()) - 1);
    m_items = fresh + m_items;
    endInsertRows();
    updateSeen();
}

void MessageModel::appendOlder(const QVector<MessageItem> &newestFirst)
{
    QSet<QString> shown;
    for (const auto &m : m_items) shown.insert(m.id);
    QVector<MessageItem> older;
    for (const auto &m : newestFirst)
        if (!shown.contains(m.id)) older.push_back(m);
    if (older.isEmpty()) return;
    const int first = int(m_items.size());
    beginInsertRows({}, first, first + int(older.size()) - 1);
    m_items += older;
    endInsertRows();
}

void MessageModel::setReplyCount(const QString &id, int count)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id != id || m_items[i].replyCount == count) continue;
        m_items[i].replyCount = count;
        emit dataChanged(index(i), index(i), {ReplyCountRole});
        return;
    }
}

CalendarModel::CalendarModel(QObject *parent) : QAbstractListModel(parent) {}

int CalendarModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_items.size()); }

static bool isAllDay(const CalendarEvent &e)
{
    const QDateTime s = e.start.toLocalTime(), en = e.end.toLocalTime();
    return s.time() == QTime(0, 0) && en.time() == QTime(0, 0) && s.daysTo(en) >= 1;
}

QVariant CalendarModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) return {};
    const auto &e = m_items[index.row()];
    const QLocale locale;
    switch (role) {
    case IdRole: return e.id;
    case SubjectRole: return Demo::title(e.subject);
    case StartRole: return e.start;
    case EndRole: return e.end;
    case TimeRangeRole:
        if (isAllDay(e)) return tr("All day");
        return locale.toString(e.start.toLocalTime().time(), QLocale::ShortFormat) + QStringLiteral(" – ")
            + locale.toString(e.end.toLocalTime().time(), QLocale::ShortFormat);
    case DayRole: {
        const QString day = locale.toString(e.start.toLocalTime().date(), QStringLiteral("dddd d MMMM"));
        return e.start.toLocalTime().date() == QDate::currentDate() ? tr("Today, %1").arg(day) : day;
    }
    case LocationRole: return Demo::enabled() ? QString() : e.location;
    case OrganizerRole: return Demo::name(e.organizer);
    case ChatIdRole: return e.chatId;
    case OnlineRole: return e.online;
    case ShowAsRole: return e.showAs;
    case JoinUrlRole: return e.joinUrl;
    case ICalUidRole: return e.iCalUid;
    case MyResponseRole: return e.myResponse;
    case AllDayRole: return isAllDay(e);
    case NowRole: {
        const QDateTime now = QDateTime::currentDateTime();
        return e.start <= now && now < e.end;
    }
    }
    return {};
}

QHash<int, QByteArray> CalendarModel::roleNames() const
{
    return {{IdRole, "id"}, {SubjectRole, "subject"}, {StartRole, "start"}, {EndRole, "end"},
            {TimeRangeRole, "timeRange"}, {DayRole, "day"}, {LocationRole, "location"}, {OrganizerRole, "organizer"},
            {ChatIdRole, "chatId"}, {OnlineRole, "online"}, {ShowAsRole, "showAs"}, {AllDayRole, "allDay"}, {NowRole, "now"}, {JoinUrlRole, "joinUrl"},
            {ICalUidRole, "iCalUid"}, {MyResponseRole, "myResponse"}};
}

void CalendarModel::setItems(const QVector<CalendarEvent> &items) { beginResetModel(); m_items = items; endResetModel(); }

void MessageModel::setReactions(const QString &id, const QVariantList &reactions)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id != id) continue;
        m_items[i].reactions = reactions;
        emit dataChanged(index(i), index(i), {ReactionsRole});
        return;
    }
}

const MessageItem *MessageModel::find(const QString &id) const
{
    for (const auto &m : m_items)
        if (m.id == id) return &m;
    return nullptr;
}

void MessageModel::setReadHorizons(const QList<qint64> &horizons)
{
    m_horizons = horizons;
    updateSeen();
}

void MessageModel::updateSeen()
{
    // As in Teams: the line sits under the newest of my messages only.
    int newestMine = -1;
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items[i].mine && !m_items[i].deleted) { newestMine = i; break; }
    const int total = int(m_horizons.size());
    for (int i = 0; i < m_items.size(); ++i) {
        auto &m = m_items[i];
        int count = -1;
        if (i == newestMine && !m_horizons.isEmpty()) {
            // Message ids grow with time: a member whose horizon reached this id has read it.
            const qint64 id = m.id.toLongLong();
            count = int(std::count_if(m_horizons.cbegin(), m_horizons.cend(), [id](qint64 h) { return h >= id; }));
        }
        if (m.seenCount == count && m.seenTotal == total) continue;
        m.seenCount = count;
        m.seenTotal = total;
        emit dataChanged(index(i), index(i), {SeenCountRole, SeenTotalRole});
    }
}

ActivityModel::ActivityModel(QObject *parent) : QAbstractListModel(parent) {}

int ActivityModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_items.size()); }

QVariant ActivityModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size()) return {};
    const auto &it = m_items[index.row()];
    switch (role) {
    case IdRole: return it.id;
    case WhoRole: return Demo::name(it.who);
    case WhoMriRole: return it.whoMri;
    case TextRole: return it.text;
    case PreviewRole: return Demo::text(it.preview);
    case WhereRole: return Demo::title(it.where);
    case ThreadIdRole: return it.threadId;
    case MessageIdRole: return it.messageId;
    case TimestampRole: return QVariant::fromValue(it.timestamp);
    case ReadRole: return it.read;
    }
    return {};
}

QHash<int, QByteArray> ActivityModel::roleNames() const
{
    return {{IdRole, "id"}, {WhoRole, "who"}, {WhoMriRole, "whoMri"}, {TextRole, "what"}, {PreviewRole, "preview"}, {WhereRole, "where"},
            {ThreadIdRole, "threadId"}, {MessageIdRole, "messageId"}, {TimestampRole, "timestamp"}, {ReadRole, "read"}};
}

void ActivityModel::markAllRead()
{
    bool changed = false;
    for (auto &it : m_items) if (!it.read) { it.read = true; changed = true; }
    if (!changed || m_items.isEmpty()) return;
    emit dataChanged(index(0), index(m_items.size() - 1), {ReadRole});
}

void ActivityModel::setItems(const QVector<ActivityItem> &items)
{
    beginResetModel();
    m_items = items;
    std::stable_sort(m_items.begin(), m_items.end(), [](const ActivityItem &a, const ActivityItem &b) { return a.timestamp > b.timestamp; });
    endResetModel();
}

int ActivityModel::unreadCount() const
{
    return int(std::count_if(m_items.cbegin(), m_items.cend(), [](const ActivityItem &a) { return !a.read; }));
}

void CalendarModel::setMyResponse(const QString &iCalUid, const QString &response)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].iCalUid != iCalUid) continue;
        m_items[i].myResponse = response;
        emit dataChanged(index(i), index(i), {MyResponseRole});
    }
}
