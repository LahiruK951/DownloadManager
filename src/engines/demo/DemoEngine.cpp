#include "engines/demo/DemoEngine.h"

#include <QDir>
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
    t.fail = q.queryItemValue(QStringLiteral("fail")) == QLatin1String("1");
    const qint64 resumed = QJsonDocument::fromJson(p.resumeData).object().value(QStringLiteral("received")).toInteger(-1);
    t.received = resumed >= 0 ? double(resumed) : t.total * q.queryItemValue(QStringLiteral("start")).toDouble();
    m_tasks.insert(p.id, t);
    emit log(p.id, tr("Demo transfer of %1 bytes").arg(t.total));
    emit meta(p.id, t.name, t.path, t.total, !t.fail);
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
        ++t.ticks;
        if (t.fail && t.ticks >= 2) {
            const QByteArray rd = resume(t);
            m_tasks.remove(id);
            emit failed(id, tr("HTTP error 404 (Not Found)"), rd);
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
