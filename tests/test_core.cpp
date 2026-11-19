#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
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
#include "ui/DownloadsModel.h"
#include <QSqlDatabase>
#include <QSqlQuery>
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

        // 1) ping with no app running
        {
            QProcess p;
            p.start(host);
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
        srv.delayMs = 120;
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
