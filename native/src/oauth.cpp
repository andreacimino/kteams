#include "oauth.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <qt6keychain/keychain.h>

namespace {
// purple-teams: TEAMS_WORK_OAUTH_CLIENT_ID, TEAMS_OAUTH_RESOURCE, teams_get_tenant_host() without a tenant.
constexpr auto kClientId = "1fec8e78-bce4-4aaf-ab1b-5451cc387264";
constexpr auto kLoginResource = "https://api.spaces.skype.com";
constexpr auto kAuthority = "https://login.microsoftonline.com/organizations";
constexpr auto kKeychainService = "kteams";
constexpr auto kKeychainKey = "refresh_token";
// Renew access tokens a little before they expire.
constexpr int kExpirySlackSecs = 120;

QJsonObject readJson(QNetworkReply *reply)
{
    return QJsonDocument::fromJson(reply->readAll()).object();
}

int intField(const QJsonObject &o, const char *name)
{
    // The v1 endpoints return some numbers as strings.
    const QJsonValue v = o.value(QLatin1String(name));
    return v.isString() ? v.toString().toInt() : v.toInt();
}
}

OAuth::OAuth(QObject *parent) : QObject(parent)
{
    connect(&m_pollTimer, &QTimer::timeout, this, &OAuth::pollDeviceCode);
}

QNetworkReply *OAuth::postForm(const QString &path, const QList<QPair<QString, QString>> &fields)
{
    QUrlQuery form;
    for (const auto &f : fields)
        form.addQueryItem(f.first, QString::fromLatin1(QUrl::toPercentEncoding(f.second)));
    QNetworkRequest req(QUrl(QString::fromLatin1(kAuthority) + path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    req.setRawHeader("Accept", "application/json");
    return m_nam.post(req, form.toString(QUrl::FullyEncoded).toLatin1());
}

void OAuth::loadStoredSession()
{
    auto *job = new QKeychain::ReadPasswordJob(QString::fromLatin1(kKeychainService), this);
    job->setKey(QString::fromLatin1(kKeychainKey));
    connect(job, &QKeychain::Job::finished, this, [this, job] {
        m_refreshToken = job->error() == QKeychain::NoError ? job->textData() : QString();
        if (!m_refreshToken.isEmpty()) { emit ready(true); return; }
        // A session stored under the app's previous name (ac-teams-native): taken over.
        auto *old = new QKeychain::ReadPasswordJob(QStringLiteral("ac-teams-native"), this);
        old->setKey(QString::fromLatin1(kKeychainKey));
        connect(old, &QKeychain::Job::finished, this, [this, old] {
            const QString token = old->error() == QKeychain::NoError ? old->textData() : QString();
            if (!token.isEmpty()) {
                storeRefreshToken(token);
                auto *remove = new QKeychain::DeletePasswordJob(QStringLiteral("ac-teams-native"), this);
                remove->setKey(QString::fromLatin1(kKeychainKey));
                remove->setAutoDelete(true);
                remove->start();
            }
            emit ready(!m_refreshToken.isEmpty());
        });
        old->start();
    });
    job->start();
}

void OAuth::storeRefreshToken(const QString &token)
{
    if (token.isEmpty() || token == m_refreshToken) return;
    m_refreshToken = token;
    auto *job = new QKeychain::WritePasswordJob(QString::fromLatin1(kKeychainService), this);
    job->setKey(QString::fromLatin1(kKeychainKey));
    job->setTextData(token);
    connect(job, &QKeychain::Job::finished, this, [this, job] {
        if (job->error() != QKeychain::NoError)
            emit failed(tr("Cannot save the session in the keychain: %1").arg(job->errorString()), false);
    });
    job->start();
}

void OAuth::signOut()
{
    cancelDeviceLogin();
    m_refreshToken.clear();
    m_cache.clear();
    m_pending.clear();
    auto *job = new QKeychain::DeletePasswordJob(QString::fromLatin1(kKeychainService), this);
    job->setKey(QString::fromLatin1(kKeychainKey));
    job->start();
}

void OAuth::startDeviceLogin()
{
    // purple-teams: teams_do_devicecode_login()
    QNetworkReply *reply = postForm(QStringLiteral("/oauth2/devicecode"),
                                    {{QStringLiteral("client_id"), QString::fromLatin1(kClientId)},
                                     {QStringLiteral("resource"), QString::fromLatin1(kLoginResource)}});
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const QJsonObject o = readJson(reply);
        m_deviceCode = o.value(QLatin1String("device_code")).toString();
        if (m_deviceCode.isEmpty()) {
            emit failed(tr("Sign-in not started: %1").arg(o.value(QLatin1String("error_description")).toString(reply->errorString())), false);
            return;
        }
        QString url = o.value(QLatin1String("verification_url")).toString();
        if (url.isEmpty()) url = o.value(QLatin1String("verification_uri")).toString();
        const int interval = qMax(intField(o, "interval"), 1);
        m_deviceCodeExpiresAt = QDateTime::currentDateTimeUtc().addSecs(intField(o, "expires_in"));
        m_pollTimer.start(interval * 1000);
        emit deviceCodeReceived(o.value(QLatin1String("user_code")).toString(), url, o.value(QLatin1String("message")).toString());
    });
}

