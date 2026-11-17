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
#include "core/FileNamer.h"
#include "engines/www/WwwEngine.h"

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
