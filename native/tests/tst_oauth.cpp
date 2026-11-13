// Microsoft sign-in (src/oauth.cpp): device code flow, token refresh and caching, session loss.
// A fake login server on loopback and a token store in memory; the system keychain is never used.
#include "fakeserver.h"
#include "oauth.h"

#include <QSignalSpy>
#include <QtTest>

namespace {
const QString kSpaces = QStringLiteral("https://api.spaces.skype.com");

class MemoryStore : public TokenStore
{
public:
    QString stored;
    QStringList writes;
    int removed = 0;
    QString writeError;
    void read(std::function<void(const QString &)> done) override { done(stored); }
    void write(const QString &token, std::function<void(const QString &)> done) override
    {
        writes << token;
        stored = token;
        done(writeError);
    }
    void remove() override { ++removed; stored.clear(); }
};
}

class OAuthTest : public QObject
{
    Q_OBJECT
    FakeServer server;
    MemoryStore *store = nullptr; // owned by oauth
    std::unique_ptr<OAuth> oauth;

    void signedInWith(const QString &refreshToken)
    {
        store->stored = refreshToken;
        QSignalSpy ready(oauth.get(), &OAuth::ready);
        oauth->loadStoredSession();
        QCOMPARE(ready.count(), 1);
        QVERIFY(ready[0].at(0).toBool());
    }

private slots:
    void initTestCase() { QVERIFY(server.start()); }

    void init()
    {
        server.reset();
        oauth = std::make_unique<OAuth>();
        auto memory = std::make_unique<MemoryStore>();
        store = memory.get();
        oauth->setTokenStore(std::move(memory));
        oauth->setAuthority(server.base());
    }

    // --- stored session -------------------------------------------------------------------

    void storedSessionIsReported()
    {
        QSignalSpy ready(oauth.get(), &OAuth::ready);
        oauth->loadStoredSession();
        QCOMPARE(ready.count(), 1);
        QVERIFY(!ready[0].at(0).toBool()); // nothing stored yet
        signedInWith("refresh-1");
    }

    // --- device code ----------------------------------------------------------------------

    void deviceCodeIsRequestedAndShown()
    {
        server.answer = R"({"device_code":"dc","user_code":"ABCD-1234","verification_url":"https://microsoft.com/devicelogin","message":"Go there","interval":"5","expires_in":"900"})";
        QSignalSpy shown(oauth.get(), &OAuth::deviceCodeReceived);
        oauth->startDeviceLogin();
        QVERIFY(shown.wait(5000));
        const Request &r = server.requests.first();
        QCOMPARE(r.method, QByteArray("POST"));
        QCOMPARE(r.path(), QStringLiteral("/oauth2/devicecode"));
        QCOMPARE(r.headers.value("content-type"), QByteArray("application/x-www-form-urlencoded"));
        QCOMPARE(r.form("client_id"), QStringLiteral("1fec8e78-bce4-4aaf-ab1b-5451cc387264"));
        QCOMPARE(r.form("resource"), kSpaces);
        QCOMPARE(shown[0].at(0).toString(), QStringLiteral("ABCD-1234"));
        QCOMPARE(shown[0].at(1).toString(), QStringLiteral("https://microsoft.com/devicelogin"));
        QCOMPARE(shown[0].at(2).toString(), QStringLiteral("Go there"));
        oauth->cancelDeviceLogin();
    }

    void verificationUriIsAcceptedToo()
    {
        server.answer = R"({"device_code":"dc","user_code":"X","verification_uri":"https://microsoft.com/other","interval":5,"expires_in":900})";
        QSignalSpy shown(oauth.get(), &OAuth::deviceCodeReceived);
        oauth->startDeviceLogin();
        QVERIFY(shown.wait(5000));
        QCOMPARE(shown[0].at(1).toString(), QStringLiteral("https://microsoft.com/other"));
        oauth->cancelDeviceLogin();
    }