void OAuth::cancelDeviceLogin()
{
    m_pollTimer.stop();
    m_deviceCode.clear();
}

void OAuth::pollDeviceCode()
{
    if (m_deviceCode.isEmpty()) { m_pollTimer.stop(); return; }
    if (QDateTime::currentDateTimeUtc() > m_deviceCodeExpiresAt) {
        cancelDeviceLogin();
        emit failed(tr("The code has expired: try again."), false);
        return;
    }
    // purple-teams: teams_devicecode_login_poll()
    QNetworkReply *reply = postForm(QStringLiteral("/oauth2/token"),
                                    {{QStringLiteral("client_id"), QString::fromLatin1(kClientId)},
                                     {QStringLiteral("grant_type"), QStringLiteral("urn:ietf:params:oauth:grant-type:device_code")},
                                     {QStringLiteral("code"), m_deviceCode}});
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (m_deviceCode.isEmpty()) return; // cancelled meanwhile
        const QJsonObject o = readJson(reply);
        const QString error = o.value(QLatin1String("error")).toString();
        if (error == QLatin1String("authorization_pending") || error == QLatin1String("slow_down")) return;
        cancelDeviceLogin();
        const QString refresh = o.value(QLatin1String("refresh_token")).toString();
        if (refresh.isEmpty()) {
            emit failed(tr("Sign-in failed: %1").arg(o.value(QLatin1String("error_description")).toString(reply->errorString())), false);
            return;
        }
        // This first token is for kLoginResource: keep it instead of asking again right away.
        const QString access = o.value(QLatin1String("access_token")).toString();
        if (!access.isEmpty())
            m_cache.insert(QString::fromLatin1(kLoginResource),
                           {access, QDateTime::currentDateTimeUtc().addSecs(intField(o, "expires_in") - kExpirySlackSecs)});
        storeRefreshToken(refresh);
        emit signedIn();
    });
}

void OAuth::accessToken(const QString &resource, TokenCallback cb)
{
    const auto cached = m_cache.constFind(resource);
    if (cached != m_cache.cend() && cached->expiresAt > QDateTime::currentDateTimeUtc()) {
        cb(cached->token);
        return;
    }
    if (m_refreshToken.isEmpty()) {
        emit failed(tr("No session: sign in again."), true);
        return;
    }
    // Several callers may ask for the same resource at once: one request serves them all.
    auto &waiting = m_pending[resource];
    waiting.append(std::move(cb));
    if (waiting.size() > 1) return;

    // purple-teams: teams_oauth_refresh_token_for_resource()
    QNetworkReply *reply = postForm(QStringLiteral("/oauth2/v2.0/token"),
                                    {{QStringLiteral("scope"), resource + QStringLiteral("/.default openid profile offline_access")},
                                     {QStringLiteral("client_id"), QString::fromLatin1(kClientId)},
                                     {QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
                                     {QStringLiteral("refresh_token"), m_refreshToken}});
    connect(reply, &QNetworkReply::finished, this, [this, reply, resource] {
        reply->deleteLater();
        const QList<TokenCallback> callbacks = m_pending.take(resource);
        const QJsonObject o = readJson(reply);
        const QString access = o.value(QLatin1String("access_token")).toString();
        if (access.isEmpty()) {
            const QString error = o.value(QLatin1String("error")).toString();
            const bool lost = error == QLatin1String("invalid_grant") || error == QLatin1String("interaction_required");
            if (lost) signOut();
            emit failed(tr("Token for %1 not obtained: %2")
                            .arg(resource, o.value(QLatin1String("error_description")).toString(reply->errorString()).section(QLatin1Char('\n'), 0, 0)),
                        lost);
            return;
        }
        storeRefreshToken(o.value(QLatin1String("refresh_token")).toString());
        m_cache.insert(resource, {access, QDateTime::currentDateTimeUtc().addSecs(intField(o, "expires_in") - kExpirySlackSecs)});
        for (const auto &cb : callbacks) cb(access);
    });
}
