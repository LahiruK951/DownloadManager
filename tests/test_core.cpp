#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QSysInfo>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <curl/curl.h>

#include "RangeServer.h"
#include "HlsServer.h"
#include "core/Aes128.h"
#include "core/FileMover.h"
#include "engines/hls/HlsEngine.h"
#include "engines/dash/DashEngine.h"
#include "engines/dash/Mpd.h"
#include "engines/sites/SiteEngine.h"
#include "engines/www/FileWriter.h"
#include "core/DiskSpace.h"
#include "core/InstanceServer.h"
#include "core/Secret.h"
#include "core/Checksum.h"
#include "core/ReleaseSignature.h"
#include "core/Metalink.h"
#include <QLocalSocket>
#include "engines/hls/M3u8.h"
#include "core/BatchLinks.h"
#include "core/DownloadManager.h"
#include "core/Instance.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include "core/Database.h"
#include "core/FileNamer.h"
#include "core/LinkCheck.h"
#include "core/SleepGuard.h"
#include "core/MediaConvert.h"
#include "core/ProxySettings.h"
#include "core/TorrentFolderWatcher.h"
#include "core/BrowserHost.h"
#include "core/AppLog.h"
#include "core/RemoteServer.h"
#include "core/Updater.h"
#include "core/AttachmentCheck.h"
#include "core/FdmImport.h"
#include "core/MeteredGuard.h"
#include <QStandardPaths>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include "ui/DownloadsModel.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QScopeGuard>
#include "core/Schedule.h"
#include "engines/www/WwwEngine.h"

#ifdef FLUXDM_HAS_TORRENT
#include "engines/bt/BtEngine.h"
#include <libtorrent/alert_types.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_info.hpp>
#endif

namespace {
QByteArray makeData(qint64 size)
{
    QByteArray d(size, Qt::Uninitialized);
    for (qint64 i = 0; i < size; ++i)
        d[i] = char((i * 131 + (i >> 11)) & 0xff);
    return d;
}

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

struct Outcome {
    bool finished = false;
    bool failed = false;
    QString path;
    QString error;
};

Outcome waitForEnd(QSignalSpy& fin, QSignalSpy& fail, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (fin.isEmpty() && fail.isEmpty() && t.elapsed() < timeoutMs)
        QTest::qWait(20);
    Outcome o;
    if (!fin.isEmpty()) {
        o.finished = true;
        o.path = fin.last().at(1).toString();
    }
    if (!fail.isEmpty()) {
        o.failed = true;
        o.error = fail.last().at(1).toString();
    }
    return o;
}

TaskParams params(qint64 id, const QString& url, const QString& dir, int conns = 4)
{
    TaskParams p;
    p.id = id;
    p.url = url;
    p.destDir = dir;
    p.userAgent = QStringLiteral("FluxDM-test");
    p.maxConnections = conns;
    return p;
}
    // Serves master -> media playlist with `count` AES-128 segments (implicit IVs from sequence 10).
QByteArray setupHls(HlsServer& srv, int count, QList<QByteArray>* plainOut)
    {
        const QByteArray key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
        srv.files.insert("/key.bin", key);
        QByteArray media = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:10\n"
                           "#EXT-X-KEY:METHOD=AES-128,URI=\"/key.bin\"\n";
        QByteArray all;
        for (int i = 0; i < count; ++i) {
            QByteArray plain = makeData(150000 + i * 977);
            plain[0] = char(0x47); // TS sync byte, for realism
            plain[1] = char(i);
            *plainOut << plain;
            all += plain;
            srv.files.insert(QStringLiteral("/show/hi/seg%1.ts").arg(i), Aes128::encrypt(key, M3u8::sequenceIv(10 + i), plain));
            media += "#EXTINF:4.0,\nseg" + QByteArray::number(i) + ".ts\n";
        }
        media += "#EXT-X-ENDLIST\n";
        srv.files.insert("/show/hi/index.m3u8", media);
        srv.files.insert("/show/master.m3u8",
                         "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=300000\nlo/index.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=3000000,RESOLUTION=1280x720\nhi/index.m3u8\n");
        return all;
    }

} // namespace

