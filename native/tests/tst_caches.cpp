// Picture caches on disk (src/mediacache.cpp, src/avatarcache.cpp). Nothing here touches the
// network: the cases are the ones answered from disk or skipped before a request is made.
#include "avatarcache.h"
#include "mediacache.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QString sha1(const QString &s)
{
    return QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1).toHex());
}
QString cacheDir(const QString &sub) { return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QLatin1Char('/') + sub; }
void write(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}
QString fileUrl(const QString &path) { return QUrl::fromLocalFile(path).toString(); }
}

class CachesTest : public QObject
{
    Q_OBJECT
private slots:
    // --- which pictures get the session token ---------------------------------------------

    void isProtectedImage_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("isProtected");
        QTest::newRow("regional media host") << "https://eu-api.asm.skype.com/v1/objects/0-x/views/imgo" << true;
        QTest::newRow("global media host") << "https://api.asm.skype.com/v1/objects/0-x/views/imgt1" << true;
        QTest::newRow("asyncgw host") << "https://eu.asyncgw.teams.microsoft.com/v1/objects/0-x/views/imgpsh_fullsize" << true;
        QTest::newRow("plain http") << "http://eu-api.asm.skype.com/v1/objects/0-x/views/imgo" << false;
        QTest::newRow("credentials in the URL") << "https://user:pw@eu-api.asm.skype.com/v1/objects/0-x/views/imgo" << false;
        QTest::newRow("other host") << "https://example.com/v1/objects/0-x/views/imgo" << false;
        QTest::newRow("media host as a prefix of another") << "https://eu-api.asm.skype.com.example.com/v1/objects/0-x/views/imgo" << false;
        QTest::newRow("look-alike host") << "https://notapi.asm.skype.com/v1/objects/0-x/views/imgo" << false;
        QTest::newRow("not an image view") << "https://eu-api.asm.skype.com/v1/objects/0-x/views/video" << false;
        QTest::newRow("empty") << "" << false;
    }
    void isProtectedImage()
    {
        QFETCH(QString, url);
        QFETCH(bool, isProtected);
        QCOMPARE(MediaCache::isProtectedImage(QUrl(url)), isProtected);
    }

    // --- media cache ----------------------------------------------------------------------

    void mediaFileOnDiskIsServedWithoutDownloading()
    {
        const QString remote = QStringLiteral("https://example.com/pic.png");
        const QString path = cacheDir("media") + QLatin1Char('/') + sha1(remote);
        write(path, "png-bytes");
        MediaCache cache;
        QCOMPARE(cache.urlFor(remote), fileUrl(path));
        QCOMPARE(cache.urlFor(remote), fileUrl(path)); // remembered
        QCOMPARE(cache.urlFor(QString()), QString());
    }

    void emptyFilesAndSkippedRequestsGiveNoUrl()
    {
        MediaCache cache;
        QSignalSpy ready(&cache, &MediaCache::mediaReady);
        // An empty file on disk is a failed download, not a picture. The URL is plain http,
        // which the cache never fetches, so the case stays offline.
        const QString plainHttp = QStringLiteral("http://example.com/x.png");
        write(cacheDir("media") + QLatin1Char('/') + sha1(plainHttp), QByteArray());
        QCOMPARE(cache.urlFor(plainHttp), QString());
        // A media-service picture without a session token is not requested either.
        QCOMPARE(cache.urlFor(QStringLiteral("https://eu-api.asm.skype.com/v1/objects/0-x/views/imgo")), QString());
        QCOMPARE(ready.count(), 0);
    }

    void clearMakesTheCacheReadTheDiskAgain()
    {
        const QString remote = QStringLiteral("https://example.com/later.png");
        const QString path = cacheDir("media") + QLatin1Char('/') + sha1(remote);
        QFile::remove(path);
        MediaCache cache;
        cache.setSkypeToken("unused");
        // Not on disk yet and not fetchable here: use a scheme the cache skips, same code path as "pending".
        const QString skipped = QStringLiteral("http://example.com/later.png");
        QCOMPARE(cache.urlFor(skipped), QString());
        write(cacheDir("media") + QLatin1Char('/') + sha1(skipped), "now there");
        QCOMPARE(cache.urlFor(skipped), QString()); // still "pending" in memory
        cache.clear();
        QCOMPARE(cache.urlFor(skipped), fileUrl(cacheDir("media") + QLatin1Char('/') + sha1(skipped)));
    }

    // --- avatar cache ---------------------------------------------------------------------

    void freshAvatarPictureIsServed()
    {
        const QString mri = QStringLiteral("8:orgid:fresh");
        const QString path = cacheDir("avatars") + QLatin1Char('/') + sha1(mri) + QStringLiteral(".img");
        write(path, "jpeg");
        AvatarCache cache;
        QCOMPARE(cache.urlFor(mri), fileUrl(path));
        QCOMPARE(cache.urlFor(QString()), QString());
    }

    void freshMarkerMeansThePersonHasNoPicture()
    {
        const QString mri = QStringLiteral("8:orgid:nopic");
        write(cacheDir("avatars") + QLatin1Char('/') + sha1(mri) + QStringLiteral(".none"), QByteArray());
        AvatarCache cache;
        QCOMPARE(cache.urlFor(mri), QString());
    }

    void stalePictureIsShownWhileItWouldBeRefreshed()
    {
        const QString mri = QStringLiteral("8:orgid:stale");
        const QString path = cacheDir("avatars") + QLatin1Char('/') + sha1(mri) + QStringLiteral(".img");
        write(path, "old jpeg");
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.setFileTime(QDateTime::currentDateTime().addDays(-30), QFileDevice::FileModificationTime));
        f.close();
        AvatarCache cache; // not configured: the refresh stays queued, no request
        QSignalSpy ready(&cache, &AvatarCache::avatarReady);
        QCOMPARE(cache.urlFor(mri), fileUrl(path));
        QCOMPARE(ready.count(), 0);
    }

    void unknownPersonHasNoPictureYet()
    {
        AvatarCache cache;
        QCOMPARE(cache.urlFor(QStringLiteral("8:orgid:never-seen")), QString());
    }

    void clearForgetsWhatWasLearned()
    {
        const QString mri = QStringLiteral("8:orgid:forgotten");
        AvatarCache cache;
        QCOMPARE(cache.urlFor(mri), QString());
        const QString path = cacheDir("avatars") + QLatin1Char('/') + sha1(mri) + QStringLiteral(".img");
        write(path, "jpeg");
        QCOMPARE(cache.urlFor(mri), QString()); // remembered as "none yet"
        cache.clear();
        QCOMPARE(cache.urlFor(mri), fileUrl(path));
    }
};

int main(int argc, char **argv)
{
    // The caches live under XDG_CACHE_HOME: a temporary one, so the real cache is never touched.
    static QTemporaryDir cacheHome;
    qputenv("XDG_CACHE_HOME", cacheHome.path().toUtf8());
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("kteams-test"));
    CachesTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_caches.moc"
