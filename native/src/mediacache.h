#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QStringList>

// Images shown in messages (pasted pictures, GIFs), downloaded on demand into the
// cache folder. The skypetoken is attached only to Microsoft's media service hosts
// (same check as purple-teams' teams_uri_is_protected_image()); other images are
// fetched without credentials.
class MediaCache : public QObject
{
    Q_OBJECT
public:
    explicit MediaCache(QObject *parent = nullptr);

    void setSkypeToken(const QString &token) { m_skypeToken = token; }
    // Local file URL for a remote image, or "" while it downloads (or if it failed).
    QString urlFor(const QString &remoteUrl);
    void clear();

    static bool isProtectedImage(const QUrl &url);

signals:
    void mediaReady(const QString &remoteUrl);

private:
    QString pathFor(const QString &remoteUrl) const;
    void pump();

    QNetworkAccessManager m_nam;
    QString m_dir;
    QString m_skypeToken;
    QHash<QString, QString> m_known;  // remote URL -> local URL ("" = pending or failed)
    QStringList m_queue;
    int m_running = 0;
};