class TestCore : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    void cleanupTestCase() { curl_global_cleanup(); }

    void contentDisposition()
    {
        QCOMPARE(FileNamer::fromContentDisposition("attachment; filename=\"a b.zip\""), QStringLiteral("a b.zip"));
        QCOMPARE(FileNamer::fromContentDisposition("inline; filename=report.pdf"), QStringLiteral("report.pdf"));
        QCOMPARE(FileNamer::fromContentDisposition("attachment; filename=\"x.bin\"; filename*=UTF-8''%E2%82%AC%20rates.pdf"),
                 QString::fromUtf8("\xE2\x82\xAC rates.pdf"));
        QCOMPARE(FileNamer::fromContentDisposition(""), QString());
        QCOMPARE(FileNamer::fromContentDisposition("attachment; filename=\"SITE.COM+-+[abc]+Clip+(720).mp4\""),
                 QStringLiteral("SITE.COM - [abc] Clip (720).mp4"));
        QCOMPARE(FileNamer::fromContentDisposition("attachment; filename=\"a+b.txt\""), QStringLiteral("a+b.txt"));
        QCOMPARE(FileNamer::fromContentDisposition("attachment; filename=\"C++Primer+5th+ed.pdf\""), QStringLiteral("C++Primer+5th+ed.pdf"));
    }

    void namesAndCategories()
    {
        QCOMPARE(FileNamer::fromUrl(QUrl("https://x.com/dir/file%20name.iso?x=1")), QStringLiteral("file name.iso"));
        QCOMPARE(FileNamer::fromUrl(QUrl("https://x.com/")), QString());
        QCOMPARE(FileNamer::sanitize("a<b>:c?.txt"), QStringLiteral("a_b__c_.txt"));
        QCOMPARE(FileNamer::sanitize("CON.txt"), QStringLiteral("_CON.txt"));
        QCOMPARE(FileNamer::sanitize("  .. "), QStringLiteral("download"));
        QCOMPARE(FileNamer::category("movie.MKV"), QStringLiteral("Video"));
        QCOMPARE(FileNamer::category("setup.exe"), QStringLiteral("Programs"));
        QCOMPARE(FileNamer::category("noext"), QStringLiteral("Other"));
    }

    static QVariantMap fileEntry(const QString& name, qint64 size)
    {
        return {{QStringLiteral("name"), name}, {QStringLiteral("size"), size}};
    }

    void multiFileCategory()
    {
        // A movie torrent: the video dominates the subtitles and the poster.
        const QVariantList movie{fileEntry("Big Buck Bunny.en.srt", 140), fileEntry("Big Buck Bunny.mp4", 276134947),
                                 fileEntry("poster.jpg", 310380)};
        QCOMPARE(FileNamer::categoryOfFiles(movie), QStringLiteral("Video"));
        // An album with a large cover scan is still Music.
        const QVariantList album{fileEntry("01.flac", 30'000'000), fileEntry("02.flac", 28'000'000),
                                 fileEntry("cover.png", 20'000'000)};
        QCOMPARE(FileNamer::categoryOfFiles(album), QStringLiteral("Music"));
        QCOMPARE(FileNamer::categoryOfFiles({}), QString());

        // The Download picks it up from engine stats (multi-file only) and announces it.
        DownloadRecord r;
        r.moduleUid = QStringLiteral("downloadsbt");
        r.title = QStringLiteral("Big Buck Bunny"); // folder name: no extension
        r.state = Download::Downloading;
        Download d(r);
        QCOMPARE(d.category(), QStringLiteral("Other"));
        QSignalSpy spy(&d, &Download::categoryChanged);
        TaskStats st;
        st.id = r.id;
        st.files = movie;
        d.applyStats(st);
        QCOMPARE(d.category(), QStringLiteral("Video"));
        QCOMPARE(spy.count(), 1);
        d.applyStats(st); // unchanged: no repeat notification
        QCOMPARE(spy.count(), 1);
        st.files = QVariantList{fileEntry("only.iso", 100)}; // single file: the title decides
        d.applyStats(st);
        QCOMPARE(d.category(), QStringLiteral("Other"));
    }

    void renameFinishedDownload()
    {
        QTemporaryDir dir;
        const QString folder = QDir::toNativeSeparators(dir.path());
        auto touch = [&](const QString& name) {
            QFile f(QDir(dir.path()).filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("data");
        };
        touch(QStringLiteral("clip.mp4"));
        touch(QStringLiteral("taken.mp4"));
        {
            Database db;
            QVERIFY(db.open(dir.path()));
            DownloadRecord done;
            done.id = 1;
            done.url = QStringLiteral("https://example.com/clip.mp4");
            done.destDir = folder;
            done.title = QStringLiteral("clip.mp4");
            done.filePath = QDir(folder).filePath(done.title);
            done.state = Download::Completed;
            db.save(done);
            DownloadRecord running = done;
            running.id = 2;
            running.title = QStringLiteral("other.mp4");
            running.filePath = QDir(folder).filePath(running.title);
            running.state = Download::Paused;
            db.save(running);
            DownloadManager mgr(&db);
            mgr.load();
            Download* d = mgr.find(1);
            QVERIFY(d);
            QVERIFY(DownloadManager::canRename(d));
            QVERIFY(!DownloadManager::canRename(mgr.find(2))); // not finished
            QVERIFY(!mgr.rename(mgr.find(2), QStringLiteral("x.mp4")).isEmpty());

            QVERIFY(!mgr.rename(d, QStringLiteral("bad:name.mp4")).isEmpty()); // invalid characters
            QVERIFY(!mgr.rename(d, QStringLiteral("   ")).isEmpty());
            QVERIFY(!mgr.rename(d, QStringLiteral("taken.mp4")).isEmpty()); // never overwrite
            QVERIFY(QFile::exists(QDir(dir.path()).filePath("clip.mp4")));

            QCOMPARE(mgr.rename(d, QStringLiteral("  My Clip (720).mp4 ")), QString());
            QCOMPARE(d->title(), QStringLiteral("My Clip (720).mp4"));
            QVERIFY(QFile::exists(QDir(dir.path()).filePath("My Clip (720).mp4")));
            QVERIFY(!QFile::exists(QDir(dir.path()).filePath("clip.mp4")));
            QCOMPARE(QFileInfo(d->filePath()).fileName(), QStringLiteral("My Clip (720).mp4"));

            QCOMPARE(mgr.rename(d, QStringLiteral("MY CLIP (720).mp4")), QString()); // case-only change
            QCOMPARE(d->title(), QStringLiteral("MY CLIP (720).mp4"));
        }
        Database db; // the new name was saved
        QVERIFY(db.open(dir.path()));
        const QList<DownloadRecord> all = db.loadDownloads();
        const auto it = std::find_if(all.begin(), all.end(), [](const DownloadRecord& r) { return r.id == 1; });
        QVERIFY(it != all.end());
        QCOMPARE(it->title, QStringLiteral("MY CLIP (720).mp4"));
        QCOMPARE(QFileInfo(it->filePath).fileName(), QStringLiteral("MY CLIP (720).mp4"));
    }

    void contentCategoryPersists()
    {
        QTemporaryDir dir;
        {   // A database from before the column existed: migration must add it.
            QSqlDatabase old = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("old"));
            old.setDatabaseName(QDir(dir.path()).filePath(QStringLiteral("db.sqlite")));
            QVERIFY(old.open());
            QSqlQuery q(old);
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE downloads (id INTEGER PRIMARY KEY, moduleUid TEXT, url TEXT, referrer TEXT,"
                " cookies BLOB, userAgent TEXT, destinationPath TEXT, preferredFileTitle TEXT, title TEXT, filePath TEXT,"
                " creationTime INTEGER, finishedTime INTEGER, resumeData BLOB, lastResult TEXT, state INTEGER,"
                " totalBytes INTEGER, receivedBytes INTEGER, resumeSupport INTEGER, priority INTEGER, speedLimit INTEGER)")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO downloads (id, title, state) VALUES (1, 'old.zip', 5)")));
            old.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("old"));
        {
            Database db;
            QVERIFY(db.open(dir.path()));
            const QList<DownloadRecord> before = db.loadDownloads();
            QCOMPARE(before.size(), 1);
            QCOMPARE(before.first().contentCategory, QString());
            DownloadRecord r;
            r.id = 2;
            r.title = QStringLiteral("Some Show S01");
            r.contentCategory = QStringLiteral("Video");
            db.save(r);
        }
        Database db;
        QVERIFY(db.open(dir.path()));
        const QList<DownloadRecord> all = db.loadDownloads();
        QCOMPARE(all.size(), 2);
        QCOMPARE(all.first().id, qint64(2));
        QCOMPARE(all.first().contentCategory, QStringLiteral("Video"));
        QCOMPARE(Download(all.first()).category(), QStringLiteral("Video"));
        QCOMPARE(Download(all.last()).category(), QStringLiteral("Archive"));
    }

    void torrentFileDetection()
    {
        QTemporaryDir dir;
        auto write = [&](const char* name, const QByteArray& data) {
            QFile f(dir.filePath(name));
            f.open(QIODevice::WriteOnly);
            f.write(data);
            return dir.filePath(name);
        };
        QVERIFY(FileNamer::looksLikeTorrentFile(write("3C3B5CB1", "d8:announce35:udp://tracker.example:1337/announce4:infod6:lengthi1e4:name1:a12:piece lengthi16384eee")));
        QVERIFY(!FileNamer::looksLikeTorrentFile(write("page.html", "<!doctype html><html><body>not a torrent at all</body></html>")));
        QVERIFY(!FileNamer::looksLikeTorrentFile(write("data.bin", makeData(4096))));
    }

    void uniquePath()
    {
        QTemporaryDir dir;
        QFile f(dir.filePath("f.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        QFile p(dir.filePath("f (1).txt") + FileNamer::kPartSuffix);
        QVERIFY(p.open(QIODevice::WriteOnly));
        p.close();
        QCOMPARE(QFileInfo(FileNamer::uniquePath(dir.path(), "f.txt")).fileName(), QStringLiteral("f (2).txt"));
        QCOMPARE(QFileInfo(FileNamer::uniquePath(dir.path(), "g.txt")).fileName(), QStringLiteral("g.txt"));
    }

    void scheduleWindows()
    {
        const QDate mon(2026, 11, 16); // a Monday
        QCOMPARE(mon.dayOfWeek(), 1);
        auto at = [&](int dayOffset, int h, int m) { return QDateTime(mon.addDays(dayOffset), QTime(h, m)); };

        ScheduleWindow office{0x1F, 9 * 60, 18 * 60}; // weekdays 09:00-18:00
        QVERIFY(office.contains(at(0, 9, 0)));
        QVERIFY(office.contains(at(4, 17, 59)));
        QVERIFY(!office.contains(at(0, 18, 0)));
        QVERIFY(!office.contains(at(5, 12, 0))); // Saturday

        ScheduleWindow night{0x7F, 23 * 60, 7 * 60}; // crosses midnight
        QVERIFY(night.contains(at(0, 23, 30)));
        QVERIFY(night.contains(at(1, 6, 59)));
        QVERIFY(!night.contains(at(1, 7, 0)));

        ScheduleWindow fridayNight{1 << 4, 22 * 60, 2 * 60}; // Fri 22:00 - Sat 02:00
        QVERIFY(fridayNight.contains(at(5, 1, 0)));   // Saturday 01:00 belongs to Friday's window
        QVERIFY(!fridayNight.contains(at(5, 23, 0))); // Saturday night is not Friday

        ScheduleWindow allDay{1 << 6, 0, 0}; // Sundays
        QVERIFY(allDay.contains(at(6, 0, 0)));
        QVERIFY(!allDay.contains(at(0, 12, 0)));
        QCOMPARE(office.text(), QStringLiteral("Weekdays 09:00–18:00"));

        const TrafficRule r = TrafficRule::fromMap(TrafficRule{true, office, 0}.toMap());
        QCOMPARE(r.window.days, 0x1F);
        QCOMPARE(r.window.fromMin, 540);
        QCOMPARE(r.mode, 0);
    }

    void retryClassification()
    {
        QVERIFY(DownloadManager::isRetriable("Couldn't connect to server"));
        QVERIFY(DownloadManager::isRetriable("HTTP error 503"));
        QVERIFY(DownloadManager::isRetriable("Timeout was reached"));
        QVERIFY(DownloadManager::isRetriable("HTTP error 408"));
        QVERIFY(DownloadManager::isRetriable("HTTP error 429"));
        QVERIFY(!DownloadManager::isRetriable("HTTP error 404"));
        QVERIFY(!DownloadManager::isRetriable("HTTP error 403"));
        QVERIFY(!DownloadManager::isRetriable("Disk write error: not enough space"));
        QVERIFY(!DownloadManager::isRetriable("This torrent is already being downloaded"));
        QVERIFY(!DownloadManager::isRetriable(""));
    }

    void batchLinks()
    {
        QCOMPARE(BatchLinks::expand("http://x/a[1-3].jpg"),
                 (QStringList{"http://x/a1.jpg", "http://x/a2.jpg", "http://x/a3.jpg"}));
        QCOMPARE(BatchLinks::expand("http://x/p[08-10].bin"),
                 (QStringList{"http://x/p08.bin", "http://x/p09.bin", "http://x/p10.bin"}));
        QCOMPARE(BatchLinks::expand("http://x/[c-a].txt"), (QStringList{"http://x/c.txt", "http://x/b.txt", "http://x/a.txt"}));
        QCOMPARE(BatchLinks::expand("http://x/plain.zip"), QStringList{"http://x/plain.zip"});
        QCOMPARE(BatchLinks::expand("http://x/[1-5000]", 100).size(), 100);
    }

    void segmentedDownload()
    {
        RangeServer srv;
        srv.data = makeData(12 * 1024 * 1024);
        QVERIFY(srv.start());
        QTemporaryDir dir;
        WwwEngine eng;
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        eng.start(params(1, srv.url("/data/big.bin"), dir.path(), 4));
        const Outcome o = waitForEnd(fin, fail, 60000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QCOMPARE(QFileInfo(o.path).fileName(), QStringLiteral("big.bin"));
        QVERIFY(readAll(o.path) == srv.data);
        QVERIFY(!QFile::exists(o.path + FileNamer::kPartSuffix));
        QVERIFY2(srv.rangedRequests >= 4, qPrintable(QString::number(srv.rangedRequests)));
    }

    void resumeAfterStop()
    {
        RangeServer srv;
        srv.data = makeData(16 * 1024 * 1024);
        srv.chunk = 16 * 1024;
        srv.intervalMs = 5;
        QVERIFY(srv.start());
        QTemporaryDir dir;
        WwwEngine eng;
        QSignalSpy stats(&eng, &DownloadEngine::stats), stopped(&eng, &DownloadEngine::stopped);
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);

        eng.start(params(7, srv.url("/r.bin"), dir.path(), 4));
        QElapsedTimer t;
        t.start();
        qint64 got = 0;
        while (got < srv.data.size() / 4 && t.elapsed() < 30000 && fin.isEmpty()) {
            QTest::qWait(20);
            if (!stats.isEmpty())
                got = stats.last().at(0).value<TaskStats>().received;
        }
        QVERIFY2(fin.isEmpty(), "finished before we could stop it; slow the server down");
        eng.stop(7);
        QTRY_VERIFY_WITH_TIMEOUT(!stopped.isEmpty(), 5000);
        const QByteArray resume = stopped.last().at(1).toByteArray();
        QVERIFY(!resume.isEmpty());
        QVERIFY(QFile::exists(dir.filePath("r.bin") + FileNamer::kPartSuffix));

        srv.rangeStarts.clear();
        TaskParams p = params(7, srv.url("/r.bin"), dir.path(), 4);
        p.resumeData = resume;
        eng.start(p);
        const Outcome o = waitForEnd(fin, fail, 60000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QCOMPARE(QFileInfo(o.path).fileName(), QStringLiteral("r.bin")); // resumed into the same file
        QVERIFY(readAll(o.path) == srv.data);
        QVERIFY(!srv.rangeStarts.isEmpty());
        QVERIFY2(!srv.rangeStarts.contains(0), "resume restarted from byte 0");
    }

    void noRangeSupportFallsBackToSingleConnection()
    {
        RangeServer srv;
        srv.data = makeData(3 * 1024 * 1024);
        srv.rangeSupport = false;
        QVERIFY(srv.start());
        QTemporaryDir dir;
        WwwEngine eng;
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        eng.start(params(2, srv.url("/plain.dat"), dir.path(), 8));
        const Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QVERIFY(readAll(o.path) == srv.data);
        QCOMPARE(srv.requests, 1);
    }

    void contentDispositionName()
    {
        RangeServer srv;
        srv.data = makeData(200 * 1024);
        srv.contentDisposition = "attachment; filename=\"report final.bin\"";
        QVERIFY(srv.start());
        QTemporaryDir dir;
        WwwEngine eng;
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        eng.start(params(3, srv.url("/download?id=42"), dir.path()));
        const Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QCOMPARE(QFileInfo(o.path).fileName(), QStringLiteral("report final.bin"));
    }

    void httpErrorFails()
    {
        RangeServer srv;
        srv.data = makeData(1024);
        srv.failStatus = 404;
        QVERIFY(srv.start());
        QTemporaryDir dir;
        WwwEngine eng;
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        eng.start(params(4, srv.url("/missing.bin"), dir.path()));
        const Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY(o.failed);
        QVERIFY2(o.error.contains("404"), qPrintable(o.error));
    }

    void linkCheckRules()
    {
        using LinkCheck::isUnexpectedWebPage;
        const QStringList zip{QString(), QStringLiteral("game.zip")};
        QVERIFY(isUnexpectedWebPage("text/html; charset=UTF-8", {}, zip));
        QVERIFY(isUnexpectedWebPage("application/xhtml+xml", {}, {QStringLiteral("clip.mp4")}));
        QVERIFY(!isUnexpectedWebPage("application/zip", {}, zip));                     // the real file
        QVERIFY(!isUnexpectedWebPage("text/html", "attachment; filename=\"a.zip\"", zip)); // offered as a file
        QVERIFY(!isUnexpectedWebPage("text/html", {}, {QStringLiteral("index.html"), QStringLiteral("view.php")}));
        QVERIFY(!isUnexpectedWebPage("text/html", {}, {QStringLiteral("download"), QString()})); // no extension
        QVERIFY(!isUnexpectedWebPage("text/plain", {}, zip));

        const QDateTime now = QDateTime::fromSecsSinceEpoch(1'795'200'000);
        const QUrl expired(QStringLiteral("https://cdn.example.com/f.zip?key=x&expire=1795113261&filename=f.zip"));
        const QUrl valid(QStringLiteral("https://cdn.example.com/f.zip?Expires=1795300000"));
        QCOMPARE(LinkCheck::linkExpiry(expired), QDateTime::fromSecsSinceEpoch(1795113261));
        QVERIFY(!LinkCheck::linkExpiry(QUrl(QStringLiteral("https://x.com/f.zip?expires=3600"))).isValid()); // a duration
        QVERIFY(LinkCheck::httpErrorText(403, expired, now).contains(QStringLiteral("expired on")));
        QVERIFY(!LinkCheck::httpErrorText(403, valid, now).contains(QStringLiteral("expired on")));
        QVERIFY(LinkCheck::httpErrorText(403, valid, now).startsWith(QStringLiteral("HTTP error 403: access denied")));
        QVERIFY(LinkCheck::webPageText(expired, now).contains(QStringLiteral("expired on")));
        // The friendlier texts keep the auto-retry classification.
        QVERIFY(!DownloadManager::isRetriable(LinkCheck::httpErrorText(403, expired, now)));
        QVERIFY(!DownloadManager::isRetriable(LinkCheck::httpErrorText(410, valid, now)));
        QVERIFY(DownloadManager::isRetriable(LinkCheck::httpErrorText(503, valid, now)));
        QVERIFY(DownloadManager::isRetriable(LinkCheck::httpErrorText(429, valid, now)));
        QVERIFY(!DownloadManager::isRetriable(LinkCheck::webPageText(valid, now)));
    }

    void webPageInsteadOfFileFails()
    {
        RangeServer srv;
        srv.data = QByteArray("<!doctype html><title>Download expired</title><p>Please go back.</p>").repeated(50);
        srv.contentType = "text/html; charset=utf-8";
        QVERIFY(srv.start());
        QTemporaryDir dir;
        WwwEngine eng;
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        eng.start(params(5, srv.url("/1/application/zip/abc.zip?key=k&expire=1700000000"), dir.path()));
        const Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY(o.failed);
        QVERIFY2(o.error.contains("web page instead of the file"), qPrintable(o.error));
        QVERIFY2(o.error.contains("expired on"), qPrintable(o.error));
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty()); // no broken "abc.zip" left behind
        QCOMPARE(srv.requests, 1); // not retried
    }

    void webPageWhenAskedDownloads()
    {
        RangeServer srv;
        srv.data = QByteArray("<!doctype html><title>Docs</title>").repeated(100);
        srv.contentType = "text/html";
        QVERIFY(srv.start());
        QTemporaryDir dir;
        WwwEngine eng;
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        eng.start(params(6, srv.url("/manual/index.html"), dir.path()));
        const Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QVERIFY(readAll(o.path) == srv.data);
    }

    void expiredLinkErrorExplains()
    {
        RangeServer srv;
        srv.data = makeData(1024);
        srv.failStatus = 403;
        QVERIFY(srv.start());
        QTemporaryDir dir;
        WwwEngine eng;
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        eng.start(params(7, srv.url("/video.mp4?expires=1700000000"), dir.path()));
        const Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY(o.failed);
        QVERIFY2(o.error.startsWith("HTTP error 403: the link expired on"), qPrintable(o.error));
        QVERIFY(!DownloadManager::isRetriable(o.error));
    }

    // ffmpeg for converter tests: FLUXDM_FFMPEG, else FDM's bundled copy if installed (read-only use).
    static QString testFfmpeg()
    {
        const QString env = qEnvironmentVariable("FLUXDM_FFMPEG");
        if (!env.isEmpty() && QFileInfo(env).isExecutable())
            return env;
        const QString fdm = QStringLiteral("C:/Program Files/Softdeluxe/Free Download Manager/ffmpeg.exe");
        return QFileInfo(fdm).isExecutable() ? fdm : QString();
    }
    static bool makeMedia(const QString& ffmpeg, const QStringList& args)
    {
        QProcess p;
        p.start(ffmpeg, QStringList{"-hide_banner", "-loglevel", "error", "-nostdin", "-y"} + args);
        return p.waitForFinished(60000) && p.exitCode() == 0;
    }
    static Outcome waitConvert(MediaConvert& job, QList<double>* progress)
    {
        QSignalSpy fin(&job, &MediaConvert::finished);
        QObject::connect(&job, &MediaConvert::progress, &job, [progress](double f) { progress->append(f); });
        job.start();
        Outcome o;
        if (!fin.wait(120000))
            return o;
        o.path = fin.first().at(0).toString();
        o.error = fin.first().at(1).toString();
        o.finished = o.error.isEmpty();
        o.failed = !o.finished;
        return o;
    }

    void converterHelpers()
    {
        QCOMPARE(MediaConvert::parseClock("00:01:02.50"), 62.5);
        QCOMPARE(MediaConvert::parseClock("1:00:00"), 3600.0);
        QCOMPARE(MediaConvert::parseClock("bad"), -1.0);
        ConvertOptions o;
        o.bitrateKbps = 256;
        QVERIFY(MediaConvert::mp3Args("a.flac", "a.mp3", o, {"libmp3lame"}).join(' ').contains("-c:a libmp3lame -b:a 256k"));
        o.vbr = true;
        o.vbrQuality = 0;
        QVERIFY(MediaConvert::mp3Args("a.flac", "a.mp3", o, {"libmp3lame"}).join(' ').contains("-q:a 0"));
        QVERIFY(MediaConvert::mp3Args("a.flac", "a.mp3", o, {"mp3_mf"}).join(' ').contains("-c:a mp3_mf -b:a 192k"));
        QCOMPARE(MediaConvert::mp4Plan({"libx264", "h264_mf"}), (QStringList{"copy", "libx264", "mpeg4"}));
        QCOMPARE(MediaConvert::mp4Plan({"h264_mf"}), (QStringList{"copy", "h264_mf", "mpeg4"}));
        QCOMPARE(MediaConvert::mp4Plan({}), (QStringList{"copy", "mpeg4"}));
        QVERIFY(MediaConvert::mp4Args("a.mkv", "a.mp4", "copy").join(' ').contains("-c copy"));
    }

    void convertToMp3AndMp4()
    {
        const QString ffmpeg = testFfmpeg();
        if (ffmpeg.isEmpty())
            QSKIP("no ffmpeg (set FLUXDM_FFMPEG)");
        QTemporaryDir dir;
        const QString wav = dir.filePath("tone.wav");
        QVERIFY(makeMedia(ffmpeg, {"-f", "lavfi", "-i", "sine=frequency=440:duration=6", wav}));

        // MP3, constant 128 kbps: ~6 s * 16 KB/s
        ConvertOptions o;
        o.bitrateKbps = 128;
        QList<double> progress;
        MediaConvert mp3(ffmpeg, wav, o);
        QCOMPARE(QFileInfo(mp3.outputPath()).fileName(), QStringLiteral("tone.mp3"));
        Outcome r = waitConvert(mp3, &progress);
        QVERIFY2(r.finished, qPrintable(r.error));
        const qint64 size = QFileInfo(r.path).size();
        QVERIFY2(size > 80000 && size < 115000, qPrintable(QString::number(size)));
        QVERIFY(!progress.isEmpty() && progress.last() == 1.0);
        QVERIFY(QFile::exists(wav)); // the original stays

        // MP4 from MKV with MPEG-4 video + AAC: a lossless remux into another folder.
        const QString mkv = dir.filePath("clip.mkv");
        QVERIFY(makeMedia(ffmpeg, {"-f", "lavfi", "-i", "testsrc=duration=3:size=160x120:rate=10", "-f", "lavfi", "-i",
                                   "sine=duration=3", "-c:v", "mpeg4", "-c:a", "aac", "-shortest", mkv}));
        ConvertOptions m;
        m.format = QStringLiteral("mp4");
        m.outputDir = dir.filePath("out");
        progress.clear();
        MediaConvert mp4(ffmpeg, mkv, m);
        r = waitConvert(mp4, &progress);
        QVERIFY2(r.finished, qPrintable(r.error));
        QCOMPARE(QFileInfo(r.path).absolutePath(), QDir(dir.filePath("out")).absolutePath());
        QVERIFY(QFileInfo(r.path).size() > 1000);

        // PCM audio can't go into MP4 as is: falls back to re-encoding.
        const QString pcm = dir.filePath("pcm.mkv");
        QVERIFY(makeMedia(ffmpeg, {"-f", "lavfi", "-i", "testsrc=duration=2:size=160x120:rate=10", "-f", "lavfi", "-i",
                                   "sine=duration=2", "-c:v", "mpeg4", "-c:a", "pcm_s16le", "-shortest", pcm}));
        MediaConvert enc(ffmpeg, pcm, m);
        r = waitConvert(enc, &progress);
        QVERIFY2(r.finished, qPrintable(r.error));
        QVERIFY(QFileInfo(r.path).size() > 1000);

        // Not a media file: a clear error and no output left behind.
        const QString junk = dir.filePath("junk.mp4");
        {
            QFile f(junk);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("not a video");
        }
        MediaConvert bad(ffmpeg, junk, o);
        r = waitConvert(bad, &progress);
        QVERIFY(r.failed && !r.error.isEmpty());
        QVERIFY(!QFile::exists(bad.outputPath()));
    }

    void convertFromTheList()
    {
        const QString ffmpeg = testFfmpeg();
        if (ffmpeg.isEmpty())
            QSKIP("no ffmpeg (set FLUXDM_FFMPEG)");
        QTemporaryDir dir;
        const QString flac = dir.filePath("song.flac");
        QVERIFY(makeMedia(ffmpeg, {"-f", "lavfi", "-i", "sine=duration=3", flac}));
        Database db;
        QVERIFY(db.open(dir.path()));
        db.setSetting("FfmpegPath", ffmpeg);
        DownloadRecord r;
        r.id = 1;
        r.title = QStringLiteral("song.flac");
        r.filePath = QDir::toNativeSeparators(flac);
        r.destDir = dir.path();
        r.state = Download::Completed;
        db.save(r);
        DownloadManager mgr(&db);
        mgr.load();
        Download* d = mgr.find(1);
        QCOMPARE(DownloadManager::convertibleFile(d), QFileInfo(flac).absoluteFilePath());
        ConvertOptions o;
        o.format = QStringLiteral("mp4");
        QVERIFY(!mgr.convertFile(d, o).isEmpty()); // audio can't become MP4 video
        QSignalSpy done(&mgr, &DownloadManager::conversionFinished);
        o.format = QStringLiteral("mp3");
        QCOMPARE(mgr.convertFile(d, o), QString());
        QVERIFY(d->converting());
        QVERIFY(!mgr.convertFile(d, o).isEmpty()); // one at a time
        QVERIFY(done.wait(60000));
        QVERIFY(!d->converting());
        QCOMPARE(done.first().at(2).toString(), QString());
        QVERIFY(QFile::exists(dir.filePath("song.mp3")));
    }

    void torrentWatchFolder()
    {
        QTemporaryDir watched, keep;
        TorrentFolderWatcher w(keep.path());
        QSignalSpy added(&w, &TorrentFolderWatcher::torrentAdded);
        w.setFolder(watched.path());
        auto put = [&](const QString& name, const QByteArray& data) {
            QFile f(QDir(watched.path()).filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(data);
        };
        const QByteArray torrent = "d8:announce30:udp://tracker.example.org:13374:infod6:lengthi1024e4:name5:a.bin12:piece lengthi16384e6:pieces20:aaaaaaaaaaaaaaaaaaaaee";
        put("movie.torrent", torrent);
        put("broken.torrent", "this is not a torrent file at all, just text");
        w.scanNow(); // first look: sizes noted, nothing taken while a file may still be written
        QCOMPARE(added.count(), 0);
        w.scanNow(); // unchanged: taken
        QCOMPARE(added.count(), 1);
        QCOMPARE(added.first().at(1).toString(), QStringLiteral("movie.torrent"));
        const QString copy = added.first().at(0).toString();
        QCOMPARE(QFileInfo(copy).absolutePath(), QDir(keep.path()).absolutePath());
        QCOMPARE(readAll(copy), torrent);
        QVERIFY(QFile::exists(QDir(watched.path()).filePath("movie.torrent.added")));
        QVERIFY(!QFile::exists(QDir(watched.path()).filePath("movie.torrent")));
        QVERIFY(QFile::exists(QDir(watched.path()).filePath("broken.torrent"))); // not a torrent: left alone
        w.scanNow();
        w.scanNow();
        QCOMPARE(added.count(), 1); // never twice

        // The file system notification path works too (no manual scans).
        put("second.torrent", torrent);
        QTRY_COMPARE_WITH_TIMEOUT(added.count(), 2, 10000);
        w.setFolder({}); // off
        put("third.torrent", torrent);
        QTest::qWait(1500);
        QCOMPARE(added.count(), 2);
    }

    void proxySettings()
    {
        ProxySettings ps;
        QVERIFY(ps.curlProxy().isEmpty()); // system: curl's own defaults
        ps.mode = ProxySettings::None;
        QCOMPARE(ps.curlProxy(), QStringLiteral("none"));
        QCOMPARE(ps.qtProxy().type(), QNetworkProxy::NoProxy);
        ps.mode = ProxySettings::Manual;
        QVERIFY(ps.curlProxy().isEmpty()); // incomplete manual settings are ignored
        ps.host = QStringLiteral("proxy.example.com");
        ps.port = 3128;
        QCOMPARE(ps.curlProxy(), QStringLiteral("http://proxy.example.com:3128"));
        ps.type = QStringLiteral("socks5");
        ps.user = QStringLiteral("me");
        ps.password = QStringLiteral("p@ss word");
        QCOMPARE(ps.curlProxy(), QStringLiteral("socks5h://me:p%40ss%20word@proxy.example.com:3128"));
        QCOMPARE(ps.qtProxy().type(), QNetworkProxy::Socks5Proxy);

        QTemporaryDir dir;
        {
            Database db;
            QVERIFY(db.open(dir.path()));
            DownloadManager mgr(&db);
            mgr.setProxySettings(ps);
            QCOMPARE(mgr.proxyFor("https://example.com/a.zip"), ps.curlProxy());
            QVERIFY(!db.setting("ProxyPassword").toByteArray().contains("p@ss")); // encrypted at rest
            mgr.setMaxConnectionsPerServer(4);
        }
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadManager mgr(&db);
        QCOMPARE(mgr.proxySettings().password, QStringLiteral("p@ss word"));
        QCOMPARE(mgr.maxConnectionsPerServer(), 4);
        ProxySettings off;
        mgr.setProxySettings(off); // back to the system proxy for the rest of the tests
    }

    void downloadThroughProxy()
    {
        RangeServer proxy; // answers any absolute-URI request with its data, like a caching proxy would
        proxy.data = makeData(300 * 1024);
        QVERIFY(proxy.start());
        QTemporaryDir dir;
        WwwEngine eng;
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        TaskParams p = params(41, QStringLiteral("http://files.example.invalid/pack.bin"), dir.path());
        ProxySettings ps;
        ps.mode = ProxySettings::Manual;
        ps.host = QStringLiteral("127.0.0.1");
        ps.port = QUrl(proxy.url()).port();
        p.proxy = ps.curlProxy();
        eng.start(p);
        const Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QVERIFY(readAll(o.path) == proxy.data);
        QVERIFY(!proxy.requestLines.isEmpty());
        QVERIFY2(proxy.requestLines.first().startsWith("GET http://files.example.invalid/pack.bin"), proxy.requestLines.first().constData());
    }

    void connectionsPerServerCap()
    {
        RangeServer srv;
        srv.data = makeData(3 * 1024 * 1024);
        srv.chunk = 16 * 1024; // slow enough for the downloads to overlap
        QVERIFY(srv.start());
        QTemporaryDir dir;
        WwwEngine eng;
        eng.setMaxConnections(6);
        eng.setMaxConnectionsPerServer(3);
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        TaskParams a = params(42, srv.url("/a.bin"), dir.path());
        TaskParams b = params(43, srv.url("/b.bin"), dir.path());
        a.maxConnections = b.maxConnections = 6;
        eng.start(a);
        eng.start(b);
        QTRY_VERIFY_WITH_TIMEOUT(fin.count() == 2 || !fail.isEmpty(), 60000);
        QVERIFY2(fail.isEmpty(), qPrintable(fail.isEmpty() ? QString() : fail.first().at(1).toString()));
        // Transfers in progress, not sockets: a finished connection may linger a moment before it closes.
        QVERIFY2(srv.peakActive <= 3, qPrintable(QStringLiteral("peak %1 transfers").arg(srv.peakActive)));
        QVERIFY(srv.peakActive >= 2); // still parallel
    }

    void mpdParsing()
    {
        QCOMPARE(Mpd::parseDuration("PT1H2M3.5S"), 3723.5);
        QCOMPARE(Mpd::parseDuration("PT10S"), 10.0);
        QCOMPARE(Mpd::parseDuration("P1DT1S"), 86401.0);
        QCOMPARE(Mpd::parseDuration("garbage"), -1.0);
        QCOMPARE(Mpd::expandTemplate("v/$RepresentationID$/seg-$Number%05d$.m4s?a=$$x", "720p", 42, 0, 0),
                 QStringLiteral("v/720p/seg-00042.m4s?a=$x"));
        QCOMPARE(Mpd::expandTemplate("t-$Time$-$Bandwidth$.m4s", "a", 0, 96000, 128000), QStringLiteral("t-96000-128000.m4s"));

        const QUrl base("https://cdn.example.com/shows/ep1/manifest.mpd?token=abc");
        // SegmentTemplate with a fixed duration, BaseURL on the Period, three representations.
        const QByteArray tmpl = R"(<?xml version="1.0"?>
<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static" mediaPresentationDuration="PT10S">
 <Period><BaseURL>media/</BaseURL>
  <AdaptationSet mimeType="video/mp4">
   <SegmentTemplate initialization="$RepresentationID$/init.mp4" media="$RepresentationID$/$Number%03d$.m4s" startNumber="1" duration="4000" timescale="1000"/>
   <Representation id="v360" bandwidth="800000" width="640" height="360" codecs="avc1.4d401e"/>
   <Representation id="v1080" bandwidth="5000000" width="1920" height="1080" codecs="avc1.640028"/>
  </AdaptationSet>
  <AdaptationSet contentType="audio" mimeType="audio/mp4" lang="en">
   <Representation id="a128" bandwidth="128000" codecs="mp4a.40.2">
    <SegmentTemplate initialization="audio/init.mp4" media="audio/$Number$.m4s" duration="4" timescale="1"/>
   </Representation>
  </AdaptationSet>
 </Period>
</MPD>)";
        MpdManifest m = Mpd::parse(tmpl, base);
        QVERIFY2(m.valid, qPrintable(m.error));
        QCOMPARE(m.duration, 10.0);
        const MpdRepresentation* v = m.best("video");
        QVERIFY(v && v->id == QStringLiteral("v1080") && v->height == 1080);
        QCOMPARE(v->init.url.toString(), QStringLiteral("https://cdn.example.com/shows/ep1/media/v1080/init.mp4"));
        QCOMPARE(v->segments.size(), 3); // ceil(10 / 4)
        QCOMPARE(v->segments.last().url.toString(), QStringLiteral("https://cdn.example.com/shows/ep1/media/v1080/003.m4s"));
        const MpdRepresentation* a = m.best("audio");
        QVERIFY(a && a->lang == QStringLiteral("en"));
        QCOMPARE(a->segments.first().url.toString(), QStringLiteral("https://cdn.example.com/shows/ep1/media/audio/1.m4s"));

        // SegmentTimeline: explicit start, repeats, and $Time$.
        const QByteArray timeline = R"(<MPD type="static" mediaPresentationDuration="PT8S"><Period>
  <AdaptationSet mimeType="video/mp4"><SegmentTemplate timescale="90000" initialization="i-$RepresentationID$.mp4" media="s-$Time$.m4s">
   <SegmentTimeline><S t="0" d="180000" r="2"/><S d="90000"/><S d="90000" r="-1"/></SegmentTimeline></SegmentTemplate>
   <Representation id="v" bandwidth="1"/></AdaptationSet></Period></MPD>)";
        m = Mpd::parse(timeline, base);
        QVERIFY2(m.valid, qPrintable(m.error));
        const MpdRepresentation* t = m.best("video");
        QStringList names;
        for (const MpdSegment& s : t->segments)
            names << s.url.fileName();
        // 3 x 2 s, then 1 s, then 1 s repeated until 8 s
        QCOMPARE(names, (QStringList{"s-0.m4s", "s-180000.m4s", "s-360000.m4s", "s-540000.m4s", "s-630000.m4s"}));

        // SegmentList with byte ranges inside one file.
        const QByteArray list = R"(<MPD type="static" mediaPresentationDuration="PT6S"><Period><AdaptationSet mimeType="audio/mp4">
  <Representation id="a" bandwidth="64000"><BaseURL>track.mp4</BaseURL>
   <SegmentList><Initialization range="0-799"/><SegmentURL mediaRange="800-1999"/><SegmentURL mediaRange="2000-2999"/></SegmentList>
  </Representation></AdaptationSet></Period></MPD>)";
        m = Mpd::parse(list, base);
        QVERIFY2(m.valid, qPrintable(m.error));
        const MpdRepresentation* l = m.best("audio");
        QCOMPARE(l->init.rangeEnd, qint64(799));
        QCOMPARE(l->segments.size(), 2);
        QCOMPARE(l->segments[1].rangeStart, qint64(2000));
        QCOMPARE(l->segments[1].url.fileName(), QStringLiteral("track.mp4"));

        // SegmentBase / bare BaseURL: one file per representation.
        const QByteArray single = R"(<MPD type="static" mediaPresentationDuration="PT6S"><Period><AdaptationSet mimeType="video/mp4">
  <Representation id="v" bandwidth="1"><BaseURL>https://other.example.net/full.mp4</BaseURL><SegmentBase indexRange="0-99"/></Representation>
  </AdaptationSet></Period></MPD>)";
        m = Mpd::parse(single, base);
        QVERIFY2(m.valid, qPrintable(m.error));
        QCOMPARE(m.best("video")->segments.size(), 1);
        QCOMPARE(m.best("video")->segments[0].url.host(), QStringLiteral("other.example.net"));

        // What can't be downloaded says why.
        QVERIFY(Mpd::parse(R"(<MPD type="dynamic"><Period/></MPD>)", base).error.contains("Live"));
        QVERIFY(Mpd::parse(QByteArray(tmpl).replace("<Representation id=\"v360\"",
                                                    "<ContentProtection schemeIdUri=\"urn:mpeg:dash:mp4protection:2011\"/><Representation id=\"v360\""), base)
                    .error.contains("DRM"));
        QVERIFY(!Mpd::parse("<html>nope</html>", base).valid);
    }

    // Serves every file of a folder (a DASH stream made by ffmpeg) by path.
    static void serveFolder(HlsServer& srv, const QString& dir, const QString& prefix)
    {
        for (const QFileInfo& fi : QDir(dir).entryInfoList(QDir::Files)) {
            QFile f(fi.absoluteFilePath());
            if (f.open(QIODevice::ReadOnly))
                srv.files.insert(prefix + fi.fileName(), f.readAll());
        }
    }

    void dashDownload()
    {
        const QString ffmpeg = testFfmpeg();
        if (ffmpeg.isEmpty())
            QSKIP("no ffmpeg (set FLUXDM_FFMPEG)");
        QTemporaryDir src, out1, out2;
        // A 6-second stream with separate video and audio adaptation sets, 1-second segments.
        QVERIFY(makeMedia(ffmpeg, {"-f", "lavfi", "-i", "testsrc=duration=6:size=320x240:rate=10", "-f", "lavfi", "-i", "sine=duration=6",
                                   "-map", "0:v", "-map", "1:a", "-c:v", "mpeg4", "-c:a", "aac", "-f", "dash", "-seg_duration", "1",
                                   "-use_template", "1", "-use_timeline", "1", "-adaptation_sets", "id=0,streams=v id=1,streams=a",
                                   QDir(src.path()).filePath("show.mpd")}));
        HlsServer srv;
        serveFolder(srv, src.path(), "/dash/my-show/");
        QVERIFY(srv.start());
        QVERIFY(srv.files.size() > 8); // manifest, 2 init segments, media segments

        DashEngine eng;
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed), meta(&eng, &DownloadEngine::meta);
        TaskParams p = params(31, srv.url("/dash/my-show/show.mpd"), out1.path());
        p.ffmpeg = ffmpeg;
        eng.start(p);
        Outcome o = waitForEnd(fin, fail, 60000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QCOMPARE(QFileInfo(o.path).fileName(), QStringLiteral("show.mp4"));
        QCOMPARE(meta.count(), 1);
        QCOMPARE(QDir(out1.path()).entryList(QDir::Files), QStringList{"show.mp4"}); // parts cleaned up
        // Both tracks made it into the one file.
        QProcess probe;
        probe.start(ffmpeg, {"-hide_banner", "-i", o.path});
        QVERIFY(probe.waitForFinished(20000));
        const QString info = QString::fromLocal8Bit(probe.readAllStandardError());
        QVERIFY2(info.contains("Video:") && info.contains("Audio:"), qPrintable(info));

        // Without ffmpeg: the two tracks are kept as two playable files.
        DashEngine eng2;
        QSignalSpy fin2(&eng2, &DownloadEngine::finished), fail2(&eng2, &DownloadEngine::failed);
        TaskParams p2 = params(32, srv.url("/dash/my-show/show.mpd"), out2.path());
        eng2.start(p2);
        o = waitForEnd(fin2, fail2, 60000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QCOMPARE(QDir(out2.path()).entryList(QDir::Files), (QStringList{"show.m4a", "show.mp4"}));

        // Stop half-way and resume from the saved state: finished segments aren't fetched again.
        QTemporaryDir out3;
        srv.delayMs = 400;
        srv.hits.clear();
        DashEngine eng3;
        eng3.setMaxConnections(1); // one segment at a time, so the stop lands mid-way
        QSignalSpy fin3(&eng3, &DownloadEngine::finished), fail3(&eng3, &DownloadEngine::failed), stopped(&eng3, &DownloadEngine::stopped);
        QSignalSpy stats(&eng3, &DownloadEngine::stats);
        TaskParams p3 = params(33, srv.url("/dash/my-show/show.mpd"), out3.path());
        p3.ffmpeg = ffmpeg;
        eng3.start(p3);
        auto chunkHits = [&] {
            int n = 0;
            for (auto it = srv.hits.cbegin(); it != srv.hits.cend(); ++it)
                n += it.key().contains("chunk-") ? it.value() : 0;
            return n;
        };
        QTRY_VERIFY_WITH_TIMEOUT(chunkHits() >= 3, 20000); // a few media segments are on disk
        QVERIFY(fin3.isEmpty());
        eng3.stop(33);
        QTRY_VERIFY_WITH_TIMEOUT(!stopped.isEmpty(), 5000);
        p3.resumeData = stopped.last().at(1).toByteArray();
        QVERIFY(QJsonDocument::fromJson(p3.resumeData).object().value("kind").toString() == QStringLiteral("dash"));
        srv.delayMs = 0;
        eng3.start(p3);
        o = waitForEnd(fin3, fail3, 60000);
        QVERIFY2(o.finished, qPrintable(o.error));
        // Each segment once, except at most the one that was in flight when we stopped.
        int chunks = 0, repeats = 0;
        for (auto it = srv.hits.cbegin(); it != srv.hits.cend(); ++it)
            if (it.key().contains("chunk-")) {
                ++chunks;
                repeats += it.value() - 1;
            }
        QCOMPARE(chunks, 11);
        QVERIFY2(repeats <= 1, qPrintable(QString::number(repeats)));
        probe.start(ffmpeg, {"-hide_banner", "-i", o.path});
        QVERIFY(probe.waitForFinished(20000));
        const QString info3 = QString::fromLocal8Bit(probe.readAllStandardError());
        QVERIFY2(info3.contains("Video:") && info3.contains("Audio:") && info3.contains("Duration: 00:00:06"), qPrintable(info3));
    }

    void mirrors()
    {
        const QByteArray file = makeData(3 * 1024 * 1024);
        RangeServer main, mirror, other, broken;
        main.data = mirror.data = file;
        other.data = makeData(1024 * 1024); // a different file
        broken.data = file;
        broken.failStatus = 404;
        for (RangeServer* s : {&main, &mirror, &other, &broken}) {
            s->chunk = 32 * 1024; // slow enough for the download to split into several parts
            QVERIFY(s->start());
        }
        QTemporaryDir dir;
        WwwEngine eng;
        eng.setMaxConnections(6);
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed), log(&eng, &DownloadEngine::log);

        TaskParams p = params(71, main.url("/pack.iso"), dir.path());
        p.maxConnections = 6;
        p.mirrors = {mirror.url("/pack.iso")};
        eng.start(p);
        Outcome o = waitForEnd(fin, fail, 60000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QVERIFY(readAll(o.path) == file);
        QVERIFY2(mirror.requests >= 1, "the mirror was never used");
        QVERIFY(main.requests >= 1);

        // A mirror with another file and one that answers 404: both dropped, the file is still right.
        fin.clear();
        p.id = 72;
        p.mirrors = {other.url("/pack.iso"), broken.url("/pack.iso")};
        eng.start(p);
        o = waitForEnd(fin, fail, 60000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QVERIFY(readAll(o.path) == file);
        QStringList lines;
        for (const auto& l : log)
            if (l.at(0).toLongLong() == 72)
                lines << l.at(1).toString();
        // Each bad mirror is dropped with a reason (a different file is caught by its size or length).
        const QString all = lines.join('\n');
        QVERIFY2(all.contains("Mirror " + other.url("/pack.iso") + " failed"), qPrintable(all));
        QVERIFY2(all.contains("Mirror " + broken.url("/pack.iso") + " failed (HTTP 404)"), qPrintable(all));
    }

    void postDownload()
    {
        RangeServer srv;
        srv.data = makeData(200 * 1024);
        QVERIFY(srv.start());
        QTemporaryDir dir;
        WwwEngine eng;
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        TaskParams p = params(61, srv.url("/export.php"), dir.path());
        p.fileName = QStringLiteral("export.csv");
        p.postData = "report=sales&year=2026&token=abc";
        eng.start(p);
        const Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QVERIFY(readAll(o.path) == srv.data);
        QVERIFY2(srv.requestLines.first().startsWith("POST /export.php"), srv.requestLines.first().constData());
        QVERIFY(srv.bodies.first().contains("report=sales&year=2026&token=abc"));

        // Kept with the download, so a restart submits the form again.
        Database db;
        QVERIFY(db.open(dir.filePath("profile")));
        DownloadRecord r;
        r.id = 1;
        r.postData = p.postData;
        db.save(r);
        QCOMPARE(db.loadDownloads().first().postData, p.postData);
    }

    void metalinkFiles()
    {
        const QByteArray v4 = R"(<?xml version="1.0" encoding="UTF-8"?>
<metalink xmlns="urn:ietf:params:xml:ns:metalink">
  <file name="../evil/dir/ubuntu.iso">
    <size>1000</size>
    <hash type="md5">d41d8cd98f00b204e9800998ecf8427e</hash>
    <hash type="sha-256">E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855</hash>
    <url priority="2">https://mirror2.example.org/ubuntu.iso</url>
    <url priority="1">https://mirror1.example.org/ubuntu.iso</url>
    <url priority="3">rsync://nope.example.org/ubuntu.iso</url>
  </file>
  <file name="empty.bin"><url>rsync://only.example.org/x</url></file>
</metalink>)";
        const auto files = Metalink::parse(v4);
        QCOMPARE(files.size(), 1); // nothing usable in the second one
        QCOMPARE(files[0].name, QStringLiteral("ubuntu.iso")); // folders dropped
        QCOMPARE(files[0].size, 1000);
        QCOMPARE(files[0].urls, (QStringList{"https://mirror1.example.org/ubuntu.iso", "https://mirror2.example.org/ubuntu.iso"}));
        QVERIFY(files[0].hash.startsWith("sha-256:e3b0")); // the strongest one
        const QByteArray v3 = R"(<metalink version="3.0" xmlns="http://www.metalinker.org/"><files><file name="a.zip">
            <verification><hash type="sha1">da39a3ee5e6b4b0d3255bfef95601890afd80709</hash></verification>
            <resources><url type="http" preference="10">http://slow.example.org/a.zip</url>
                       <url type="http" preference="100">http://fast.example.org/a.zip</url></resources></file></files></metalink>)";
        const auto old = Metalink::parse(v3);
        QCOMPARE(old.size(), 1);
        QCOMPARE(old[0].urls.first(), QStringLiteral("http://fast.example.org/a.zip"));
        QVERIFY(Checksum::parse(old[0].hash).valid());

        // A .meta4 downloaded from the web turns into the real download, mirrors and checksum included.
        RangeServer srv;
        srv.data = makeData(128 * 1024);
        QVERIFY(srv.start());
        const QString sha = QString::fromLatin1(QCryptographicHash::hash(srv.data, QCryptographicHash::Sha256).toHex());
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.filePath("p")));
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();
        const QString meta = dir.filePath("set.meta4");
        QFile f(meta);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QStringLiteral(R"(<metalink xmlns="urn:ietf:params:xml:ns:metalink"><file name="data.bin">
            <hash type="sha-256">%1</hash><url priority="1">%2</url><url priority="2">%3</url></file></metalink>)")
                    .arg(sha, srv.url("/data.bin"), srv.url("/mirror/data.bin")).toUtf8());
        f.close();
        QVERIFY(Metalink::looksLike(meta));
        const auto added = mgr.addFromMetalink(meta, dir.path(), true);
        QCOMPARE(added.size(), 1);
        Download* d = added.first();
        QCOMPARE(d->rec.mirrors, QStringList{srv.url("/mirror/data.bin")});
        QTRY_COMPARE_WITH_TIMEOUT(d->hashStatus(), int(Download::HashMatch), 20000);
        QCOMPARE(QFileInfo(d->rec.filePath).fileName(), QStringLiteral("data.bin"));
    }

