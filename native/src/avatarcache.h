#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <functional>

// Profile pictures by MRI ("8:orgid:<guid>"), downloaded once and kept on disk.
// Lookups are cheap: a picture is only downloaded the first time a view asks for it,
// a few at a time. Users without a picture are remembered too, so they are not
// asked again until the entry expires.
class AvatarCache : public QObject
{
    Q_OBJECT
public:
    // Calls back with a Bearer token for api.spaces.skype.com.
    using TokenProvider = std::function<void(std::function<void(const QString &)>)>;

    explicit AvatarCache(QObject *parent = nullptr);

    // selfMri: whose "users/<guid>" path the requests use; region: as in /api/mt/<region>/.
    void configure(const QString &selfMri, const QString &region, TokenProvider tokens);
    // Local file URL of the picture, or an empty string (then it is fetched in the background).
    QString urlFor(const QString &mri);
    void clear();

signals:
    void avatarReady(const QString &mri);

private:
    QString pathFor(const QString &mri, const QString &suffix) const;
    void pump();
    void download(const QString &mri, const QString &token);

    QNetworkAccessManager m_nam;
    QString m_dir;
    QString m_selfGuid;
    QString m_region;
    TokenProvider m_tokens;
    QHash<QString, QString> m_known;  // mri -> file URL ("" = no picture)
    QStringList m_queue;
    QSet<QString> m_queued;
    int m_running = 0;
};
