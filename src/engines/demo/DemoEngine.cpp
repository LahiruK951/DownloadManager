#include "engines/demo/DemoEngine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QUrlQuery>

#include "core/FileNamer.h"

DemoEngine::DemoEngine(QObject* parent)
    : DownloadEngine(parent)
{
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &DemoEngine::tick);
    m_timer.start();
}

QByteArray DemoEngine::resume(const Task& t)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("received"), qint64(t.received)}}).toJson(QJsonDocument::Compact);
}

void DemoEngine::start(const TaskParams& p)
{
    const QUrl url(p.url);
    const QUrlQuery q(url);
    Task t;
    t.name = FileNamer::fromUrl(url);
    t.path = QDir(p.destDir).filePath(t.name);
    t.total = q.queryItemValue(QStringLiteral("size")).toLongLong();
    if (t.total <= 0)
        t.total = 500LL * 1024 * 1024;
    t.speed = q.queryItemValue(QStringLiteral("speed")).toDouble();
    if (t.speed <= 0)
        t.speed = 4.0 * 1024 * 1024;
    t.conns = qMax(1, q.queryItemValue(QStringLiteral("conns")).toInt());
    if (!q.hasQueryItem(QStringLiteral("conns")))
        t.conns = qMin(p.maxConnections, 8);
    const QString fail = q.queryItemValue(QStringLiteral("fail"));
    t.fail = fail == QLatin1String("1") || fail == QLatin1String("401");
    t.failLogin = fail == QLatin1String("401");
    t.bt = q.queryItemValue(QStringLiteral("bt")) == QLatin1String("1");
    t.waiting = q.queryItemValue(QStringLiteral("pick")) == QLatin1String("1") && p.resumeData.isEmpty();
    const qint64 resumed = QJsonDocument::fromJson(p.resumeData).object().value(QStringLiteral("received")).toInteger(-1);
    t.received = resumed >= 0 ? double(resumed) : t.total * q.queryItemValue(QStringLiteral("start")).toDouble();
    m_tasks.insert(p.id, t);
    emit log(p.id, tr("Demo transfer of %1 bytes").arg(t.total));
    emit meta(p.id, t.name, t.path, t.total, !t.fail);
    if (t.waiting) {
        QVariantList files;
        const QStringList names = {QStringLiteral("Episode 01 - Pilot.mkv"), QStringLiteral("Episode 02 - The Bridge.mkv"),
                                   QStringLiteral("Episode 03 - Undertow.mkv"), QStringLiteral("Extras/Behind the scenes.mp4"),
                                   QStringLiteral("Subtitles/English.srt"), QStringLiteral("cover.jpg")};
        const QList<qint64> sizes = {1395864371, 1288490188, 1342177280, 524288000, 61440, 245760};
        for (int i = 0; i < names.size(); ++i)
            files << QVariantMap{{QStringLiteral("index"), i}, {QStringLiteral("name"), names[i]}, {QStringLiteral("size"), sizes[i]}};
        emit filesAwaiting(p.id, files);
    }
}

void DemoEngine::chooseFiles(qint64 id, const QList<int>&)
{
    if (m_tasks.contains(id))
        m_tasks[id].waiting = false;
}

QVariantMap DemoEngine::details(qint64 id) const
{
    if (!m_tasks.contains(id) || !m_tasks.value(id).bt)
        return {};
    auto peer = [](const char* addr, const char* client, int down, int up, double progress, bool seed) {
        return QVariantMap{{QStringLiteral("address"), QString::fromLatin1(addr)}, {QStringLiteral("client"), QString::fromLatin1(client)},
                           {QStringLiteral("down"), down}, {QStringLiteral("up"), up}, {QStringLiteral("progress"), progress},
                           {QStringLiteral("seed"), seed}};
    };
    const QVariantList peers = {peer("203.0.113.24:51413", "Transmission 4.0.6", 1843200, 0, 1.0, true),
                                peer("198.51.100.7:6881", "qBittorrent 5.0.2", 1122304, 16384, 1.0, true),
                                peer("[2001:db8::42]:49160", "libtorrent 2.0.10", 655360, 49152, 0.62, false),
                                peer("192.0.2.88:58231", "Deluge 2.1.1", 0, 8192, 0.18, false)};
    auto tracker = [](const char* url, int tier, const char* status, int seeds, int leechers) {
        return QVariantMap{{QStringLiteral("url"), QString::fromLatin1(url)}, {QStringLiteral("tier"), tier},
                           {QStringLiteral("status"), QString::fromLatin1(status)}, {QStringLiteral("seeds"), seeds},
                           {QStringLiteral("leechers"), leechers}};
    };
    const QVariantList trackers = {tracker("udp://tracker.example.org:1337/announce", 0, "Working", 214, 37),
                                   tracker("https://tracker.example.net/announce", 1, "Working", 198, 41),
                                   tracker("udp://open.example.com:6969/announce", 2, "Connection timed out", -1, -1)};
    return {{QStringLiteral("peers"), peers}, {QStringLiteral("trackers"), trackers}, {QStringLiteral("sequential"), false}};
}

void DemoEngine::stop(qint64 id)
{
    const auto it = m_tasks.find(id);
    if (it == m_tasks.end()) {
        emit stopped(id, {});
        return;
    }
    const QByteArray rd = resume(*it);
    m_tasks.erase(it);
    emit stopped(id, rd);
}

void DemoEngine::tick()
{
    const QList<qint64> ids = m_tasks.keys();
    for (qint64 id : ids) {
        Task& t = m_tasks[id];
        if (t.waiting)
            continue;
        ++t.ticks;
        if (t.fail && t.ticks >= 2) {
            const QByteArray rd = resume(t);
            m_tasks.remove(id);
            emit failed(id, t.failLogin ? QStringLiteral("HTTP error 401: the server requires a user name and password")
                                        : tr("HTTP error 404 (Not Found)"), rd);
            continue;
        }
        double sp = t.speed * (0.85 + 0.3 * QRandomGenerator::global()->generateDouble());
        if (m_limit > 0)
            sp = qMin(sp, double(m_limit));
        t.received = qMin(double(t.total), t.received + sp * 0.5);

        TaskStats s;
        s.id = id;
        s.total = t.total;
        s.received = qint64(t.received);
        s.speed = sp;
        s.connections = t.conns;
        // even segments, each a bit ahead/behind so the segment bar looks real
        const qint64 seg = t.total / t.conns;
        for (int i = 0; i < t.conns; ++i) {
            const qint64 b = i * seg;
            const qint64 e = (i == t.conns - 1) ? t.total - 1 : b + seg - 1;
            const double f = qBound(0.0, t.received / double(t.total) * (1.0 + 0.15 * ((i % 3) - 1)), 1.0);
            s.segments << b << e << qint64((e - b + 1) * f);
        }
        emit stats(s);
        if (qint64(t.received) >= t.total) {
            const QString path = t.path;
            QDir().mkpath(QFileInfo(path).absolutePath()); // empty placeholder so "Open"/missing-file logic behaves
            QFile placeholder(path);
            placeholder.open(QIODevice::WriteOnly);
            m_tasks.remove(id);
            emit finished(id, path);
        } else if (t.ticks % 4 == 0) {
            emit resumeData(id, resume(t));
        }
    }
}

QHash<qint64, QByteArray> DemoEngine::shutdown()
{
    QHash<qint64, QByteArray> out;
    for (auto it = m_tasks.begin(); it != m_tasks.end(); ++it)
        out.insert(it.key(), resume(it.value()));
    m_tasks.clear();
    m_timer.stop();
    return out;
}