#ifdef FLUXDM_HAS_TORRENT
    void createTorrentFromFile()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("share me.bin");
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(makeData(300 * 1024));
        f.close();
        QString err;
        QVERIFY2(BtEngine::createTorrent(file, dir.filePath("direct.torrent"), "https://example.com/share%20me.bin", &err), qPrintable(err));
        QVERIFY(FileNamer::looksLikeTorrentFile(dir.filePath("direct.torrent")));

        Database db;
        QVERIFY(db.open(dir.filePath("p")));
        DownloadRecord r;
        r.id = 1;
        r.url = QStringLiteral("https://example.com/share%20me.bin");
        r.state = Download::Completed;
        r.filePath = file;
        r.destDir = dir.path();
        QVERIFY(db.save(r));
        DownloadManager mgr(&db);
        mgr.addEngine(new BtEngine(dir.filePath("bt"), 0, true));
        mgr.addEngine(new WwwEngine);
        mgr.load();
        Download* d = mgr.find(1);
        QTRY_VERIFY(!d->fileMissing());
        QVERIFY(mgr.canCreateTorrent(d));
        QSignalSpy made(&mgr, &DownloadManager::torrentCreated);
        QVERIFY(mgr.createTorrent(d).isEmpty());
        QVERIFY(made.wait(15000));
        QVERIFY2(made.first().at(1).toString().isEmpty(), qPrintable(made.first().at(1).toString()));
        QCOMPARE(QFileInfo(made.first().at(0).toString()).fileName(), QStringLiteral("share me.bin.torrent"));
        // Added as a torrent over the existing data: it checks the file and seeds.
        Download* t = nullptr;
        for (Download* x : mgr.downloads())
            if (x->isTorrent())
                t = x;
        QVERIFY(t);
        QTRY_VERIFY_WITH_TIMEOUT(t->completed(), 15000);
    }
