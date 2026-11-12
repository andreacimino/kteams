#include "mediacache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>

namespace {
constexpr int kMaxParallel = 3;
constexpr qint64 kMaxImageBytes = 15 * 1024 * 1024;
}

MediaCache::MediaCache(QObject *parent) : QObject(parent)
{
    m_dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/media");
    QDir().mkpath(m_dir);
}

void MediaCache::clear()
{
    m_known.clear();
    m_queue.clear();
    m_skypeToken.clear();
}

bool MediaCache::isProtectedImage(const QUrl &url)
{
    // purple-teams: teams_uri_is_protected_image()
    if (url.scheme() != QLatin1String("https") || !url.userInfo().isEmpty()) return false;
    const QString host = url.host();
    const bool mediaHost = host == QLatin1String("api.asm.skype.com") || host.endsWith(QLatin1String("-api.asm.skype.com"))
        || host.endsWith(QLatin1String(".asyncgw.teams.microsoft.com"));
    const QString path = url.path();
    return mediaHost
        && (path.contains(QLatin1String("/views/imgpsh_fullsize")) || path.contains(QLatin1String("/views/imgt1"))
            || path.contains(QLatin1String("/views/imgo")));
}

QString MediaCache::pathFor(const QString &remoteUrl) const
{
    return m_dir + QLatin1Char('/')
        + QString::fromLatin1(QCryptographicHash::hash(remoteUrl.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString MediaCache::urlFor(const QString &remoteUrl)
{
    if (remoteUrl.isEmpty()) return {};
    const auto known = m_known.constFind(remoteUrl);
    if (known != m_known.cend()) return *known;

    // Message images never change: a file on disk is good forever.
    const QFileInfo file(pathFor(remoteUrl));
    if (file.exists() && file.size() > 0) {
        const QString url = QUrl::fromLocalFile(file.filePath()).toString();
        m_known.insert(remoteUrl, url);
        return url;
    }
    m_known.insert(remoteUrl, QString());
    m_queue.append(remoteUrl);
    pump();
    return {};
}

void MediaCache::pump()
{
    while (m_running < kMaxParallel && !m_queue.isEmpty()) {
        const QString remote = m_queue.takeFirst();
        const QUrl url(remote);
        if (url.scheme() != QLatin1String("https")) continue;
        const bool protectedImage = isProtectedImage(url);
        if (protectedImage && m_skypeToken.isEmpty()) continue;

        QNetworkRequest req(url);
        req.setRawHeader("Accept", "image/*");
        // Credentials only for Microsoft's media service, and no redirects that could carry them away.
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         protectedImage ? QNetworkRequest::ManualRedirectPolicy : QNetworkRequest::NoLessSafeRedirectPolicy);
        if (protectedImage) req.setRawHeader("Cookie", "skypetoken_asm=" + m_skypeToken.toUtf8());

        ++m_running;
        QNetworkReply *reply = m_nam.get(req);
        connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
            if (received > kMaxImageBytes) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, remote] {
            reply->deleteLater();
            --m_running;
            const QByteArray type = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
            const QByteArray data = reply->readAll();
            if (reply->error() == QNetworkReply::NoError && type.startsWith("image/") && !data.isEmpty()) {
                QFile f(pathFor(remote));
                if (f.open(QIODevice::WriteOnly)) {
                    f.write(data);
                    f.close();
                    m_known.insert(remote, QUrl::fromLocalFile(f.fileName()).toString());
                    emit mediaReady(remote);
                }
            }
            pump();
        });
    }
}