    void aRefusedDeviceCodeIsAFailure()
    {
        server.status = 400;
        server.answer = R"({"error":"invalid_client","error_description":"unknown client"})";
        QSignalSpy failed(oauth.get(), &OAuth::failed);
        oauth->startDeviceLogin();
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed[0].at(0).toString(), QStringLiteral("Sign-in not started: unknown client"));
        QCOMPARE(failed[0].at(1).toBool(), false);
    }

    void signInCompletesAfterThePendingAnswers()
    {
        int polls = 0;
        server.respond = [&](const Request &r) -> QByteArray {
            if (r.path() == QStringLiteral("/oauth2/devicecode"))
                return R"({"device_code":"dc","user_code":"X","verification_url":"https://microsoft.com/devicelogin","interval":1,"expires_in":900})";
            return ++polls < 2 ? QByteArray(R"({"error":"authorization_pending"})")
                               : QByteArray(R"({"access_token":"at-1","refresh_token":"rt-1","expires_in":"3600"})");
        };
        QSignalSpy signedIn(oauth.get(), &OAuth::signedIn);
        QSignalSpy failed(oauth.get(), &OAuth::failed);
        oauth->startDeviceLogin();
        QVERIFY(signedIn.wait(8000));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(polls, 2);
        const Request &poll = server.requests.last();
        QCOMPARE(poll.path(), QStringLiteral("/oauth2/token"));
        QCOMPARE(poll.form("grant_type"), QStringLiteral("urn:ietf:params:oauth:grant-type:device_code"));
        QCOMPARE(poll.form("code"), QStringLiteral("dc"));
        QCOMPARE(store->writes, QStringList{QStringLiteral("rt-1")});

        // The first token is for the login resource: kept, so no new request is needed.
        const qsizetype before = server.requests.size();
        QString got;
        oauth->accessToken(kSpaces, [&](const QString &t) { got = t; });
        QCOMPARE(got, QStringLiteral("at-1"));
        QCOMPARE(server.requests.size(), before);
    }

    void aFinalErrorEndsTheSignIn()
    {
        server.respond = [](const Request &r) -> QByteArray {
            if (r.path() == QStringLiteral("/oauth2/devicecode"))
                return R"({"device_code":"dc","user_code":"X","verification_url":"u","interval":1,"expires_in":900})";
            return R"({"error":"authorization_declined","error_description":"user said no"})";
        };
        QSignalSpy signedIn(oauth.get(), &OAuth::signedIn);
        QSignalSpy failed(oauth.get(), &OAuth::failed);
        oauth->startDeviceLogin();
        QVERIFY(failed.wait(8000));
        QCOMPARE(failed[0].at(0).toString(), QStringLiteral("Sign-in failed: user said no"));
        QCOMPARE(signedIn.count(), 0);
        QVERIFY(store->writes.isEmpty());
    }

    void anExpiredCodeStopsThePolling()
    {
        server.answer = R"({"device_code":"dc","user_code":"X","verification_url":"u","interval":1,"expires_in":0})";
        QSignalSpy failed(oauth.get(), &OAuth::failed);
        oauth->startDeviceLogin();
        QVERIFY(failed.wait(8000));
        QCOMPARE(failed[0].at(0).toString(), QStringLiteral("The code has expired: try again."));
        QCOMPARE(server.requests.size(), 1); // no token request was made
    }

    void cancellingStopsThePolling()
    {
        server.answer = R"({"device_code":"dc","user_code":"X","verification_url":"u","interval":1,"expires_in":900})";
        QSignalSpy shown(oauth.get(), &OAuth::deviceCodeReceived);
        oauth->startDeviceLogin();
        QVERIFY(shown.wait(5000));
        oauth->cancelDeviceLogin();
        QTest::qWait(1500);
        QCOMPARE(server.requests.size(), 1);
    }

    // --- access tokens --------------------------------------------------------------------

    void accessTokenIsRefreshedAndCached()
    {
        signedInWith("rt-old");
        server.answer = R"({"access_token":"at-2","refresh_token":"rt-new","expires_in":"3600"})";
        QString got;
        oauth->accessToken("https://presence.teams.microsoft.com", [&](const QString &t) { got = t; });
        QVERIFY(server.waitForRequests(1));
        QTRY_COMPARE(got, QStringLiteral("at-2"));
        const Request &r = server.requests.first();
        QCOMPARE(r.path(), QStringLiteral("/oauth2/v2.0/token"));
        QCOMPARE(r.form("grant_type"), QStringLiteral("refresh_token"));
        QCOMPARE(r.form("refresh_token"), QStringLiteral("rt-old"));
        QCOMPARE(r.form("scope"), QStringLiteral("https://presence.teams.microsoft.com/.default openid profile offline_access"));
        QCOMPARE(r.form("client_id"), QStringLiteral("1fec8e78-bce4-4aaf-ab1b-5451cc387264"));
        QCOMPARE(store->writes, QStringList{QStringLiteral("rt-new")}); // the rotated refresh token is kept

        got.clear();
        oauth->accessToken("https://presence.teams.microsoft.com", [&](const QString &t) { got = t; });
        QCOMPARE(got, QStringLiteral("at-2")); // from the cache
        QCOMPARE(server.requests.size(), 1);
    }

    void callersAskingTogetherShareOneRequest()
    {
        signedInWith("rt");
        server.answer = R"({"access_token":"shared","expires_in":3600})";
        QStringList got;
        for (int i = 0; i < 3; ++i) oauth->accessToken(kSpaces, [&](const QString &t) { got << t; });
        QTRY_COMPARE(got.size(), 3);
        QCOMPARE(got, (QStringList{"shared", "shared", "shared"}));
        QCOMPARE(server.requests.size(), 1);
        QVERIFY(store->writes.isEmpty()); // no refresh token in the answer: nothing to store
    }

    void aTokenAboutToExpireIsNotReused()
    {
        signedInWith("rt");
        server.answer = R"({"access_token":"short","expires_in":100})"; // inside the renewal margin
        QString got;
        oauth->accessToken(kSpaces, [&](const QString &t) { got = t; });
        QTRY_COMPARE(got, QStringLiteral("short"));
        oauth->accessToken(kSpaces, [&](const QString &t) { got = t; });
        QTRY_COMPARE(server.requests.size(), 2);
    }

    void aSameRefreshTokenIsNotWrittenAgain()
    {
        signedInWith("rt");
        server.answer = R"({"access_token":"a","refresh_token":"rt","expires_in":3600})";
        QString got;
        oauth->accessToken(kSpaces, [&](const QString &t) { got = t; });
        QTRY_VERIFY(!got.isEmpty());
        QVERIFY(store->writes.isEmpty());
    }

    // --- losing the session ---------------------------------------------------------------

    void withoutASessionThereIsNothingToRefresh()
    {
        QSignalSpy failed(oauth.get(), &OAuth::failed);
        bool called = false;
        oauth->accessToken(kSpaces, [&](const QString &) { called = true; });
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed[0].at(0).toString(), QStringLiteral("No session: sign in again."));
        QCOMPARE(failed[0].at(1).toBool(), true);
        QVERIFY(!called);
        QVERIFY(server.requests.isEmpty());
    }

    void aRevokedSessionSignsOut()
    {
        signedInWith("rt");
        server.status = 400;
        server.answer = R"({"error":"invalid_grant","error_description":"AADSTS700082: expired\nTrace ID: 1"})";
        QSignalSpy failed(oauth.get(), &OAuth::failed);
        bool called = false;
        oauth->accessToken("https://presence.teams.microsoft.com", [&](const QString &) { called = true; });
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed[0].at(0).toString(), QStringLiteral("Token for https://presence.teams.microsoft.com not obtained: AADSTS700082: expired"));
        QCOMPARE(failed[0].at(1).toBool(), true); // session lost
        QVERIFY(!called);
        QCOMPARE(store->removed, 1);
        // And the next call does not even try.
        const qsizetype before = server.requests.size();
        oauth->accessToken(kSpaces, [](const QString &) {});
        QCOMPARE(server.requests.size(), before);
    }

    void otherErrorsKeepTheSession()
    {
        signedInWith("rt");
        server.status = 500;
        server.answer = R"({"error":"temporarily_unavailable","error_description":"try later"})";
        QSignalSpy failed(oauth.get(), &OAuth::failed);
        oauth->accessToken(kSpaces, [](const QString &) {});
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed[0].at(1).toBool(), false);
        QCOMPARE(store->removed, 0);
        QCOMPARE(store->stored, QStringLiteral("rt"));
    }

    void signOutForgetsEverything()
    {
        signedInWith("rt");
        server.answer = R"({"access_token":"a","expires_in":3600})";
        QString got;
        oauth->accessToken(kSpaces, [&](const QString &t) { got = t; });
        QTRY_VERIFY(!got.isEmpty());
        oauth->signOut();
        QCOMPARE(store->removed, 1);
        QSignalSpy failed(oauth.get(), &OAuth::failed);
        oauth->accessToken(kSpaces, [](const QString &) {}); // the cached token is gone too
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed[0].at(1).toBool(), true);
    }

    void aStoreThatCannotWriteIsReported()
    {
        signedInWith("rt");
        store->writeError = "locked wallet";
        server.answer = R"({"access_token":"a","refresh_token":"rt-2","expires_in":3600})";
        QSignalSpy failed(oauth.get(), &OAuth::failed);
        oauth->accessToken(kSpaces, [](const QString &) {});
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed[0].at(0).toString(), QStringLiteral("Cannot save the session in the keychain: locked wallet"));
        QCOMPARE(failed[0].at(1).toBool(), false);
    }
};

QTEST_GUILESS_MAIN(OAuthTest)
#include "tst_oauth.moc"