#endif

    void extractArchives()
    {
        QVERIFY(DownloadManager::isArchive("C:/d/a.zip") && DownloadManager::isArchive("x.tar.gz") && DownloadManager::isArchive("y.RAR"));
        QVERIFY(!DownloadManager::isArchive("movie.mp4") && !DownloadManager::isArchive("zip"));
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.filePath("p")));
        DownloadManager mgr(&db);
        if (mgr.extractorPath().isEmpty())
            QSKIP("no 7-Zip and no tar.exe");
        // A zip made with Windows' tar.
        QDir(dir.path()).mkpath("src/inner");
        QFile a(dir.filePath("src/inner/readme.txt"));
        QVERIFY(a.open(QIODevice::WriteOnly));
        a.write("hello");
        a.close();
        QProcess tar;
        tar.setWorkingDirectory(dir.filePath("src"));
        tar.start(QDir(qEnvironmentVariable("SystemRoot")).filePath("System32/tar.exe"), {"-a", "-cf", dir.filePath("pack.zip"), "inner"});
        QVERIFY(tar.waitForFinished(10000) && tar.exitCode() == 0);

        DownloadRecord r;
        r.id = 1;
        r.url = QStringLiteral("https://example.com/pack.zip");
        r.state = Download::Completed;
        r.filePath = dir.filePath("pack.zip");
        QVERIFY(db.save(r));
        mgr.load();
        Download* d = mgr.find(1);
        QTRY_VERIFY(!d->fileMissing());
        QVERIFY(mgr.canExtract(d));
        QSignalSpy done(&mgr, &DownloadManager::extracted);
        QVERIFY(mgr.extract(d).isEmpty());
        QVERIFY(done.wait(15000));
        QVERIFY2(done.first().at(2).toString().isEmpty(), qPrintable(done.first().at(2).toString()));
        const QString folder = done.first().at(1).toString();
        QCOMPARE(QFileInfo(folder).fileName(), QStringLiteral("pack"));
        QCOMPARE(readAll(QDir(folder).filePath("inner/readme.txt")), QByteArray("hello"));
        QVERIFY(!d->converting());
        // Again: a second folder, nothing overwritten.
        QVERIFY(mgr.extract(d).isEmpty());
        QVERIFY(done.wait(15000));
        QVERIFY(done.last().at(1).toString() != folder);
    }

    void missingFilesAreNoticed()
    {
        QTemporaryDir dir;
        const QString here = dir.filePath("here.bin"), gone = dir.filePath("gone.bin");
        QFile f(here);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
        {
            Database db;
            QVERIFY(db.open(dir.filePath("p")));
            for (int i = 1; i <= 2; ++i) {
                DownloadRecord r;
                r.id = i;
                r.url = QStringLiteral("https://example.com/%1").arg(i);
                r.state = Download::Completed;
                r.filePath = i == 1 ? here : gone;
                QVERIFY(db.save(r));
            }
        }
        Database db;
        QVERIFY(db.open(dir.filePath("p")));
        DownloadManager mgr(&db);
        mgr.load(); // checks on a worker thread
        QTRY_VERIFY_WITH_TIMEOUT(mgr.find(2)->fileMissing(), 5000);
        QVERIFY(!mgr.find(1)->fileMissing());
    }

    void trafficStatistics()
    {
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.path()));
        QVERIFY(db.addUsage("2026-10-31", 100, 5));
        QVERIFY(db.addUsage("2026-11-02", 1000, 50));
        QVERIFY(db.addUsage("2026-11-02", 1000, 50)); // adds up
        QCOMPARE(db.usageSince("2026-11-01"), (QPair<qint64, qint64>(2000, 100)));
        QCOMPARE(db.usageSince(QString()), (QPair<qint64, qint64>(2100, 105)));

        RangeServer srv;
        srv.data = makeData(256 * 1024);
        QVERIFY(srv.start());
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();
        Download* d = mgr.add(srv.url("/t.bin"), dir.path(), {}, true);
        QTRY_VERIFY_WITH_TIMEOUT(d->completed(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(mgr.usage().value("todayDown").toLongLong() > 0, 3000);
        const qint64 today = mgr.usage().value("todayDown").toLongLong();
        QVERIFY2(today <= srv.data.size(), qPrintable(QString::number(today)));
        QVERIFY(mgr.usage().value("totalDown").toLongLong() >= 2100 + today);
        mgr.flushUsage();
        QCOMPARE(db.usageSince(QDate::currentDate().toString(Qt::ISODate)).first, today); // written, not doubled
        QCOMPARE(mgr.usage().value("todayDown").toLongLong(), today);
    }

    void connectionSettingsAndCleanup()
    {
        RangeServer srv;
        srv.data = makeData(64 * 1024);
        QVERIFY(srv.start());
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();

        // Own user agent (control characters dropped); a browser's one still wins for its downloads.
        mgr.setUserAgent(QStringLiteral("MyAgent/1.0\r\nX-Bad: 1"));
        QCOMPARE(mgr.userAgent(), QStringLiteral("MyAgent/1.0X-Bad: 1"));
        mgr.setUserAgent(QStringLiteral("MyAgent/1.0"));
        mgr.setIpVersion(4);
        Download* a = mgr.add(srv.url("/ua.bin"), dir.path(), {}, true);
        Download* b = mgr.add(srv.url("/browser.bin"), dir.path(), {}, true, {}, {}, QStringLiteral("BrowserUA/2"));
        QTRY_VERIFY_WITH_TIMEOUT(a->completed() && b->completed(), 20000);
        bool sawMine = false, sawBrowser = false;
        for (const QByteArray& h : srv.heads) {
            sawMine |= h.contains("User-Agent: MyAgent/1.0\r\n");
            sawBrowser |= h.contains("User-Agent: BrowserUA/2\r\n");
        }
        QVERIFY(sawMine && sawBrowser);
        QCOMPARE(mgr.ipVersion(), 4);
        mgr.setIpVersion(5);
        QCOMPARE(mgr.ipVersion(), 0); // only 4 or 6

        TaskParams p;
        p.url = QStringLiteral("https://youtu.be/abc");
        p.ipVersion = 6;
        QVERIFY(SiteEngine::arguments(p, 1, 0).contains(QStringLiteral("-6")));

        // Finished downloads leave the list after N days; files stay.
        a->rec.finished = QDateTime::currentDateTime().addDays(-10);
        const QString kept = a->rec.filePath;
        QCOMPARE(mgr.removeOldFinished(), 0); // off by default
        mgr.setAutoRemoveDays(7);             // applies right away
        QVERIFY(!mgr.findByUrl(srv.url("/ua.bin")));
        QVERIFY(mgr.findByUrl(srv.url("/browser.bin"))); // finished today
        QVERIFY(QFileInfo::exists(kept));
        mgr.setBtListenPort(80);
        QCOMPARE(mgr.btListenPort(), 1024);
    }

    void checksumVerification()
    {
        const QString sha = QStringLiteral("E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");
        QCOMPARE(Checksum::parse(sha).name, QStringLiteral("SHA-256"));
        QCOMPARE(Checksum::parse("SHA256: " + sha).hex, sha.toLower());
        QCOMPARE(Checksum::parse(sha.toLower() + "  ubuntu.iso").algo, QCryptographicHash::Sha256);
        QCOMPARE(Checksum::parse("d41d8cd98f00b204e9800998ecf8427e").name, QStringLiteral("MD5"));
        QCOMPARE(Checksum::parse("da39a3ee5e6b4b0d3255bfef95601890afd80709").name, QStringLiteral("SHA-1"));
        QVERIFY(!Checksum::parse("not a hash").valid());
        QVERIFY(!Checksum::parse("abc123").valid());

        RangeServer srv;
        srv.data = makeData(300 * 1024);
        QVERIFY(srv.start());
        const QString good = QString::fromLatin1(QCryptographicHash::hash(srv.data, QCryptographicHash::Sha256).toHex());
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();
        QSignalSpy checked(&mgr, &DownloadManager::checksumChecked);

        // Published checksum given when adding: verified on completion.
        Download* a = mgr.add(srv.url("/a.bin"), dir.path(), {}, false);
        mgr.setExpectedChecksum(a, "sha256:" + good);
        mgr.resume(a);
        QTRY_COMPARE_WITH_TIMEOUT(a->hashStatus(), int(Download::HashMatch), 20000);
        QCOMPARE(checked.count(), 1);
        QVERIFY(checked.first().at(1).toBool());

        // A wrong one is reported.
        Download* b = mgr.add(srv.url("/b.bin"), dir.path(), {}, false);
        mgr.setExpectedChecksum(b, "d41d8cd98f00b204e9800998ecf8427e");
        mgr.resume(b);
        QTRY_COMPARE_WITH_TIMEOUT(b->hashStatus(), int(Download::HashMismatch), 20000);
        QVERIFY(!checked.last().at(1).toBool());
        QVERIFY(b->hashText().contains("does not match"));

        // Checked later by hand; bad input refused.
        QVERIFY(!mgr.verifyChecksum(b, "xyz").isEmpty());
        QVERIFY(mgr.verifyChecksum(b, good).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(b->hashStatus(), int(Download::HashMatch), 10000);

        // The verdict is kept.
        const auto rows = db.loadDownloads();
        for (const DownloadRecord& r : rows)
            QCOMPARE(r.hashStatus, int(Download::HashMatch));
    }

    void diskFullWaitsForSpace()
    {
        QVERIFY(DiskSpace::isShortage(DiskSpace::shortageText(2LL << 30, 1LL << 30, QDir::tempPath())));
        QVERIFY(DiskSpace::shortageText(2LL << 30, 1LL << 30, QDir::tempPath()).contains("2.0 GB"));
        QVERIFY(DiskSpace::looksLikeFullDisk("Disk write error: There is not enough space on the disk."));
        QVERIFY(!DiskSpace::looksLikeFullDisk("HTTP error 404"));
        QVERIFY(DiskSpace::freeBytes(QDir::temp().filePath("no/such/folder/yet")) > 0); // nearest existing folder

        RangeServer srv;
        srv.data = makeData(2 * 1024 * 1024);
        QVERIFY(srv.start());
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();

        qint64 fakeFree = 100 * 1024; // less than the file needs
        DiskSpace::setFreeBytesOverride([&](const QString&) { return fakeFree; });
        auto restore = qScopeGuard([] { DiskSpace::setFreeBytesOverride(nullptr); });

        Download* d = mgr.add(srv.url("/big.bin"), dir.path(), {}, true);
        QTRY_VERIFY_WITH_TIMEOUT(d->failed(), 10000);
        QVERIFY2(DiskSpace::isShortage(d->rec.lastError), qPrintable(d->rec.lastError));
        QVERIFY(!d->autoRetryAt.isValid()); // waits for space, doesn't burn automatic retries

        mgr.recheckDiskSpace(); // still not enough: keeps waiting
        QVERIFY(d->failed());

        fakeFree = 1LL << 40; // space was freed
        mgr.recheckDiskSpace();
        QTRY_VERIFY_WITH_TIMEOUT(d->completed(), 20000);
        QCOMPARE(readAll(d->rec.filePath), srv.data);

        // Size known (e.g. resumed later): checked before the engine even starts.
        fakeFree = 1024;
        Download* e = mgr.add(srv.url("/again.bin"), dir.path(), {}, false);
        e->rec.total = srv.data.size();
        mgr.resume(e);
        QTRY_VERIFY_WITH_TIMEOUT(e->failed(), 5000);
        QVERIFY(DiskSpace::isShortage(e->rec.lastError));
    }

    void missingDriveWaits()
    {
        QCOMPARE(DiskSpace::volumeRoot("D:\\Downloads\\x"), QStringLiteral("D:/"));
        QCOMPARE(DiskSpace::volumeRoot("\\\\nas\\media\\films"), QStringLiteral("//nas/media"));
        QCOMPARE(DiskSpace::volumeRoot("relative/dir"), QString());
        QVERIFY(DiskSpace::volumeAvailable(QDir::tempPath()));
        QString missing;
        for (char c = 'Z'; c >= 'G' && missing.isEmpty(); --c)
            if (!QFileInfo::exists(QStringLiteral("%1:/").arg(QLatin1Char(c))))
                missing = QStringLiteral("%1:/FluxDM test").arg(QLatin1Char(c));
        if (missing.isEmpty())
            QSKIP("every drive letter is in use");
        QVERIFY(!DiskSpace::volumeAvailable(missing));

        RangeServer srv;
        srv.data = makeData(256 * 1024);
        QVERIFY(srv.start());
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();
        Download* d = mgr.add(srv.url("/usb.bin"), missing, {}, true);
        QTRY_VERIFY_WITH_TIMEOUT(d->failed(), 5000);
        QVERIFY2(DiskSpace::isUnavailable(d->rec.lastError), qPrintable(d->rec.lastError));
        QVERIFY(!d->autoRetryAt.isValid());
        QCOMPARE(srv.requests, 0); // nothing was fetched for a drive that isn't there

        mgr.recheckDiskSpace(); // still missing
        QVERIFY(d->failed());
        d->rec.destDir = dir.path(); // "the drive is back"
        mgr.recheckDiskSpace();
        QTRY_VERIFY_WITH_TIMEOUT(d->completed(), 20000);
    }

    void duplicateLinks()
    {
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();
        Download* a = mgr.add(QStringLiteral("https://example.com/files/a.zip"), dir.path(), {}, false);
        QVERIFY(a);
        QCOMPARE(mgr.findByUrl(QStringLiteral("https://example.com/files/a.zip")), a);
        QCOMPARE(mgr.findByUrl(QStringLiteral(" https://example.com/files/a.zip#top ")), a); // fragment, spaces
        QVERIFY(!mgr.findByUrl(QStringLiteral("https://example.com/files/a.zip?v=2")));
        QVERIFY(!mgr.findByUrl(QStringLiteral("https://example.com/files/b.zip")));
        QVERIFY(!mgr.findByUrl(QString()));
        Download* again = mgr.add(QStringLiteral("https://example.com/files/a.zip"), dir.path(), {}, false);
        QCOMPARE(mgr.findByUrl(QStringLiteral("https://example.com/files/a.zip")), again); // newest first
    }

    void statsAreCoalesced()
    {
        RangeServer srv;
        srv.data = makeData(8 * 1024 * 1024);
        srv.chunk = 8 * 1024; // slow, so all of them keep running
        QVERIFY(srv.start());
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();
        QList<Download*> list;
        for (int i = 0; i < 4; ++i)
            list << mgr.add(srv.url(QStringLiteral("/s%1.bin").arg(i)), dir.path(), {}, true);
        for (Download* d : list)
            QCOMPARE(mgr.find(d->id()), d);
        QTRY_VERIFY_WITH_TIMEOUT(std::all_of(list.begin(), list.end(), [](Download* d) { return d->running(); }), 5000);

        QSignalSpy spy(&mgr, &DownloadManager::statsUpdated);
        QTest::qWait(2000);
        // 4 tasks report every 500 ms (16 signals in 2 s); the UI hears about it at most every 100 ms.
        QVERIFY2(spy.count() >= 2 && spy.count() <= 21, qPrintable(QString::number(spy.count())));

        const qint64 gone = list.first()->id();
        mgr.remove(list.first(), true);
        QVERIFY(!mgr.find(gone));
        QCOMPARE(mgr.find(list.last()->id()), list.last());
    }

    void fileWriterFlushes()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("f.part");
        FileWriter w;
        QVERIFY(w.open(path, true));
        QVERIFY(w.flush());
        QCOMPARE(w.flushes(), 0); // nothing written yet
        QVERIFY(w.write(0, "abcd", 4));
        QVERIFY(w.flush());
        QCOMPARE(w.flushes(), 1);
        QVERIFY(w.flush());
        QCOMPARE(w.flushes(), 1); // nothing new since
        QVERIFY(w.write(4, "ef", 2));
        w.close(); // flushes what is left
        QCOMPARE(w.flushes(), 2);
        QCOMPARE(readAll(path), QByteArray("abcdef"));

        // In the background: the callback runs after the data is on the disk; close() waits for it.
        FileWriter a;
        QVERIFY(a.open(dir.filePath("g.part"), true));
        QVERIFY(a.write(0, "xyz", 3));
        std::atomic_bool flushed{false};
        QVERIFY(a.flushAsync([&](bool ok) { flushed = ok; }));
        a.close();
        QVERIFY(flushed.load());
        QCOMPARE(a.flushes(), 1);
    }

    void databaseWriteFailures()
    {
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.path()));
        QStringList reported;
        db.setErrorHandler([&](const QString& e) { reported << e; });

        DownloadRecord r;
        r.id = 1;
        r.url = QStringLiteral("https://example.com/a.zip");
        r.tags = {1};
        QVERIFY(db.save(r));
        QVERIFY(db.saveProgress(1, 10, "rd"));
        QCOMPARE(db.writeErrors(), 0);

        // Another connection damages the schema: a save touching tags fails as a whole.
        {
            QSqlDatabase other = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("other"));
            other.setDatabaseName(QDir(dir.path()).filePath("db.sqlite"));
            QVERIFY(other.open());
            QSqlQuery q(other);
            QVERIFY(q.exec(QStringLiteral("DROP TABLE downloadsTags")));
            other.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("other"));
        DownloadRecord r2 = r;
        r2.id = 2;
        QVERIFY(!db.save(r2));
        QVERIFY(db.writeErrors() > 0);
        QVERIFY(!reported.isEmpty());
        QVERIFY(!db.lastError().isEmpty());
        const auto rows = db.loadDownloads();
        QCOMPARE(rows.size(), 1); // rolled back: no half-saved download 2
        QCOMPARE(rows.first().received, 10);

        // The manager turns write failures into one (rate-limited) storageError.
        DownloadManager mgr(&db);
        QSignalSpy err(&mgr, &DownloadManager::storageError);
        DownloadRecord r3 = r;
        r3.id = 3;
        QVERIFY(!db.save(r3));
        QVERIFY(!db.save(r3));
        QCOMPARE(err.count(), 1);
    }

    void headerInjectionIsStripped()
    {
        // Referrer, cookies and user agent come from the browser extension or the remote page.
        QCOMPARE(headerSafe(QStringLiteral(" https://a.example/\r\nX-Injected: 1 ")), QStringLiteral("https://a.example/X-Injected: 1"));
        QCOMPARE(headerSafe(QStringLiteral("a=1; b=2")), QStringLiteral("a=1; b=2"));

        RangeServer srv;
        srv.data = makeData(64 * 1024);
        QVERIFY(srv.start());
        QTemporaryDir dir;
        WwwEngine eng;
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        TaskParams p = params(62, srv.url("/h.bin"), dir.path(), 1);
        p.referrer = QStringLiteral("https://a.example/\r\nX-Injected: ref");
        p.cookies = QStringLiteral("sid=1\r\nX-Injected: cookie");
        p.userAgent = QStringLiteral("Agent\nX-Injected: ua");
        eng.start(p);
        const Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QVERIFY(!srv.heads.isEmpty());
        for (const QByteArray& h : srv.heads) {
            QVERIFY2(!h.contains("\nX-Injected:"), h.constData());
            QVERIFY2(h.contains("Referer: https://a.example/X-Injected: ref"), h.constData());
        }

        p.url = QStringLiteral("https://youtu.be/abc");
        for (const QString& a : SiteEngine::arguments(p, 1, 0))
            QVERIFY2(!a.contains(QLatin1Char('\r')) && !a.contains(QLatin1Char('\n')), qPrintable(a));
    }

    void meteredConnection()
    {
        RangeServer srv;
        srv.data = makeData(4 * 1024 * 1024);
        srv.chunk = 4 * 1024; // slow: still running while we flip the connection
        QVERIFY(srv.start());
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();
        Download* a = mgr.add(srv.url("/a.bin"), dir.path(), {}, true);
        Download* b = mgr.add(srv.url("/b.bin"), dir.path(), {}, true);
        Download* mine = mgr.add(srv.url("/c.bin"), dir.path(), {}, false); // paused by the user
        QTRY_VERIFY_WITH_TIMEOUT(a->running() && b->running(), 5000);

        MeteredGuard g(&mgr);
        QSignalSpy ask(&g, &MeteredGuard::askUser), paused(&g, &MeteredGuard::paused), resumed(&g, &MeteredGuard::resumed);
        g.setMetered(true); // policy Ask (default)
        QCOMPARE(ask.count(), 1);
        QCOMPARE(ask.first().at(0).toInt(), 2);
        QVERIFY(a->running()); // nothing happens until the user answers
        g.pauseNow();
        QVERIFY(a->paused() && b->paused());
        g.setMetered(false); // back on Wi-Fi: only what we paused continues
        QCOMPARE(resumed.first().at(0).toInt(), 2);
        QVERIFY(!a->paused() && !b->paused());
        QVERIFY(mine->paused());

        g.setPolicy(MeteredGuard::Pause);
        g.setMetered(true);
        QCOMPARE(paused.count(), 2); // once by hand, once automatically
        QVERIFY(a->paused());
        g.setMetered(false);
        QTRY_VERIFY_WITH_TIMEOUT(a->running() || a->queued(), 5000);

        g.setPolicy(MeteredGuard::Ask);
        g.setMetered(true);
        g.keepDownloading(); // "Keep downloading": no more questions while metered
        g.setMetered(true);
        QCOMPARE(ask.count(), 2);
        g.setMetered(false);

        g.setPolicy(MeteredGuard::Ignore);
        g.setMetered(true);
        QCOMPARE(ask.count(), 2);
        QVERIFY(!a->paused());
        mgr.pauseAll();
    }

    void untrustedCertificate()
    {
        const QString python = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("Programs/Python/Python312/python.exe");
        const QString openssl = QStringLiteral("C:/Program Files/Git/usr/bin/openssl.exe");
        const QString script = QFINDTESTDATA("https_server.py");
        if (!QFileInfo(python).isExecutable() || !QFileInfo(openssl).isExecutable() || script.isEmpty())
            QSKIP("needs Python and OpenSSL (Git for Windows) for a self-signed HTTPS server");
        QTemporaryDir dir;
        const QString cert = dir.filePath("cert.pem"), key = dir.filePath("key.pem");
        QProcess gen;
        gen.start(openssl, {"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert, "-days", "2",
                            "-subj", "/CN=127.0.0.1"});
        QVERIFY(gen.waitForFinished(30000) && gen.exitCode() == 0);
        QProcess server;
        server.start(python, {"-I", script, cert, key, QString::number(150 * 1024)});
        QVERIFY(server.waitForReadyRead(10000));
        const int port = QString::fromLatin1(server.readLine()).section(' ', 1).trimmed().toInt();
        QVERIFY(port > 0);
        const QString url = QStringLiteral("https://127.0.0.1:%1/nas/backup.bin").arg(port);

        Database db;
        QVERIFY(db.open(dir.filePath("profile")));
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();
        Download* d = mgr.add(url, dir.path(), {}, true);
        QTRY_VERIFY_WITH_TIMEOUT(d->failed(), 20000);
        QVERIFY2(d->certificateProblem(), qPrintable(d->error()));
        QVERIFY(!d->autoRetryAt.isValid()); // retrying won't change the certificate
        mgr.trustCertificate(d);           // "Trust this server and continue"
        QTRY_VERIFY_WITH_TIMEOUT(d->completed(), 20000);
        QCOMPARE(QFileInfo(d->filePath()).size(), qint64(150 * 1024));
        QCOMPARE(mgr.trustedHosts(), QStringList{"127.0.0.1"});
        mgr.forgetTrustedHosts();
        QVERIFY(mgr.trustedHosts().isEmpty());
        server.kill();
        server.waitForFinished(3000);
    }

    void importFromFdm()
    {
        QTemporaryDir dir;
        const QString fdm = dir.filePath("fdm-db.sqlite");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "fake-fdm");
            db.setDatabaseName(fdm);
            QVERIFY(db.open());
            QSqlQuery q(db);
            // The columns FluxDM reads, as in FDM 6's db.sqlite.
            QVERIFY(q.exec("CREATE TABLE downloads (id INTEGER PRIMARY KEY, moduleUid TEXT, url TEXT, referrer TEXT, userAgent TEXT,"
                           " destinationPath TEXT, title TEXT, creationTime INTEGER, finishedTime INTEGER, priority INTEGER)"));
            QVERIFY(q.exec("CREATE TABLE tags (id INTEGER PRIMARY KEY, name TEXT, color TEXT)"));
            QVERIFY(q.exec("CREATE TABLE downloadsTags (downloadId INTEGER PRIMARY KEY, tagsIds TEXT)"));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO downloads VALUES (1, 'downloadswww', 'https://example.com/a.zip', '', '', '%1', 'a.zip', 1791483710, 1791483800, 2)")
                               .arg(dir.path())));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO downloads VALUES (2, 'downloadsbt', 'magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Show', '', '', '%1', 'Show', 1791483900, 0, 1)")
                               .arg(dir.path())));
            QVERIFY(q.exec("INSERT INTO tags VALUES (7, 'Work', '#e5534b')"));
            QVERIFY(q.exec("INSERT INTO downloadsTags VALUES (1, '7')"));
            db.close();
        }
        QSqlDatabase::removeDatabase("fake-fdm");
        {
            QFile f(dir.filePath("a.zip"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("12345");
        }
        QString err;
        const QList<FdmImport::Item> items = FdmImport::read(fdm, &err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QCOMPARE(items.size(), 2);
        QCOMPARE(items[0].tags, QStringList{"Work"});
        QCOMPARE(items[0].created, QDateTime::fromSecsSinceEpoch(1791483710));
        QVERIFY(!items[1].finished.isValid());

        Database db;
        QVERIFY(db.open(dir.filePath("profile")));
        DownloadManager mgr(&db);
        mgr.load();
        const auto [added, skipped] = mgr.importFdm(items);
        QCOMPARE(added, 2);
        QCOMPARE(skipped, 0);
        Download* zip = nullptr;
        Download* mag = nullptr;
        for (Download* d : mgr.downloads())
            (d->url().startsWith("magnet:") ? mag : zip) = d;
        QVERIFY(zip && zip->completed() && zip->total() == 5 && zip->priority() == 2);
        QCOMPARE(QFileInfo(zip->filePath()).fileName(), QStringLiteral("a.zip"));
        QVERIFY(mag && mag->paused() && mag->isTorrent());
        QCOMPARE(mgr.tags().size(), 1);
        QVERIFY(zip->hasTag(mgr.tags().first().id));
        QCOMPARE(mgr.tags().first().color, QStringLiteral("#e5534b"));
        QCOMPARE(mgr.importFdm(items).second, 2); // a second import adds nothing
        QCOMPARE(mgr.downloads().size(), 2);

        QVERIFY(!FdmImport::read(dir.filePath("nope.sqlite"), &err).size() && !err.isEmpty());
        // The real FDM list on this PC, if there is one: read through a copy, nothing printed.
        if (QFile::exists(FdmImport::defaultDatabase())) {
            err.clear();
            FdmImport::read(FdmImport::defaultDatabase(), &err);
            QVERIFY2(err.isEmpty(), qPrintable(err));
        }
    }

    void antivirusAndMarkOfTheWeb()
    {
#ifndef Q_OS_WIN
        QSKIP("Windows only");
#else
        QTemporaryDir dir;
        const QString path = dir.filePath("notes.txt");
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("plain text, nothing to see here\n");
        }
        QString msg;
        const auto r = AttachmentCheck::check(path, "https://downloads.example.com/notes.txt", "https://example.com/page", &msg);
        QVERIFY2(r == AttachmentCheck::Clean, qPrintable(msg));
        // The Mark of the Web: an NTFS stream saying "from the internet, from this address".
        QFile zone(path + ":Zone.Identifier");
        QVERIFY(zone.open(QIODevice::ReadOnly));
        const QByteArray z = zone.readAll();
        QVERIFY2(z.contains("ZoneId=3") && z.contains("downloads.example.com"), z.constData());
        QCOMPARE(AttachmentCheck::check(dir.filePath("missing.bin"), {}, {}, &msg), AttachmentCheck::Unavailable);

        // After a download finishes, the manager runs the check and logs it.
        RangeServer srv;
        srv.data = makeData(100 * 1024);
        QVERIFY(srv.start());
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();
        QVERIFY(mgr.antivirusCheck()); // on by default
        Download* d = mgr.add(srv.url("/report.txt"), dir.path(), {}, true);
        QTRY_VERIFY_WITH_TIMEOUT(d->log().join('\n').contains("antivirus"), 30000);
        QVERIFY(d->completed());
#endif
    }

    void updater()
    {
        QVERIFY(Updater::compareVersions("0.2.0", "0.1.9") > 0);
        QVERIFY(Updater::compareVersions("1.10", "1.9") > 0); // numbers, not text
        QCOMPARE(Updater::compareVersions("1.0", "1.0.0"), 0);
        QVERIFY(Updater::compareVersions("0.1.0", "0.1.1") < 0);
        QVERIFY(Updater::allowedUrl(QUrl("https://example.org/latest.json")));
        QVERIFY(!Updater::allowedUrl(QUrl("http://example.org/latest.json")));
        QVERIFY(Updater::allowedUrl(QUrl("http://127.0.0.1:8080/latest.json")));

        const QByteArray installer = makeData(200 * 1024);
        const QString sha = QString::fromLatin1(QCryptographicHash::hash(installer, QCryptographicHash::Sha256).toHex());
        HlsServer srv;
        srv.files.insert("/rel/latest.json", QJsonDocument(QJsonObject{{"version", "9.9.0"}, {"url", "FluxDM-9.9.0-setup.exe"},
                                                                         {"sha256", sha}, {"notes", "Faster"}}).toJson());
        srv.files.insert("/rel/FluxDM-9.9.0-setup.exe", installer);
        srv.files.insert("/old/latest.json", QJsonDocument(QJsonObject{{"version", "0.0.1"}, {"url", "x.exe"}, {"sha256", sha}}).toJson());
        srv.files.insert("/bad/latest.json", QJsonDocument(QJsonObject{{"version", "9.9.0"}, {"url", "evil.exe"}, {"sha256", sha}}).toJson());
        srv.files.insert("/bad/evil.exe", makeData(1000));
        srv.files.insert("/broken/latest.json", "{\"version\": \"9.9.0\"}");
        QVERIFY(srv.start());

        Updater u("0.1.0");
        QSignalSpy checked(&u, &Updater::checked), ready(&u, &Updater::ready), failed(&u, &Updater::failed);
        u.check(QUrl(srv.url("/rel/latest.json")));
        QVERIFY(checked.wait(5000));
        QCOMPARE(checked.last().at(0).toBool(), true);
        QCOMPARE(checked.last().at(1).toString(), QStringLiteral("9.9.0"));
        QCOMPARE(u.notes(), QStringLiteral("Faster"));
        u.download(); // the installer link is relative to the manifest
        QVERIFY(ready.wait(5000));
        QVERIFY(readAll(ready.last().at(0).toString()) == installer);
        QFile::remove(ready.last().at(0).toString());

        u.check(QUrl(srv.url("/old/latest.json")));
        QVERIFY(checked.wait(5000));
        QCOMPARE(checked.last().at(0).toBool(), false); // already newer

        u.check(QUrl(srv.url("/bad/latest.json")));
        QVERIFY(checked.wait(5000));
        u.download();
        QVERIFY(failed.wait(5000));
        QVERIFY(failed.last().at(0).toString().contains("checksum"));
        QVERIFY(!QFile::exists(QDir(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath("FluxDM-update"))
                                    .filePath("FluxDM-9.9.0-setup.exe"))); // nothing left to run

        u.check(QUrl(srv.url("/broken/latest.json")));
        QVERIFY(failed.wait(5000));
        QVERIFY(failed.last().at(0).toString().contains("incomplete"));
        u.check(QUrl("http://updates.example.org/latest.json"));
        QVERIFY(failed.last().at(0).toString().contains("HTTPS"));
    }

    void signedUpdates()
    {
        QByteArray priv, pub;
        QVERIFY(ReleaseSignature::generateKey(&priv, &pub));
        QCOMPARE(ReleaseSignature::publicKeyOf(priv), pub);
        const QByteArray installer = makeData(64 * 1024);
        const QByteArray sha = QCryptographicHash::hash(installer, QCryptographicHash::Sha256).toHex();
        const QByteArray sig = ReleaseSignature::sign(priv, ReleaseSignature::message("9.9.1", sha));
        QVERIFY(ReleaseSignature::verify(pub, ReleaseSignature::message("9.9.1", sha), sig));
        QVERIFY(!ReleaseSignature::verify(pub, ReleaseSignature::message("9.9.2", sha), sig)); // other version
        QByteArray other, otherPub;
        QVERIFY(ReleaseSignature::generateKey(&other, &otherPub));
        QVERIFY(!ReleaseSignature::verify(otherPub, ReleaseSignature::message("9.9.1", sha), sig)); // other key

        HlsServer srv;
        auto manifest = [&](const QByteArray& signature) {
            return QJsonDocument(QJsonObject{{"version", "9.9.1"}, {"url", "setup.exe"}, {"sha256", QString::fromLatin1(sha)},
                                             {"signature", QString::fromLatin1(signature)}}).toJson();
        };
        srv.files.insert("/good/latest.json", manifest(sig));
        srv.files.insert("/good/setup.exe", installer);
        srv.files.insert("/unsigned/latest.json", manifest({}));
        srv.files.insert("/forged/latest.json", manifest(ReleaseSignature::sign(other, ReleaseSignature::message("9.9.1", sha))));
        QVERIFY(srv.start());

        Updater u("0.1.0");
        u.setPublicKey(pub);
        QSignalSpy checked(&u, &Updater::checked), ready(&u, &Updater::ready), failed(&u, &Updater::failed);
        u.check(QUrl(srv.url("/unsigned/latest.json")));
        QVERIFY(failed.wait(5000));
        QVERIFY(failed.last().at(0).toString().contains("signed"));
        u.check(QUrl(srv.url("/forged/latest.json")));
        QVERIFY(failed.wait(5000));
        QCOMPARE(checked.count(), 0);
        u.check(QUrl(srv.url("/good/latest.json")));
        QVERIFY(checked.wait(5000));
        QVERIFY(checked.last().at(0).toBool());
        u.download();
        QVERIFY(ready.wait(5000));
        const QString first = ready.last().at(0).toString();
        QVERIFY(QFileInfo(first).absolutePath().contains("FluxDM-update-")); // its own new folder
        u.download();
        QVERIFY(ready.wait(5000));
        QVERIFY(QFileInfo(ready.last().at(0).toString()).absolutePath() != QFileInfo(first).absolutePath());
        for (const QVariantList& r : ready)
            QDir(QFileInfo(r.at(0).toString()).absolutePath()).removeRecursively();
    }

    void remoteControl()
    {
        QVERIFY(RemoteServer::isLocalNetwork(QHostAddress("192.168.1.5")));
        QVERIFY(RemoteServer::isLocalNetwork(QHostAddress("10.20.30.40")));
        QVERIFY(RemoteServer::isLocalNetwork(QHostAddress("172.20.0.1")));
        QVERIFY(RemoteServer::isLocalNetwork(QHostAddress("::ffff:192.168.0.9")));
        QVERIFY(RemoteServer::isLocalNetwork(QHostAddress("fe80::1")));
        QVERIFY(RemoteServer::isLocalNetwork(QHostAddress::LocalHost));
        QVERIFY(!RemoteServer::isLocalNetwork(QHostAddress("8.8.8.8")));
        QVERIFY(!RemoteServer::isLocalNetwork(QHostAddress("172.32.0.1")));
        QVERIFY(!RemoteServer::isLocalNetwork(QHostAddress("2001:db8::1")));
        QCOMPARE(RemoteServer::newAccessCode().size(), 6);

        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadRecord r;
        r.id = 1;
        r.title = QStringLiteral("<b>report</b>.pdf"); // titles are data, never markup
        r.state = Download::Paused;
        db.save(r);
        DownloadManager mgr(&db);
        mgr.load();
        RemoteServer srv(&mgr);
        srv.setAccessCode("123456");
        QString added;
        srv.setAddHandler([&](const QString& url) { added = url; return QString(); });
        QVERIFY(srv.start(0, QHostAddress::LocalHost));

        QNetworkAccessManager nam;
        auto call = [&](const QString& path, const QJsonObject& body, const QByteArray& token, int* code) {
            QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(srv.port()).arg(path)));
            if (!token.isEmpty())
                req.setRawHeader("X-FluxDM-Token", token);
            QNetworkReply* rep;
            if (body.isEmpty() && path != "/api/login" && !path.startsWith("/api/") ) {
                rep = nam.get(req);
            } else if (body.isEmpty() && path == "/api/downloads") {
                rep = nam.get(req);
            } else {
                req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
                rep = nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
            }
            QSignalSpy done(rep, &QNetworkReply::finished);
            done.wait(5000);
            *code = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray data = rep->readAll();
            rep->deleteLater();
            return data;
        };
        int code = 0;
        QByteArray page = call("/", {}, {}, &code);
        QCOMPARE(code, 200);
        QVERIFY(page.contains("FluxDM remote"));
        call("/api/downloads", {}, {}, &code);
        QCOMPARE(code, 401); // not signed in
        call("/api/login", {{"code", "000000"}}, {}, &code);
        QCOMPARE(code, 403);
        const QByteArray token = QJsonDocument::fromJson(call("/api/login", {{"code", "123456"}}, {}, &code)).object().value("token").toString().toLatin1();
        QCOMPARE(code, 200);
        QCOMPARE(token.size(), 64);

        const QJsonObject list = QJsonDocument::fromJson(call("/api/downloads", {}, token, &code)).object();
        QCOMPARE(code, 200);
        QCOMPARE(list.value("downloads").toArray().first().toObject().value("title").toString(), QStringLiteral("<b>report</b>.pdf"));
        call("/api/add", {{"url", "https://example.com/file.zip"}}, token, &code);
        QCOMPARE(code, 200);
        QCOMPARE(added, QStringLiteral("https://example.com/file.zip"));
        call("/api/mode", {{"mode", 0}}, token, &code);
        QCOMPARE(code, 200);
        QCOMPARE(mgr.trafficMode(), 0);
        call("/api/pause", {{"id", 1}}, token, &code);
        QCOMPARE(code, 200);
        call("/api/nope", {{"x", 1}}, token, &code);
        QCOMPARE(code, 404);
        call("/api/logout", {{"x", 1}}, token, &code);
        call("/api/downloads", {}, token, &code);
        QCOMPARE(code, 401); // signed out

        // A new access code signs everyone out; five wrong codes lock that address out.
        const QByteArray token2 = QJsonDocument::fromJson(call("/api/login", {{"code", "123456"}}, {}, &code)).object().value("token").toString().toLatin1();
        srv.setAccessCode("654321");
        call("/api/downloads", {}, token2, &code);
        QCOMPARE(code, 401);
        for (int i = 0; i < 5; ++i)
            call("/api/login", {{"code", "111111"}}, {}, &code);
        call("/api/login", {{"code", "654321"}}, {}, &code);
        QCOMPARE(code, 429);
    }

    void instanceChannel()
    {
        InstanceServer srv;
        QVERIFY(srv.socketOptions().testFlag(QLocalServer::UserAccessOption));
        const QString name = QStringLiteral("FluxDM-test-%1").arg(QCoreApplication::applicationPid());
        QVERIFY(srv.listen(name));
        QVariantMap got;
        QStringList args;
        srv.setRequestHandler([&](const QVariantMap& r) { got = r; return QString(); });
        srv.setArgsHandler([&](const QStringList& a) { args = a; });

        auto send = [&](const QByteArray& data) {
            QLocalSocket s;
            s.connectToServer(name);
            if (!s.waitForConnected(2000))
                return QByteArray("no connection");
            s.write(data);
            s.waitForBytesWritten(2000);
            QByteArray reply;
            QElapsedTimer t;
            t.start();
            while (s.state() == QLocalSocket::ConnectedState && t.elapsed() < 3000) {
                QCoreApplication::processEvents();
                s.waitForReadyRead(20);
                reply += s.readAll();
            }
            return reply + s.readAll();
        };
        QVERIFY(send("{\"cmd\":\"ping\"}\n").contains("\"ok\":true"));
        QCOMPARE(got.value("cmd").toString(), QStringLiteral("ping"));
        send("[\"--silent\",\"https://example.com/a.zip\"]\n");
        QTRY_COMPARE(args, (QStringList{"--silent", "https://example.com/a.zip"}));

        // A huge message without a line end is dropped instead of filling memory.
        got.clear();
        QByteArray junk(InstanceServer::kMaxLine + 4096, 'x');
        const QByteArray r = send(junk);
        QVERIFY(r.isEmpty());
        QVERIFY(got.isEmpty());
    }

    void remoteControlHardening()
    {
        QVERIFY(RemoteServer::isAllowedHost("192.168.1.5:8080"));
        QVERIFY(RemoteServer::isAllowedHost("[fe80::1]:8080"));
        QVERIFY(RemoteServer::isAllowedHost("localhost:1234"));
        QVERIFY(RemoteServer::isAllowedHost(QSysInfo::machineHostName().toLatin1() + ".fritz.box"));
        QVERIFY(!RemoteServer::isAllowedHost("attacker.example:8080"));
        QVERIFY(!RemoteServer::isAllowedHost(""));

        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadManager mgr(&db);
        mgr.load();
        RemoteServer srv(&mgr);
        srv.setAccessCode("123456");
        srv.setIdleTimeoutMs(300);
        QStringList added;
        srv.setAddHandler([&](const QString& url) { added << url; return QString(); });
        QVERIFY(srv.start(0, QHostAddress::LocalHost));

        auto raw = [&](const QByteArray& request) {
            QTcpSocket s;
            s.connectToHost(QHostAddress::LocalHost, srv.port());
            if (!s.waitForConnected(3000))
                return QByteArray("no connection");
            s.write(request);
            QByteArray got;
            QElapsedTimer t;
            t.start();
            while (s.state() == QAbstractSocket::ConnectedState && t.elapsed() < 3000) {
                QCoreApplication::processEvents();
                s.waitForReadyRead(20);
                got += s.readAll();
            }
            return got + s.readAll();
        };
        // DNS rebinding: a page on attacker.example that now points at this computer.
        QVERIFY(raw("GET / HTTP/1.1\r\nHost: attacker.example\r\n\r\n").startsWith("HTTP/1.1 403"));
        QVERIFY(raw("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").startsWith("HTTP/1.1 200"));
        // Malformed lengths.
        QVERIFY(raw("POST /api/login HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: -5\r\n\r\n").startsWith("HTTP/1.1 400"));
        QVERIFY(raw("POST /api/login HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 99999999\r\n\r\n").startsWith("HTTP/1.1 413"));
        // A client that never finishes its request is dropped.
        {
            QTcpSocket s;
            s.connectToHost(QHostAddress::LocalHost, srv.port());
            QVERIFY(s.waitForConnected(3000));
            s.write("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n"); // no blank line
            QTRY_VERIFY_WITH_TIMEOUT(srv.openConnections() == 1, 2000);
            QTRY_COMPARE_WITH_TIMEOUT(srv.openConnections(), 0, 3000);
        }

        // Only web and magnet links can be added remotely.
        const QByteArray login = R"({"code":"123456"})";
        const QByteArray resp = raw("POST /api/login HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " + QByteArray::number(login.size()) + "\r\n\r\n" + login);
        const QByteArray token = QJsonDocument::fromJson(resp.mid(resp.indexOf("\r\n\r\n") + 4)).object().value("token").toString().toLatin1();
        QCOMPARE(token.size(), 64);
        auto add = [&](const QString& url) {
            const QByteArray body = QJsonDocument(QJsonObject{{"url", url}}).toJson(QJsonDocument::Compact);
            return raw("POST /api/add HTTP/1.1\r\nHost: 127.0.0.1\r\nX-FluxDM-Token: " + token + "\r\nContent-Length: "
                       + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        };
        QVERIFY(add("C:\\Users\\me\\Downloads\\x.torrent").startsWith("HTTP/1.1 400"));
        QVERIFY(add("file:///C:/x.torrent").startsWith("HTTP/1.1 400"));
        QVERIFY(added.isEmpty());
        QVERIFY(add("magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567").startsWith("HTTP/1.1 200"));
        QCOMPARE(added.size(), 1);
    }

    void siteLinksAndYtdlpOutput()
    {
        QVERIFY(SiteEngine::isSupportedSite(QUrl("https://www.youtube.com/watch?v=abc")));
        QVERIFY(SiteEngine::isSupportedSite(QUrl("https://youtu.be/abc")));
        QVERIFY(SiteEngine::isSupportedSite(QUrl("https://player.vimeo.com/video/1")));
        QVERIFY(!SiteEngine::isSupportedSite(QUrl("https://notyoutube.com/watch")));       // not a subdomain
        QVERIFY(!SiteEngine::isSupportedSite(QUrl("https://cdn.vimeo.com/files/clip.mp4"))); // a file: plain HTTP
        QVERIFY(!SiteEngine::isSupportedSite(QUrl("ftp://youtube.com/x")));

        auto l = SiteEngine::parse("FLUXDM-PROGRESS 1024 4096 NA 2048.5");
        QCOMPARE(l.kind, SiteEngine::Line::Progress);
        QCOMPARE(l.done, qint64(1024));
        QCOMPARE(l.total, qint64(4096));
        QCOMPARE(l.speed, 2048.5);
        l = SiteEngine::parse("FLUXDM-PROGRESS 10 NA 900.0 None"); // only an estimate
        QCOMPARE(l.total, qint64(900));
        QCOMPARE(SiteEngine::parse("[download] Destination: C:\\v\\A [x].f137.mp4").path, QStringLiteral("C:\\v\\A [x].f137.mp4"));
        QCOMPARE(SiteEngine::parse("[Merger] Merging formats into \"C:\\v\\A [x].mp4\"").path, QStringLiteral("C:\\v\\A [x].mp4"));
        QCOMPARE(SiteEngine::parse("[download] C:\\v\\A.mp4 has already been downloaded").kind, SiteEngine::Line::AlreadyThere);
        l = SiteEngine::parse("ERROR: [youtube] abc123: Video unavailable. This video is private");
        QCOMPARE(l.kind, SiteEngine::Line::Error);
        QCOMPARE(l.text, QStringLiteral("Video unavailable. This video is private"));

        TaskParams p = params(1, "https://youtu.be/abc", "C:/dl");
        p.ffmpeg = "C:/tools/ffmpeg.exe";
        p.proxy = "socks5h://127.0.0.1:1080";
        const QString args = SiteEngine::arguments(p, 4, 1000000).join(' ');
        QVERIFY(args.contains("-f bv*+ba/b") && args.contains("--ffmpeg-location"));
        QVERIFY(args.contains("--limit-rate 1000000") && args.contains("--proxy socks5h://127.0.0.1:1080") && args.contains("-N 4"));
        QVERIFY(args.endsWith("-- https://youtu.be/abc"));
        p.ffmpeg.clear();
        QVERIFY(SiteEngine::arguments(p, 4, 0).join(' ').contains("-f b ")); // no merging without ffmpeg
    }

    void siteDownloadThroughYtdlp()
    {
        const QString python = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("Programs/Python/Python312/python.exe");
        const QString script = QFINDTESTDATA("fake_ytdlp.py");
        if (!QFileInfo(python).isExecutable() || script.isEmpty())
            QSKIP("needs Python for the yt-dlp stand-in");
        QTemporaryDir dir;
        SiteEngine eng;
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        QSignalSpy stats(&eng, &DownloadEngine::stats), meta(&eng, &DownloadEngine::meta), stopped(&eng, &DownloadEngine::stopped);

        TaskParams p = params(51, "https://www.youtube.com/watch?v=ok", dir.path());
        p.siteTool = {python, "-I", script};
        p.ffmpeg = "ffmpeg.exe"; // only passed through to the stand-in
        eng.start(p);
        Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QCOMPARE(QFileInfo(o.path).fileName(), QStringLiteral("Test Clip [ok].mp4"));
        QCOMPARE(QFileInfo(o.path).size(), qint64(350000));
        QCOMPARE(meta.first().at(1).toString(), QStringLiteral("Test Clip [ok].mp4")); // ".f137" removed right away
        // Progress counts the video stream, then adds the audio stream on top.
        qint64 maxReceived = 0;
        for (const auto& s : stats)
            maxReceived = qMax(maxReceived, s.at(0).value<TaskStats>().received);
        QCOMPARE(maxReceived, qint64(350000));

        fin.clear();
        fail.clear();
        p.id = 52;
        p.url = "https://youtu.be/?v=gone";
        eng.start(p);
        o = waitForEnd(fin, fail, 30000);
        QVERIFY(o.failed);
        QCOMPARE(o.error, QStringLiteral("Video unavailable"));

        fail.clear();
        p.id = 53;
        p.siteTool.clear(); // yt-dlp missing
        eng.start(p);
        o = waitForEnd(fin, fail, 5000);
        QVERIFY(o.failed && o.error.contains("yt-dlp"));

        // Stop half-way, start again: the .part file is continued, not restarted.
        fin.clear();
        fail.clear();
        stats.clear();
        p.id = 54;
        p.url = "https://www.youtube.com/watch?v=slow";
        p.siteTool = {python, "-I", script};
        eng.start(p);
        QTRY_VERIFY_WITH_TIMEOUT(!stats.isEmpty() && stats.last().at(0).value<TaskStats>().received > 100000, 20000);
        eng.stop(54);
        QTRY_VERIFY_WITH_TIMEOUT(!stopped.isEmpty(), 10000);
        const qint64 partSize = QFileInfo(dir.filePath("Test Clip [slow].mp4.part")).size();
        QVERIFY(partSize > 100000 && partSize < 400000);
        stats.clear();
        eng.start(p);
        o = waitForEnd(fin, fail, 30000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QCOMPARE(QFileInfo(o.path).size(), qint64(400000));
        QVERIFY(stats.first().at(0).value<TaskStats>().received > partSize - 20000); // picked up where it was
    }

    void appLogAndCrashReports()
    {
        QTemporaryDir dir;
        const QString logs = dir.filePath("logs");
        AppLog::init(logs);
        QVERIFY(!AppLog::previousSessionCrashed()); // first run
        qWarning("disk is full on purpose");       // Qt messages land in the log
        {
            DownloadRecord r;
            r.id = 77;
            Download d(r);
            d.addLog("Connecting to example.com"); // and so do download events
        }
        QStringList t = AppLog::tail(10);
        QVERIFY2(t.join('\n').contains("[warning] disk is full on purpose"), qPrintable(t.join('\n')));
        QVERIFY(t.last().endsWith("#77 Connecting to example.com"));

        AppLog::init(logs); // started again without a clean shutdown: like after a crash
        QVERIFY(AppLog::previousSessionCrashed());
        AppLog::shutdown();
        AppLog::init(logs);
        QVERIFY(!AppLog::previousSessionCrashed()); // a clean exit isn't reported

        // Rotation at 1 MB keeps the log small; tail() reads across files.
        const QString filler(200, QLatin1Char('x'));
        for (int i = 0; i < 6000; ++i)
            AppLog::write(filler);
        AppLog::write("the newest line");
        QVERIFY(QFile::exists(QDir(logs).filePath("fluxdm.1.log")));
        QVERIFY(QFileInfo(AppLog::currentFile()).size() < 1100 * 1024);
        t = AppLog::tail(3);
        QCOMPARE(t.size(), 3);
        QVERIFY(t.last().endsWith("the newest line"));

#ifdef Q_OS_WIN
        const QString dump = QDir(logs).filePath("crash-test.dmp");
        QVERIFY(AppLog::writeMiniDump(dump));
        QVERIFY(QFileInfo(dump).size() > 10000);
        QCOMPARE(AppLog::crashDumps().first(), QFileInfo(dump).absoluteFilePath());
#endif
        const QString report = AppLog::report(5);
        QVERIFY(report.contains("OS: ") && report.contains("Qt: ") && report.contains("the newest line"));
#ifdef Q_OS_WIN
        QVERIFY(report.contains("crash-test.dmp"));
#endif
        AppLog::shutdown();
    }

    void manualQueueOrder()
    {
        QTemporaryDir dir;
        {
            Database db;
            QVERIFY(db.open(dir.path()));
            for (int i = 1; i <= 4; ++i) {
                DownloadRecord r;
                r.id = i;
                r.title = QStringLiteral("f%1.bin").arg(i);
                r.state = Download::Paused;
                db.save(r);
            }
            DownloadRecord done;
            done.id = 5;
            done.state = Download::Completed;
            db.save(done);
            DownloadManager mgr(&db);
            mgr.load();
            auto ids = [&] {
                QList<qint64> l;
                for (Download* d : mgr.queueOrder())
                    l << d->id();
                return l;
            };
            QCOMPARE(ids(), (QList<qint64>{1, 2, 3, 4})); // first come, first served; finished ones aren't queued
            mgr.moveInQueue({mgr.find(3)}, "top");
            QCOMPARE(ids(), (QList<qint64>{3, 1, 2, 4}));
            mgr.moveInQueue({mgr.find(3)}, "up"); // already first
            QCOMPARE(ids(), (QList<qint64>{3, 1, 2, 4}));
            mgr.moveInQueue({mgr.find(1)}, "down");
            QCOMPARE(ids(), (QList<qint64>{3, 2, 1, 4}));
            mgr.moveInQueue({mgr.find(4), mgr.find(2)}, "top"); // a group keeps its relative order
            QCOMPARE(ids(), (QList<qint64>{2, 4, 3, 1}));
            mgr.moveInQueue({mgr.find(2), mgr.find(4)}, "down");
            QCOMPARE(ids(), (QList<qint64>{3, 2, 4, 1}));
            mgr.moveInQueue({mgr.find(3)}, "bottom");
            QCOMPARE(ids(), (QList<qint64>{2, 4, 1, 3}));
        }
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadManager mgr(&db);
        mgr.load();
        QList<qint64> again;
        for (Download* d : mgr.queueOrder())
            again << d->id();
        QCOMPARE(again, (QList<qint64>{2, 4, 1, 3})); // saved

        DownloadsModel model(&mgr);
        DownloadsFilterModel proxy;
        proxy.setSourceModel(&model);
        proxy.setSortOrder("queue", false);
        QCOMPARE(proxy.idAt(0), qint64(2));
        QCOMPARE(proxy.idAt(4), qint64(5)); // finished ones last
    }

    void queueOrderDecidesWhatStarts()
    {
        RangeServer srv;
        srv.data = makeData(2 * 1024 * 1024);
        srv.chunk = 8 * 1024; // slow: the first download is still running when we look
        QVERIFY(srv.start());
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();
        for (int mode = 0; mode < 3; ++mode) {
            TrafficMode::Limits l = mgr.limits(mode);
            l.maxDownloads = 1;
            mgr.setLimits(mode, l);
        }
        Download* busy = mgr.add(srv.url("/busy.bin"), dir.path(), {}, true); // takes the only slot
        QTRY_VERIFY_WITH_TIMEOUT(busy->running(), 5000);
        Download* a = mgr.add(srv.url("/a.bin"), dir.path(), {}, true);
        Download* b = mgr.add(srv.url("/b.bin"), dir.path(), {}, true);
        Download* c = mgr.add(srv.url("/c.bin"), dir.path(), {}, true);
        QVERIFY(a->queued() && b->queued() && c->queued());
        mgr.moveInQueue({c}, "top");
        mgr.pause(busy); // the slot frees up: the first in the queue gets it
        QTRY_VERIFY_WITH_TIMEOUT(c->running(), 5000);
        QVERIFY(a->queued() && b->queued());
    }

    void userTags()
    {
        QCOMPARE(Tag::parseExtensions(" ISO, .img;*.Zip  iso "), (QStringList{"iso", "img", "zip"}));
        QTemporaryDir dir;
        int isoTag = 0, workTag = 0;
        {
            Database db;
            QVERIFY(db.open(dir.path()));
            DownloadManager mgr(&db);
            mgr.load();
            Tag iso;
            iso.name = QStringLiteral("Linux ISOs");
            iso.color = QStringLiteral("#57ab5a");
            iso.extensions = Tag::parseExtensions("iso img");
            iso.folder = QDir::toNativeSeparators(dir.filePath("isos"));
            isoTag = mgr.saveTag(iso);
            Tag work;
            work.name = QStringLiteral("Work");
            workTag = mgr.saveTag(work);
            QVERIFY(isoTag > 0 && workTag > 0 && isoTag != workTag);
            QCOMPARE(mgr.saveTag(Tag{}), 0); // a name is required

            QCOMPARE(mgr.tagsFor("debian-12.ISO"), QList<int>{isoTag});
            QCOMPARE(mgr.tagFolderFor("debian-12.iso"), iso.folder);
            QVERIFY(mgr.tagFolderFor("notes.txt").isEmpty());

            // Without engines add() can't create downloads; tag records directly instead.
            DownloadRecord r;
            r.id = 1;
            r.title = QStringLiteral("report.pdf");
            r.tags = {workTag};
            db.save(r);
        }
        {
            Database db;
            QVERIFY(db.open(dir.path()));
            const QList<Tag> tags = db.loadTags();
            QCOMPARE(tags.size(), 2);
            QCOMPARE(tags.first().extensions, (QStringList{"iso", "img"}));
            QCOMPARE(tags.first().color, QStringLiteral("#57ab5a"));
            DownloadManager mgr(&db);
            mgr.load();
            Download* d = mgr.find(1);
            QVERIFY(d && d->hasTag(workTag));

            DownloadsModel model(&mgr);
            DownloadsFilterModel proxy;
            proxy.setSourceModel(&model);
            proxy.setFilter(QStringLiteral("tag:%1").arg(workTag));
            QCOMPARE(proxy.rowCount(), 1);
            proxy.setFilter(QStringLiteral("tag:%1").arg(isoTag));
            QCOMPARE(proxy.rowCount(), 0);
            mgr.setTag(d, isoTag, true);
            QCOMPARE(proxy.rowCount(), 1); // the filter follows tag changes

            QSignalSpy changed(&mgr, &DownloadManager::tagsChanged);
            mgr.removeTag(workTag); // also leaves every download
            QCOMPARE(changed.count(), 1);
            QCOMPARE(d->rec.tags, QList<int>{isoTag});
        }
        Database db;
        QVERIFY(db.open(dir.path()));
        QCOMPARE(db.loadTags().size(), 1);
        QCOMPARE(db.loadDownloads().first().tags, QList<int>{isoTag});
    }

    void tagsAssignedOnAddAndMeta()
    {
        RangeServer srv;
        srv.data = makeData(64 * 1024);
        srv.contentDisposition = "attachment; filename=\"ubuntu-24.04.iso\""; // the link doesn't say "iso"
        QVERIFY(srv.start());
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();
        Tag iso;
        iso.name = QStringLiteral("ISOs");
        iso.extensions = {QStringLiteral("iso")};
        const int isoTag = mgr.saveTag(iso);
        Download* byLink = mgr.add(srv.url("/debian.iso"), dir.path(), {}, false);
        QVERIFY(byLink->hasTag(isoTag)); // from the link's name, right away
        Download* byServer = mgr.add(srv.url("/get?id=7"), dir.path(), {}, true);
        QVERIFY(!byServer->hasTag(isoTag));
        QTRY_VERIFY_WITH_TIMEOUT(byServer->completed(), 20000);
        QVERIFY(byServer->hasTag(isoTag)); // once the server named it
    }

    void sortOrders()
    {
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.path()));
        auto rec = [&](qint64 id, const char* title, qint64 total, int state) {
            DownloadRecord r;
            r.id = id;
            r.url = QStringLiteral("https://example.com/%1").arg(title);
            r.destDir = dir.path();
            r.title = QString::fromLatin1(title);
            r.total = total;
            r.state = state;
            db.save(r);
        };
        rec(1, "banana.zip", 500, Download::Completed);
        rec(2, "apple.iso", 9000, Download::Paused);
        rec(3, "cherry.mp4", 500, Download::Error);
        rec(4, "Date.pdf", 10, Download::Completed);
        DownloadManager mgr(&db);
        mgr.load();
        DownloadsModel model(&mgr);
        DownloadsFilterModel proxy;
        proxy.setSourceModel(&model);
        auto order = [&] {
            QList<qint64> ids;
            for (int r = 0; r < proxy.rowCount(); ++r)
                ids << proxy.idAt(r);
            return ids;
        };
        proxy.setSortOrder("added", true);
        QCOMPARE(order(), (QList<qint64>{4, 3, 2, 1}));
        proxy.setSortOrder("added", false);
        QCOMPARE(order(), (QList<qint64>{1, 2, 3, 4}));
        proxy.setSortOrder("name", false); // case-insensitive, locale aware
        QCOMPARE(order(), (QList<qint64>{2, 1, 3, 4}));
        proxy.setSortOrder("name", true);
        QCOMPARE(order(), (QList<qint64>{4, 3, 1, 2}));
        proxy.setSortOrder("size", true); // equal sizes: newest first
        QCOMPARE(order(), (QList<qint64>{2, 3, 1, 4}));
        proxy.setSortOrder("size", false);
        QCOMPARE(order(), (QList<qint64>{4, 3, 1, 2}));
        proxy.setSortOrder("status", false); // paused, error, then completed (newest first)
        QCOMPARE(order(), (QList<qint64>{2, 3, 4, 1}));
        proxy.setSortOrder("bogus", false); // unknown key -> date added
        QCOMPARE(proxy.sortKey(), QStringLiteral("added"));

        // Filters still apply on top of the order.
        proxy.setSortOrder("size", true);
        proxy.setFilter("completed");
        QCOMPARE(order(), (QList<qint64>{1, 4}));
    }

    void sleepGuard()
    {
        SleepGuard g;
        QVERIFY(!g.active());
        QVERIFY(g.setActive(true));
        QVERIFY(g.active());
        QVERIFY(g.setActive(true)); // idempotent
        QVERIFY(g.setActive(false));
        QVERIFY(!g.active());
        {
            SleepGuard scoped;
            QVERIFY(scoped.setActive(true));
        } // released by the destructor
    }

    void existingFileRules()
    {
        QTemporaryDir dir;
        auto put = [&](const QString& name, const QByteArray& data) {
            QFile f(QDir(dir.path()).filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(data);
        };
        put(QStringLiteral("a.zip"), "old");
        put(QStringLiteral("b.zip.fluxpart"), "partial"); // another download is writing b.zip
        const QString d = dir.path();
        bool conflict = true;
        QCOMPARE(QFileInfo(FileNamer::resolveTarget(d, "new.zip", FileNamer::Ask, &conflict)).fileName(), QStringLiteral("new.zip"));
        QVERIFY(!conflict);
        QCOMPARE(QFileInfo(FileNamer::resolveTarget(d, "a.zip", FileNamer::KeepBoth, &conflict)).fileName(), QStringLiteral("a (1).zip"));
        QCOMPARE(QFileInfo(FileNamer::resolveTarget(d, "a.zip", FileNamer::Overwrite, &conflict)).fileName(), QStringLiteral("a.zip"));
        QVERIFY(!conflict);
        FileNamer::resolveTarget(d, "a.zip", FileNamer::Ask, &conflict);
        QVERIFY(conflict);
        // Never write into another download's part file, whatever the setting.
        QCOMPARE(QFileInfo(FileNamer::resolveTarget(d, "b.zip", FileNamer::Overwrite, &conflict)).fileName(), QStringLiteral("b (1).zip"));
        QVERIFY(!conflict);

        const QString a = QDir(d).filePath("a.zip");
        QCOMPARE(QFileInfo(FileNamer::completionTarget(a, FileNamer::KeepBoth)).fileName(), QStringLiteral("a (1).zip"));
        QVERIFY(QFile::exists(a));
        QCOMPARE(QFileInfo(FileNamer::completionTarget(a, FileNamer::Ask)).fileName(), QStringLiteral("a (1).zip")); // unanswered: never deletes
        QVERIFY(QFile::exists(a));
        QCOMPARE(FileNamer::completionTarget(a, FileNamer::Overwrite), a);
        QVERIFY(!QFile::exists(a)); // removed so the finished file can take its place
    }

    void existingFileInEngine()
    {
        RangeServer srv;
        srv.data = makeData(400 * 1024);
        QVERIFY(srv.start());
        QTemporaryDir dir;
        const QString existing = QDir(dir.path()).filePath("report.bin");
        auto writeOld = [&] {
            QFile f(existing);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("old contents");
        };
        writeOld();
        WwwEngine eng;
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);

        TaskParams p = params(11, srv.url("/report.bin"), dir.path());
        p.existingFile = FileNamer::Ask;
        eng.start(p);
        Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY(o.failed);
        QVERIFY2(o.error.startsWith("File already exists"), qPrintable(o.error));
        QVERIFY(!DownloadManager::isRetriable(o.error));
        QCOMPARE(readAll(existing), QByteArray("old contents"));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 1); // nothing else created

        fail.clear();
        p.id = 12;
        p.existingFile = FileNamer::KeepBoth;
        eng.start(p);
        o = waitForEnd(fin, fail, 30000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QCOMPARE(QFileInfo(o.path).fileName(), QStringLiteral("report (1).bin"));
        QCOMPARE(readAll(existing), QByteArray("old contents"));

        fin.clear();
        p.id = 13;
        p.existingFile = FileNamer::Overwrite;
        eng.start(p);
        o = waitForEnd(fin, fail, 30000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QCOMPARE(QFileInfo(o.path).fileName(), QStringLiteral("report.bin"));
        QVERIFY(readAll(existing) == srv.data);
    }

    void askThenReplace()
    {
        RangeServer srv;
        srv.data = makeData(200 * 1024);
        QVERIFY(srv.start());
        QTemporaryDir dir;
        {
            QFile f(QDir(dir.path()).filePath("notes.bin"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("keep me?");
        }
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();
        QCOMPARE(mgr.existingFileDefault(), int(FileNamer::Ask)); // the default is "Ask me"
        mgr.setExistingFileDefault(FileNamer::KeepBoth);
        QCOMPARE(mgr.existingFileDefault(), int(FileNamer::KeepBoth));
        mgr.setExistingFileDefault(FileNamer::Ask);
        QCOMPARE(mgr.existingFileDefault(), int(FileNamer::Ask));
        Download* d = mgr.add(srv.url("/notes.bin"), dir.path(), {}, true);
        QCOMPARE(d->rec.existingFileReaction, int(FileNamer::Ask));
        QTRY_VERIFY_WITH_TIMEOUT(d->failed(), 20000);
        QVERIFY(d->fileExists());
        QVERIFY(!d->autoRetryAt.isValid());

        mgr.setExistingFileReaction(d, FileNamer::Overwrite); // the dialog's "Replace"
        mgr.resume(d);
        QTRY_VERIFY_WITH_TIMEOUT(d->completed(), 20000);
        QCOMPARE(QFileInfo(d->filePath()).fileName(), QStringLiteral("notes.bin"));
        QVERIFY(readAll(d->filePath()) == srv.data);
    }

    void httpLogin()
    {
        RangeServer srv;
        srv.data = makeData(600 * 1024);
        srv.basicAuth = "alice:s3cret pass";
        QVERIFY(srv.start());
        QTemporaryDir dir;
        WwwEngine eng;
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);

        TaskParams p = params(8, srv.url("/private/report.bin"), dir.path());
        eng.start(p); // no login
        Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY(o.failed);
        QVERIFY2(o.error.startsWith("HTTP error 401"), qPrintable(o.error));

        fail.clear();
        p.id = 9;
        p.user = QStringLiteral("alice");
        p.password = QStringLiteral("wrong");
        eng.start(p);
        o = waitForEnd(fin, fail, 30000);
        QVERIFY(o.failed);
        QVERIFY2(o.error.startsWith("Login failed"), qPrintable(o.error));
        QVERIFY(!DownloadManager::isRetriable(o.error));

        fail.clear();
        p.id = 10;
        p.password = QStringLiteral("s3cret pass");
        p.maxConnections = 4; // every connection must authenticate
        eng.start(p);
        o = waitForEnd(fin, fail, 30000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QVERIFY(readAll(o.path) == srv.data);
    }

    void browserSecretsAreProtected()
    {
        QCOMPARE(Secret::unprotectBytes(Secret::protectBytes(QByteArray("a=1&b=\xff\x00z", 9))), QByteArray("a=1&b=\xff\x00z", 9));
        QVERIFY(Secret::isProtected(Secret::protectBytes("x")));
        QVERIFY(!Secret::isProtected("sid=abc"));

        QTemporaryDir dir;
        {
            Database db;
            QVERIFY(db.open(dir.path()));
            DownloadRecord r;
            r.id = 1;
            r.url = QStringLiteral("https://example.com/f.zip");
            r.cookies = QStringLiteral("session=SECRET-COOKIE-123");
            r.postData = "token=SECRET-FORM-456";
            QVERIFY(db.save(r));
            DownloadRecord legacy; // written by an older version: plain text
            legacy.id = 2;
            QVERIFY(db.save(legacy));
        }
        {
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("raw2"));
            raw.setDatabaseName(QDir(dir.path()).filePath(QStringLiteral("db.sqlite")));
            QVERIFY(raw.open());
            QSqlQuery q(raw);
            QVERIFY(q.exec(QStringLiteral("SELECT cookies, postData FROM downloads WHERE id = 1")) && q.next());
#ifdef Q_OS_WIN
            QVERIFY(!q.value(0).toByteArray().contains("SECRET-COOKIE"));
            QVERIFY(!q.value(1).toByteArray().contains("SECRET-FORM"));
#endif
            QVERIFY(q.exec(QStringLiteral("UPDATE downloads SET cookies = CAST('old=plain' AS BLOB), postData = CAST('a=b' AS BLOB) WHERE id = 2")));
            q.finish();
            raw.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("raw2"));
        Database db;
        QVERIFY(db.open(dir.path()));
        const auto rows = db.loadDownloads();
        QCOMPARE(rows.size(), 2);
        for (const DownloadRecord& r : rows) {
            if (r.id == 1) {
                QCOMPARE(r.cookies, QStringLiteral("session=SECRET-COOKIE-123"));
                QCOMPARE(r.postData, QByteArray("token=SECRET-FORM-456"));
            } else {
                QCOMPARE(r.cookies, QStringLiteral("old=plain"));
                QCOMPARE(r.postData, QByteArray("a=b"));
            }
        }
    }

    void savedLoginsAreProtected()
    {
        QTemporaryDir dir;
        {
            Database db;
            QVERIFY(db.open(dir.path()));
            db.saveLogin(QStringLiteral("https://files.example.com:443"), QStringLiteral("alice"), QStringLiteral("p@ss-ünïcode"));
            QCOMPARE(db.loginCount(), 1);
        }
        {   // never on disk as plain text (DPAPI on Windows)
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("raw"));
            raw.setDatabaseName(QDir(dir.path()).filePath(QStringLiteral("db.sqlite")));
            QVERIFY(raw.open());
            QSqlQuery q(raw);
            QVERIFY(q.exec(QStringLiteral("SELECT password FROM networkPasswords")) && q.next());
#ifdef Q_OS_WIN
            QVERIFY(!q.value(0).toByteArray().contains("p@ss"));
#endif
            q.finish();
            raw.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("raw"));
        Database db;
        QVERIFY(db.open(dir.path()));
        QString user, pass;
        QVERIFY(db.login(QStringLiteral("https://files.example.com:443"), &user, &pass));
        QCOMPARE(user, QStringLiteral("alice"));
        QCOMPARE(pass, QStringLiteral("p@ss-ünïcode"));
        QVERIFY(!db.login(QStringLiteral("https://other.example.com:443"), &user, &pass));
        db.clearLogins();
        QCOMPARE(db.loginCount(), 0);
    }

    void signInResumesDownload()
    {
        RangeServer srv;
        srv.data = makeData(300 * 1024);
        srv.basicAuth = "bob:hunter2";
        QVERIFY(srv.start());
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.path()));
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();
        Download* d = mgr.add(srv.url("/files/data.bin"), dir.path(), {}, true);
        QVERIFY(d);
        QTRY_VERIFY_WITH_TIMEOUT(d->failed(), 20000);
        QVERIFY(d->needsLogin());
        QVERIFY(!d->autoRetryAt.isValid()); // asking again won't help: no automatic retry

        QCOMPARE(DownloadManager::loginServer(srv.url("/x")), QStringLiteral("http://127.0.0.1:%1").arg(QUrl(srv.url()).port()));
        mgr.setLogin(d->url(), QStringLiteral("bob"), QStringLiteral("hunter2"), false); // this session only
        QCOMPARE(db.loginCount(), 0);
        mgr.resume(d);
        QTRY_VERIFY_WITH_TIMEOUT(d->completed(), 20000);
        QVERIFY(!d->needsLogin());
        QVERIFY(readAll(d->filePath()) == srv.data);

        // Another file on the same server reuses the login without asking.
        Download* d2 = mgr.add(srv.url("/files/other.bin"), dir.path(), {}, true);
        QTRY_VERIFY_WITH_TIMEOUT(d2->completed(), 20000);
    }

