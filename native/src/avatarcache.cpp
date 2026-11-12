#include "avatarcache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>

namespace {
constexpr int kMaxParallel = 4;
constexpr int kMaxAgeDays = 7;  // pictures change rarely; refresh weekly
}

AvatarCache::AvatarCache(QObject *parent) : QObject(parent)
{
    m_dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/avatars");
    QDir().mkpath(m_dir);
}

void AvatarCache::configure(const QString &selfMri, const QString &region, TokenProvider tokens)
{
    // "8:orgid:<guid>" -> "<guid>"
    m_selfGuid = selfMri.section(QLatin1Char(':'), -1);
    m_region = region;
    m_tokens = std::move(tokens);
    pump();
}

void AvatarCache::clear()
{
    m_known.clear();
    m_queue.clear();
    m_queued.clear();
    m_selfGuid.clear();
}

QString AvatarCache::pathFor(const QString &mri, const QString &suffix) const
{
    // MRIs contain ':' — hash them into a safe file name.
    return m_dir + QLatin1Char('/')
        + QString::fromLatin1(QCryptographicHash::hash(mri.toUtf8(), QCryptographicHash::Sha1).toHex()) + suffix;
}

QString AvatarCache::urlFor(const QString &mri)
{
    if (mri.isEmpty()) return {};
    const auto known = m_known.constFind(mri);
    if (known != m_known.cend()) return *known;

    const QDateTime freshAfter = QDateTime::currentDateTime().addDays(-kMaxAgeDays);
    const QFileInfo picture(pathFor(mri, QStringLiteral(".img")));
    const QFileInfo none(pathFor(mri, QStringLiteral(".none")));
    if (picture.exists() && picture.lastModified() > freshAfter) {
        const QString url = QUrl::fromLocalFile(picture.filePath()).toString();
        m_known.insert(mri, url);
        return url;
    }
    if (none.exists() && none.lastModified() > freshAfter) {
        m_known.insert(mri, QString());
        return {};
    }
    // Show the old picture meanwhile, if there is one.
    const QString stale = picture.exists() ? QUrl::fromLocalFile(picture.filePath()).toString() : QString();
    m_known.insert(mri, stale);
    if (!m_queued.contains(mri)) {
        m_queued.insert(mri);
        m_queue.append(mri);
        pump();
    }
    return stale;
}

void AvatarCache::pump()
{
    if (m_selfGuid.isEmpty() || !m_tokens) return;
    while (m_running < kMaxParallel && !m_queue.isEmpty()) {
        const QString mri = m_queue.takeFirst();
        ++m_running;
        m_tokens([this, mri](const QString &token) { download(mri, token); });
    }
}

void AvatarCache::download(const QString &mri, const QString &token)
{
    // purple-teams: teams_get_icon_now(). The picture endpoint takes the token as a cookie,
    // as a browser <img> request would.
    const QString url = QStringLiteral("https://teams.microsoft.com/api/mt/%1/beta/users/%2/profilepicturev2/%3?size=HR128x128")
                            .arg(m_region, QString::fromLatin1(QUrl::toPercentEncoding(m_selfGuid)),
                                 QString::fromLatin1(QUrl::toPercentEncoding(mri)));
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("Referer", "https://teams.microsoft.com/");
    req.setRawHeader("Cookie", "authtoken=Bearer%3D" + token.toUtf8() + "%26Origin%3Dhttps%3A%2F%2Fteams.microsoft.com");
    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, mri] {
        reply->deleteLater();
        --m_running;
        m_queued.remove(mri);
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray type = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        const QByteArray data = reply->readAll();
        if (reply->error() == QNetworkReply::NoError && type.startsWith("image/") && !data.isEmpty()) {
            QFile f(pathFor(mri, QStringLiteral(".img")));
            if (f.open(QIODevice::WriteOnly)) {
                f.write(data);
                f.close();
                QFile::remove(pathFor(mri, QStringLiteral(".none")));
                m_known.insert(mri, QUrl::fromLocalFile(f.fileName()).toString());
                emit avatarReady(mri);
            }
        } else if (http == 404) {
            // No picture: remember it for a while.
            QFile f(pathFor(mri, QStringLiteral(".none")));
            if (f.open(QIODevice::WriteOnly)) f.close();
        }
        pump();
    });
}
