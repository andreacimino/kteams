#pragma once

#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>

// Where the refresh token is kept between runs. The default is the system keychain; the
// tests use one in memory.
class TokenStore
{
public:
    virtual ~TokenStore() = default;
    // The stored token, or an empty string.
    virtual void read(std::function<void(const QString &token)> done) = 0;
    // `done` gets an empty string on success, else the error.
    virtual void write(const QString &token, std::function<void(const QString &error)> done) = 0;
    virtual void remove() = 0;
};

// Native Microsoft sign-in for a work/school account, without any embedded browser:
// OAuth device code flow, then access tokens per resource from the stored refresh token.
// Client id, endpoints and parameters follow purple-teams (teams_login.c, work build).
// The refresh token is kept in the system keychain (KWallet / Secret Service).
class OAuth : public QObject
{
    Q_OBJECT
public:
    using TokenCallback = std::function<void(const QString &accessToken)>;

    explicit OAuth(QObject *parent = nullptr);
    // For the tests: another token store, and another login server than Microsoft's.
    void setTokenStore(std::unique_ptr<TokenStore> store);
    void setAuthority(const QString &authority);

    // Reads the refresh token from the keychain; emits ready(hasSession).
    void loadStoredSession();
    void startDeviceLogin();
    void cancelDeviceLogin();
    // Forgets the refresh token (also in the keychain) and every cached access token.
    void signOut();

    // Calls `cb` with an access token for `resource` (e.g. "https://api.spaces.skype.com"),
    // from the cache or refreshed. On failure emits failed() and does not call `cb`.
    void accessToken(const QString &resource, TokenCallback cb);

signals:
    void ready(bool hasSession);
    void deviceCodeReceived(const QString &userCode, const QString &verificationUrl, const QString &message);
    void signedIn();
    // `sessionLost` = the refresh token is no longer valid and a new sign-in is needed.
    void failed(const QString &message, bool sessionLost);

private:
    struct CachedToken { QString token; QDateTime expiresAt; };
    void pollDeviceCode();
    void storeRefreshToken(const QString &token);
    QNetworkReply *postForm(const QString &path, const QList<QPair<QString, QString>> &fields);

    QNetworkAccessManager m_nam;
    std::unique_ptr<TokenStore> m_store;
    QString m_authority;
    QString m_refreshToken;
    QHash<QString, CachedToken> m_cache;
    QHash<QString, QList<TokenCallback>> m_pending;
    QString m_deviceCode;
    QTimer m_pollTimer;
    QDateTime m_deviceCodeExpiresAt;
};