#ifdef FLUXDM_HAS_TORRENT
    void torrentFromLocalSeeder()
    {
        namespace lt = libtorrent;
        QTemporaryDir seedDir, dlDir, stateDir;
        const QByteArray payload = makeData(3 * 1024 * 1024 + 12345);
        {
            QFile f(seedDir.filePath("payload.bin"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(payload);
        }

        // Build a torrent of the file and seed it from a plain libtorrent session on localhost.
        lt::create_torrent ct(lt::list_files(seedDir.filePath("payload.bin").toStdString()), 64 * 1024);
        lt::set_piece_hashes(ct, seedDir.path().toStdString());
        const std::vector<char> torrentBuf = ct.generate_buf();

        lt::settings_pack sp;
        sp.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
        sp.set_bool(lt::settings_pack::enable_dht, false);
        sp.set_bool(lt::settings_pack::enable_lsd, false);
        sp.set_bool(lt::settings_pack::enable_upnp, false);
        sp.set_bool(lt::settings_pack::enable_natpmp, false);
        sp.set_int(lt::settings_pack::alert_mask, lt::alert_category::status);
        lt::session seeder(sp);
        lt::add_torrent_params atp = lt::load_torrent_buffer(lt::span<char const>(torrentBuf.data(), torrentBuf.size()));
        atp.save_path = seedDir.path().toStdString();
        atp.flags |= lt::torrent_flags::seed_mode;
        atp.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed);
        const std::string magnet = lt::make_magnet_uri(atp);
        seeder.add_torrent(std::move(atp));
        QTRY_VERIFY_WITH_TIMEOUT(seeder.listen_port() != 0, 5000);

        const QString url = QString::fromStdString(magnet) + QStringLiteral("&x.pe=127.0.0.1:%1").arg(seeder.listen_port());

        BtEngine eng(stateDir.path(), 0, true);
        eng.setSeedRatio(0); // stop right after completion
        QSignalSpy meta(&eng, &DownloadEngine::meta), stopped(&eng, &DownloadEngine::stopped);
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        TaskParams p = params(9, url, dlDir.path());
        eng.start(p);

        const Outcome o = waitForEnd(fin, fail, 60000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QCOMPARE(meta.count(), 1);
        QCOMPARE(meta.first().at(1).toString(), QStringLiteral("payload.bin"));
        QCOMPARE(meta.first().at(3).toLongLong(), qint64(payload.size()));
        QVERIFY(readAll(o.path) == payload);
        // ratio 0 = don't seed: the engine releases the torrent on its own
        QTRY_VERIFY_WITH_TIMEOUT(!stopped.isEmpty(), 5000);
        QVERIFY(stopped.last().at(1).toByteArray().isEmpty());
    }

    void droppedTorrentFile()
    {
        // Dropping a .torrent on the window gives the Add dialog a file:// URL (spaces encoded).
        namespace lt = libtorrent;
        QTemporaryDir src, state;
        QDir(src.path()).mkpath("d");
        QFile data(QDir(src.filePath("d")).filePath("x.bin"));
        QVERIFY(data.open(QIODevice::WriteOnly));
        data.write(makeData(64 * 1024));
        data.close();
        lt::create_torrent ct(lt::list_files(src.filePath("d").toStdString()), 16 * 1024);
        lt::set_piece_hashes(ct, src.path().toStdString());
        const std::vector<char> buf = ct.generate_buf();
        const QString path = src.filePath("my pack [2026].torrent");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(buf.data(), qint64(buf.size()));
        f.close();

        Database db;
        QVERIFY(db.open(state.filePath("profile")));
        DownloadManager mgr(&db);
        mgr.addEngine(new BtEngine(state.filePath("bt"), 0, true));
        mgr.addEngine(new WwwEngine);
        const QString dropped = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded); // what QML's drop.urls gives
        QVERIFY(dropped.contains("%20"));
        const QStringList links = BatchLinks::expandAll({dropped});
        QCOMPARE(links, QStringList{dropped}); // "%5B2026%5D" is not a range to expand
        QCOMPARE(mgr.moduleFor(dropped), QStringLiteral("downloadsbt"));
        QCOMPARE(mgr.moduleFor(QUrl::fromLocalFile(path).toString()), QStringLiteral("downloadsbt")); // decoded form
        QCOMPARE(mgr.moduleFor(path), QStringLiteral("downloadsbt"));
        QVERIFY(!mgr.canHandle(QUrl::fromLocalFile(src.filePath("d/x.bin")).toString())); // other files aren't links
        Download* d = mgr.add(dropped, state.path(), {}, false);
        QVERIFY(d);
        QCOMPARE(d->title(), QStringLiteral("my pack [2026].torrent")); // shown decoded
    }

    void torrentChooseFilesFirst()
    {
        namespace lt = libtorrent;
        QTemporaryDir seedDir, dlDir, stateDir;
        QDir(seedDir.path()).mkpath("pack");
        const QByteArray a = makeData(1024 * 1024), b = makeData(1024 * 1024 + 0); // piece-aligned: no shared pieces
        auto put = [&](const char* name, const QByteArray& data) {
            QFile f(QDir(seedDir.filePath("pack")).filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(data);
        };
        put("a.bin", a);
        put("b.bin", b);
        lt::create_torrent ct(lt::list_files(seedDir.filePath("pack").toStdString()), 64 * 1024);
        lt::set_piece_hashes(ct, seedDir.path().toStdString());
        const std::vector<char> buf = ct.generate_buf();
        const QString torrentPath = dlDir.filePath("pack.torrent");
        {
            QFile f(torrentPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(buf.data(), qint64(buf.size()));
        }

        lt::settings_pack sp;
        sp.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
        sp.set_bool(lt::settings_pack::enable_dht, false);
        sp.set_bool(lt::settings_pack::enable_lsd, false);
        sp.set_bool(lt::settings_pack::enable_upnp, false);
        sp.set_bool(lt::settings_pack::enable_natpmp, false);
        lt::session seeder(sp);
        lt::add_torrent_params atp = lt::load_torrent_buffer(lt::span<char const>(buf.data(), buf.size()));
        atp.save_path = seedDir.path().toStdString();
        atp.flags |= lt::torrent_flags::seed_mode;
        atp.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed);
        seeder.add_torrent(std::move(atp));
        QTRY_VERIFY_WITH_TIMEOUT(seeder.listen_port() != 0, 5000);

        BtEngine eng(stateDir.path(), 0, true);
        eng.setSeedRatio(0);
        QSignalSpy awaiting(&eng, &DownloadEngine::filesAwaiting), stats(&eng, &DownloadEngine::stats);
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        TaskParams p = params(21, torrentPath, dlDir.path());
        p.pickFiles = true;
        p.speedLimit = 256 * 1024; // a few seconds, so the peer list can be observed
        eng.start(p);
        QTRY_COMPARE_WITH_TIMEOUT(awaiting.count(), 1, 10000);
        const QVariantList files = awaiting.first().at(1).toList();
        QCOMPARE(files.size(), 2);
        int bIndex = -1;
        for (const QVariant& v : files)
            if (v.toMap().value("name").toString() == QStringLiteral("b.bin"))
                bIndex = v.toMap().value("index").toInt();
        QVERIFY(bIndex >= 0);

        // While it waits, peers are offered but nothing is downloaded.
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 1500) {
            QTest::qWait(100);
            eng.addPeer(21, QStringLiteral("127.0.0.1"), seeder.listen_port());
        }
        for (const QList<QVariant>& s : stats)
            QCOMPARE(s.at(0).value<TaskStats>().received, qint64(0));
        QVERIFY(fin.isEmpty());

        eng.setSequential(21, true);
        QCOMPARE(eng.details(21).value("sequential").toBool(), true);
        eng.chooseFiles(21, {bIndex});
        t.restart();
        bool sawPeer = false;
        while (fin.isEmpty() && fail.isEmpty() && t.elapsed() < 60000) {
            QTest::qWait(100);
            eng.addPeer(21, QStringLiteral("127.0.0.1"), seeder.listen_port());
            sawPeer = sawPeer || !eng.details(21).value("peers").toList().isEmpty();
        }
        QVERIFY2(!fin.isEmpty(), qPrintable(fail.isEmpty() ? QStringLiteral("timeout") : fail.last().at(1).toString()));
        QVERIFY(sawPeer);
        const QDir pack(QDir(dlDir.path()).filePath("pack"));
        QVERIFY(readAll(pack.filePath("b.bin")) == b);
        QVERIFY(!QFile::exists(pack.filePath("a.bin")) || QFileInfo(pack.filePath("a.bin")).size() == 0 || readAll(pack.filePath("a.bin")) != a);
    }

    void torrentStopAndResume()
    {
        namespace lt = libtorrent;
        QTemporaryDir seedDir, dlDir, stateDir;
        const QByteArray payload = makeData(8 * 1024 * 1024);
        {
            QFile f(seedDir.filePath("big.bin"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(payload);
        }
        lt::create_torrent ct(lt::list_files(seedDir.filePath("big.bin").toStdString()), 64 * 1024);
        lt::set_piece_hashes(ct, seedDir.path().toStdString());
        const std::vector<char> torrentBuf = ct.generate_buf();
        QFile tf(dlDir.filePath("big.torrent"));
        QVERIFY(tf.open(QIODevice::WriteOnly));
        tf.write(torrentBuf.data(), qint64(torrentBuf.size()));
        tf.close();

        lt::settings_pack sp;
        sp.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
        sp.set_bool(lt::settings_pack::enable_dht, false);
        sp.set_bool(lt::settings_pack::enable_lsd, false);
        sp.set_int(lt::settings_pack::upload_rate_limit, 2 * 1024 * 1024); // slow enough to stop midway
        lt::session seeder(sp);
        // Local peers are unthrottled by default; throttle them so we can stop midway.
        lt::peer_class_info pci = seeder.get_peer_class(lt::session::local_peer_class_id);
        pci.upload_limit = 1024 * 1024;
        seeder.set_peer_class(lt::session::local_peer_class_id, pci);
        lt::add_torrent_params atp = lt::load_torrent_buffer(lt::span<char const>(torrentBuf.data(), torrentBuf.size()));
        atp.save_path = seedDir.path().toStdString();
        atp.flags |= lt::torrent_flags::seed_mode;
        atp.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed);
        seeder.add_torrent(std::move(atp));
        QTRY_VERIFY_WITH_TIMEOUT(seeder.listen_port() != 0, 5000);

        const QString saveDir = dlDir.filePath("out");
        BtEngine eng(stateDir.path(), 0, true);
        eng.setSeedRatio(0);
        QSignalSpy stats(&eng, &DownloadEngine::stats), stopped(&eng, &DownloadEngine::stopped);
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);

        // local .torrent file path, like a double-clicked file
        TaskParams p = params(11, dlDir.filePath("big.torrent"), saveDir);
        eng.start(p);
        QElapsedTimer t;
        t.start();
        qint64 got = 0;
        while (got < payload.size() / 5 && t.elapsed() < 30000 && fin.isEmpty() && fail.isEmpty()) {
            QTest::qWait(100);
            eng.addPeer(11, QStringLiteral("127.0.0.1"), seeder.listen_port());
            if (!stats.isEmpty())
                got = stats.last().at(0).value<TaskStats>().received;
        }
        QVERIFY2(fail.isEmpty(), qPrintable(fail.isEmpty() ? QString() : fail.last().at(1).toString()));
        QVERIFY2(fin.isEmpty(), "finished before we could stop it");
        QVERIFY(got > 0);
        eng.stop(11);
        QTRY_VERIFY_WITH_TIMEOUT(!stopped.isEmpty(), 10000);
        const QByteArray resume = stopped.last().at(1).toByteArray();
        QVERIFY(!resume.isEmpty());
        // What the seeder sent before the stop, including requests still in flight that the
        // stopped download threw away; only the upload *after* resuming says how it resumed.
        QTest::qWait(300);
        const std::int64_t uploadedBefore = seeder.get_torrents().front().status().total_payload_upload;

        // Resume from the saved state (the .torrent file is no longer needed).
        QFile::remove(dlDir.filePath("big.torrent"));
        TaskParams p2 = params(11, QStringLiteral("unused"), saveDir);
        p2.resumeData = resume;
        stats.clear();
        eng.start(p2);
        t.restart();
        while (fin.isEmpty() && fail.isEmpty() && t.elapsed() < 60000) {
            QTest::qWait(100);
            eng.addPeer(11, QStringLiteral("127.0.0.1"), seeder.listen_port());
        }
        QVERIFY2(!fin.isEmpty(), qPrintable(fail.isEmpty() ? QStringLiteral("timeout") : fail.last().at(1).toString()));
        QVERIFY(readAll(fin.last().at(1).toString()) == payload);
        // A real resume only fetches what was missing; restarting from zero would make the
        // seeder upload the whole file again after the resume.
        const std::int64_t uploadedAfter = seeder.get_torrents().front().status().total_payload_upload - uploadedBefore;
        QVERIFY2(uploadedAfter < qint64(payload.size()) - got / 2,
                 qPrintable(QStringLiteral("seeder uploaded %1 bytes after resuming a %2-byte file (got %3 before stop)")
                                .arg(uploadedAfter).arg(payload.size()).arg(got)));
    }
#endif

    void browserHostManifests()
    {
        const QJsonObject c = BrowserHost::chromiumManifest("com.fluxdm.nativehost", "C:\\FluxDM\\fluxdm-nativehost.exe", "abcdef");
        QCOMPARE(c.value("allowed_origins").toArray().first().toString(), QStringLiteral("chrome-extension://abcdef/"));
        QVERIFY(!c.contains("allowed_extensions"));
        const QJsonObject f = BrowserHost::firefoxManifest("com.fluxdm.nativehost", "C:\\FluxDM\\fluxdm-nativehost.exe", "browser-integration@fluxdm");
        QCOMPARE(f.value("allowed_extensions").toArray().first().toString(), QStringLiteral("browser-integration@fluxdm"));
        QVERIFY(!f.contains("allowed_origins"));
        QCOMPARE(f.value("type").toString(), QStringLiteral("stdio"));

        // The build ships a Firefox copy of the extension: same scripts, its own manifest.
        const QDir app(QCoreApplication::applicationDirPath());
        const QString ff = app.filePath("browser/firefox"), cr = app.filePath("browser/chromium");
        const QString id = BrowserHost::geckoId(QDir(ff).filePath("manifest.json"));
        QVERIFY2(QRegularExpression("^[A-Za-z0-9._-]+@[A-Za-z0-9._-]+$").match(id).hasMatch(), qPrintable(id)); // Mozilla's ID format
        const QJsonObject m = QJsonDocument::fromJson(readAll(QDir(ff).filePath("manifest.json"))).object();
        QCOMPARE(m.value("background").toObject().value("scripts").toArray().first().toString(), QStringLiteral("background.js"));
        QVERIFY(!m.contains("key")); // the Chromium signing key has no meaning in Firefox
        for (const char* file : {"background.js", "popup.js", "popup.html", "icons/48.png"})
            QVERIFY2(readAll(QDir(ff).filePath(file)) == readAll(QDir(cr).filePath(file)) && !readAll(QDir(ff).filePath(file)).isEmpty(), file);
        QVERIFY(!QFile::exists(QDir(ff).filePath("extension-id.txt")));
    }

    void nativeHostRejectsBadFrames()
    {
        QTemporaryDir profile;
        qputenv("FLUXDM_DATA_DIR", profile.path().toUtf8());
        auto unset = qScopeGuard([] { qunsetenv("FLUXDM_DATA_DIR"); });
        const QString host = QDir(QCoreApplication::applicationDirPath()).filePath("fluxdm-nativehost.exe");
        auto run = [&](const QByteArray& input) {
            QProcess p;
            p.start(host);
            if (!p.waitForStarted())
                return QByteArray("not started");
            p.write(input);
            p.closeWriteChannel();
            if (!p.waitForFinished(10000)) {
                p.kill();
                return QByteArray("hung");
            }
            return p.readAllStandardOutput();
        };
        const quint32 zero = 0, huge = 64u * 1024 * 1024, short_ = 100;
        // Zero length, over the 1 MB limit, and a frame that promises more than it sends: no reply, no hang.
        QCOMPARE(run(QByteArray(reinterpret_cast<const char*>(&zero), 4)), QByteArray());
        QCOMPARE(run(QByteArray(reinterpret_cast<const char*>(&huge), 4) + QByteArray(1000, 'x')), QByteArray());
        QCOMPARE(run(QByteArray(reinterpret_cast<const char*>(&short_), 4) + "{\"cmd\":"), QByteArray());
        QCOMPARE(run(QByteArray("\x01\x02")), QByteArray()); // not even a length
    }

#ifdef FLUXDM_HAS_TORRENT
    void corruptTorrentSessionFile()
    {
        QTemporaryDir state;
        QFile f(state.filePath("session.dat"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(makeData(4096)); // garbage, e.g. after a crash on an older version
        f.close();
        {
            BtEngine eng(state.path(), 0, true); // starts anyway
            QVERIFY(eng.canHandle(QUrl("magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567")));
            eng.shutdown(); // and writes a good one
        }
        BtEngine again(state.path(), 0, true);
        QVERIFY(QFileInfo(state.filePath("session.dat")).size() > 0);
    }
#endif

    void manyDownloadsAtOnce()
    {
        RangeServer srv;
        srv.data = makeData(96 * 1024);
        srv.chunk = 16 * 1024;
        QVERIFY(srv.start());
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.filePath("p")));
        DownloadManager mgr(&db);
        mgr.addEngine(new WwwEngine);
        mgr.load();
        TrafficMode::Limits lim = mgr.limits(TrafficMode::High);
        lim.maxDownloads = 20; // 20 at a time
        lim.maxConnections = 4;
        mgr.setLimits(TrafficMode::High, lim);
        mgr.setTrafficMode(TrafficMode::High);
        QList<Download*> list;
        for (int i = 0; i < 40; ++i)
            list << mgr.add(srv.url(QStringLiteral("/many/%1.bin").arg(i)), dir.path(), {}, true);
        QTRY_VERIFY_WITH_TIMEOUT(std::all_of(list.begin(), list.end(), [](Download* d) { return d->completed(); }), 60000);
        QSet<QString> paths;
        for (Download* d : list) {
            QVERIFY2(readAll(d->rec.filePath) == srv.data, qPrintable(d->rec.filePath));
            paths.insert(d->rec.filePath);
        }
        QCOMPARE(paths.size(), 40); // no two downloads wrote the same file
        QCOMPARE(db.loadDownloads().size(), 40);
    }

    void nativeHostForwardsToApp()
    {
        // Isolated channel: the host computes the same name from FLUXDM_DATA_DIR.
        QTemporaryDir profile;
        qputenv("FLUXDM_DATA_DIR", profile.path().toUtf8());
        const QString host = QDir(QCoreApplication::applicationDirPath()).filePath("fluxdm-nativehost.exe");
        QVERIFY2(QFile::exists(host), qPrintable(host));

        auto frame = [](const QJsonObject& o) {
            const QByteArray d = QJsonDocument(o).toJson(QJsonDocument::Compact);
            const quint32 n = quint32(d.size());
            return QByteArray(reinterpret_cast<const char*>(&n), 4) + d;
        };
        auto unframe = [](const QByteArray& raw) {
            if (raw.size() < 4)
                return QJsonObject();
            quint32 n = 0;
            memcpy(&n, raw.constData(), 4);
            return QJsonDocument::fromJson(raw.mid(4, n)).object();
        };

        // 1) ping with no app running; Firefox starts hosts with (manifest path, add-on id) arguments
        {
            QProcess p;
            p.start(host, {"C:\\Users\\x\\nativehost-firefox.json", "browser-integration@fluxdm"});
            QVERIFY(p.waitForStarted());
            p.write(frame({{"cmd", "ping"}}));
            p.closeWriteChannel();
            QVERIFY(p.waitForFinished(10000));
            const QJsonObject r = unframe(p.readAllStandardOutput());
            QCOMPARE(r.value("ok").toBool(), true);
            QCOMPARE(r.value("running").toBool(), false);
        }

        // 2) download request is forwarded to the app channel and the app's reply comes back
        QLocalServer app;
        QLocalServer::removeServer(Instance::channelName());
        QVERIFY(app.listen(Instance::channelName()));
        QJsonObject received;
        connect(&app, &QLocalServer::newConnection, this, [&] {
            QLocalSocket* s = app.nextPendingConnection();
            connect(s, &QLocalSocket::readyRead, s, [s, &received] {
                if (!s->canReadLine())
                    return;
                const QJsonObject req = QJsonDocument::fromJson(s->readLine()).object();
                if (req.value("cmd").toString() == "download")
                    received = req;
                s->write(QJsonDocument(QJsonObject{{"ok", true}, {"error", ""}}).toJson(QJsonDocument::Compact) + '\n');
                s->flush();
                s->disconnectFromServer();
            });
        });

        QProcess p;
        p.start(host);
        QVERIFY(p.waitForStarted());
        p.write(frame({{"cmd", "download"}, {"url", "https://example.com/file.zip"}, {"referrer", "https://example.com/"},
                       {"cookies", "a=1; b=2"}, {"fileName", "file.zip"}}));
        p.closeWriteChannel();
        QElapsedTimer t;
        t.start();
        while (p.state() != QProcess::NotRunning && t.elapsed() < 15000)
            QTest::qWait(20); // keep our event loop running so the fake app can answer
        const QJsonObject r = unframe(p.readAllStandardOutput());
        QCOMPARE(r.value("ok").toBool(), true);
        QCOMPARE(received.value("url").toString(), QStringLiteral("https://example.com/file.zip"));
        QCOMPARE(received.value("cookies").toString(), QStringLiteral("a=1; b=2"));
        qunsetenv("FLUXDM_DATA_DIR");
    }

    void perDownloadSpeedLimit()
    {
        RangeServer srv;
        srv.data = makeData(2 * 1024 * 1024);
        QVERIFY(srv.start());
        QTemporaryDir dir;
        WwwEngine eng; // no global limit
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        TaskParams p = params(31, srv.url("/capped.bin"), dir.path(), 4);
        p.speedLimit = 1024 * 1024; // 1 MiB/s for this download only
        QElapsedTimer t;
        t.start();
        eng.start(p);
        const Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QVERIFY(readAll(o.path) == srv.data);
        QVERIFY2(t.elapsed() >= 1300, qPrintable(QStringLiteral("took only %1 ms").arg(t.elapsed())));
    }

    void fileMover()
    {
        QTemporaryDir a, b;
        QFile f(a.filePath("one.bin"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hello");
        f.close();
        QVERIFY(FileMover::move(a.filePath("one.bin"), b.filePath("sub/one.bin")));
        QVERIFY(!QFile::exists(a.filePath("one.bin")));
        QCOMPARE(readAll(b.filePath("sub/one.bin")), QByteArray("hello"));

        QDir().mkpath(a.filePath("show/season1"));
        QFile g(a.filePath("show/season1/ep1.mkv"));
        QVERIFY(g.open(QIODevice::WriteOnly));
        g.write("video");
        g.close();
        QString err;
        QVERIFY(FileMover::copyTree(a.filePath("show"), b.filePath("copy"), &err));
        QCOMPARE(readAll(b.filePath("copy/season1/ep1.mkv")), QByteArray("video"));
        QVERIFY(FileMover::move(a.filePath("show"), b.filePath("show")));
        QVERIFY(QFile::exists(b.filePath("show/season1/ep1.mkv")));
        QVERIFY(!FileMover::move(a.filePath("missing"), b.filePath("x"), &err));
    }

    void m3u8Parsing()
    {
        const QUrl base("https://cdn.example.com/show/master.m3u8");
        const M3u8Playlist master = M3u8::parse(
            "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360\nlow/index.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080,CODECS=\"avc1.640028,mp4a.40.2\"\nhi/index.m3u8\n", base);
        QVERIFY(master.valid && master.master);
        QCOMPARE(master.variants.size(), 2);
        QCOMPARE(master.best()->resolution, QStringLiteral("1920x1080"));
        QCOMPARE(master.best()->url, QUrl("https://cdn.example.com/show/hi/index.m3u8"));

        const M3u8Playlist media = M3u8::parse(
            "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:7\n#EXT-X-MAP:URI=\"init.mp4\"\n"
            "#EXT-X-KEY:METHOD=AES-128,URI=\"https://keys.example.com/k1\",IV=0x000102030405060708090a0b0c0d0e0f\n"
            "#EXTINF:6.0,\nseg7.m4s\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:4.5,title\nseg8.m4s\n#EXT-X-ENDLIST\n",
            QUrl("https://cdn.example.com/show/hi/index.m3u8"));
        QVERIFY(media.valid && !media.master && media.endList);
        QCOMPARE(media.segments.size(), 2);
        QCOMPARE(media.segments[0].sequence, qint64(7));
        QCOMPARE(media.segments[0].keyIndex, 0);
        QCOMPARE(media.segments[1].keyIndex, -1);
        QCOMPARE(media.segments[1].url, QUrl("https://cdn.example.com/show/hi/seg8.m4s"));
        QCOMPARE(media.keys[0].iv, QByteArray::fromHex("000102030405060708090a0b0c0d0e0f"));
        QCOMPARE(media.initSection, QUrl("https://cdn.example.com/show/hi/init.mp4"));
        QCOMPARE(media.totalDuration, 10.5);
        QVERIFY(!M3u8::parse("<html>", base).valid);
        QCOMPARE(M3u8::sequenceIv(258), QByteArray::fromHex("00000000000000000000000000000102"));
        QCOMPARE(HlsEngine::outputBaseName(QUrl("https://x.com/videos/my-show/master.m3u8")), QStringLiteral("my-show"));
        QCOMPARE(HlsEngine::outputBaseName(QUrl("https://x.com/v/episode-3.m3u8")), QStringLiteral("episode-3"));
    }

    void aesRoundTrip()
    {
        const QByteArray key = QByteArray::fromHex("2b7e151628aed2a6abf7158809cf4f3c");
        const QByteArray iv = M3u8::sequenceIv(42);
        const QByteArray plain = makeData(100000);
        bool ok = false;
        const QByteArray enc = Aes128::encrypt(key, iv, plain, &ok);
        QVERIFY(ok);
        QCOMPARE(enc.size() % 16, 0);
        QVERIFY(enc != plain.left(enc.size()));
        QCOMPARE(Aes128::decrypt(key, iv, enc, &ok), plain);
        QVERIFY(ok);
    }

    void hlsDownload()
    {
        HlsServer srv;
        QList<QByteArray> plain;
        const QByteArray expected = setupHls(srv, 12, &plain);
        QVERIFY(srv.start());
        QTemporaryDir dir;
        HlsEngine eng;
        QSignalSpy meta(&eng, &DownloadEngine::meta), fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        QVERIFY(eng.canHandle(QUrl(srv.url("/show/master.m3u8"))));
        eng.start(params(21, srv.url("/show/master.m3u8"), dir.path()));
        const Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QCOMPARE(QFileInfo(o.path).fileName(), QStringLiteral("show.ts"));
        QVERIFY(readAll(o.path) == expected);
        QCOMPARE(srv.hits.value("/show/lo/index.m3u8"), 0); // best variant only
        QCOMPARE(srv.hits.value("/key.bin"), 1);           // key fetched once
    }

    void hlsStopAndResume()
    {
        HlsServer srv;
        QList<QByteArray> plain;
        const QByteArray expected = setupHls(srv, 16, &plain);
        srv.delayMs = 400;
        QVERIFY(srv.start());
        QTemporaryDir dir;
        HlsEngine eng;
        eng.setMaxConnections(2);
        QSignalSpy stats(&eng, &DownloadEngine::stats), stopped(&eng, &DownloadEngine::stopped);
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        eng.start(params(22, srv.url("/show/master.m3u8"), dir.path()));
        QElapsedTimer t;
        t.start();
        qint64 got = 0;
        while (got < expected.size() / 3 && t.elapsed() < 20000 && fin.isEmpty()) {
            QTest::qWait(50);
            if (!stats.isEmpty())
                got = stats.last().at(0).value<TaskStats>().received;
        }
        QVERIFY2(fin.isEmpty(), "finished before we could stop it");
        eng.stop(22);
        QTRY_VERIFY_WITH_TIMEOUT(!stopped.isEmpty(), 3000);
        const QByteArray resume = stopped.last().at(1).toByteArray();
        QVERIFY(!resume.isEmpty());
        int segHitsBefore = 0;
        for (int i = 0; i < 16; ++i)
            segHitsBefore += srv.hits.value(QStringLiteral("/show/hi/seg%1.ts").arg(i));

        TaskParams p = params(22, srv.url("/show/master.m3u8"), dir.path());
        p.resumeData = resume;
        eng.start(p);
        const Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QVERIFY(readAll(o.path) == expected);
        QCOMPARE(QFileInfo(o.path).fileName(), QStringLiteral("show.ts"));
        int segHits = 0;
        for (int i = 0; i < 16; ++i)
            segHits += srv.hits.value(QStringLiteral("/show/hi/seg%1.ts").arg(i));
        // only segments not yet written (plus the few in flight at stop) were fetched again
        QVERIFY2(segHits <= 16 + 4, qPrintable(QStringLiteral("segment requests: %1 (before stop %2)").arg(segHits).arg(segHitsBefore)));
    }

    void hlsRejectsLiveStreams()
    {
        HlsServer srv;
        srv.files.insert("/live.m3u8", "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4,\na.ts\n");
        srv.files.insert("/a.ts", "x");
        QVERIFY(srv.start());
        QTemporaryDir dir;
        HlsEngine eng;
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        eng.start(params(23, srv.url("/live.m3u8"), dir.path()));
        const Outcome o = waitForEnd(fin, fail, 10000);
        QVERIFY(o.failed);
        QVERIFY2(o.error.contains("Live"), qPrintable(o.error));
    }

    void speedLimit()
    {
        RangeServer srv;
        srv.data = makeData(3 * 1024 * 1024);
        QVERIFY(srv.start());
        QTemporaryDir dir;
        WwwEngine eng;
        eng.setSpeedLimit(1024 * 1024); // 1 MiB/s
        QSignalSpy fin(&eng, &DownloadEngine::finished), fail(&eng, &DownloadEngine::failed);
        QElapsedTimer t;
        t.start();
        eng.start(params(5, srv.url("/slow.bin"), dir.path(), 4));
        const Outcome o = waitForEnd(fin, fail, 30000);
        QVERIFY2(o.finished, qPrintable(o.error));
        QVERIFY(readAll(o.path) == srv.data);
        QVERIFY2(t.elapsed() >= 2000, qPrintable(QStringLiteral("took only %1 ms").arg(t.elapsed())));
    }
};

QTEST_MAIN(TestCore)
#include "test_core.moc"
