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
#include "core/BatchLinks.h"
#include "core/FileNamer.h"
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

        BtEngine eng(stateDir.path(), 0);
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
        BtEngine eng(stateDir.path(), 0);
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

        // Resume from the saved state (the .torrent file is no longer needed).
        QFile::remove(dlDir.filePath("big.torrent"));
        TaskParams p2 = params(11, QStringLiteral("unused"), saveDir);
        p2.resumeData = resume;
        stats.clear();
        eng.start(p2);
        bool sawPartial = false;
        t.restart();
        while (fin.isEmpty() && fail.isEmpty() && t.elapsed() < 60000) {
            QTest::qWait(100);
            eng.addPeer(11, QStringLiteral("127.0.0.1"), seeder.listen_port());
            // Within the first moments after resuming, progress must already include the old data
            // (2 MB/s seeder: re-downloading `got` bytes from scratch would take far longer).
            if (t.elapsed() < 1500)
                for (const auto& args : stats)
                    if (args.at(0).value<TaskStats>().received >= got / 2)
                        sawPartial = true;
        }
        QVERIFY2(!fin.isEmpty(), qPrintable(fail.isEmpty() ? QStringLiteral("timeout") : fail.last().at(1).toString()));
        QVERIFY2(sawPartial, "resume started from zero");
        QVERIFY(readAll(fin.last().at(1).toString()) == payload);
    }
#endif

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
