#include "core/DownloadManager.h"

#include <QFile>
#include <QFileInfo>
#include <QNetworkProxyFactory>
#include <QUrl>
#include <algorithm>

#include "core/Database.h"
#include "core/DownloadEngine.h"
#include "core/FileNamer.h"
#include "engines/www/WwwEngine.h"

namespace {
QString systemProxyFor(const QString& url)
{
    const auto proxies = QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(QUrl(url)));
    for (const auto& p : proxies) {
        if (p.type() == QNetworkProxy::HttpProxy)
            return QStringLiteral("http://%1:%2").arg(p.hostName()).arg(p.port());
        if (p.type() == QNetworkProxy::Socks5Proxy)
            return QStringLiteral("socks5h://%1:%2").arg(p.hostName()).arg(p.port());
    }
    return {};
}
} // namespace

DownloadManager::DownloadManager(Database* db, QObject* parent)
    : QObject(parent), m_db(db)
{
    m_engines.append(new WwwEngine(this));
    for (DownloadEngine* e : m_engines) {
        connect(e, &DownloadEngine::meta, this, &DownloadManager::onMeta);
        connect(e, &DownloadEngine::stats, this, &DownloadManager::onStats);
        connect(e, &DownloadEngine::resumeData, this, &DownloadManager::onResumeData);
        connect(e, &DownloadEngine::finished, this, &DownloadManager::onFinished);
        connect(e, &DownloadEngine::failed, this, &DownloadManager::onFailed);
        connect(e, &DownloadEngine::stopped, this, &DownloadManager::onStopped);
        connect(e, &DownloadEngine::log, this, &DownloadManager::onLog);
    }
    m_mode = qBound(0, m_db->setting(QStringLiteral("TumMode"), int(TrafficMode::High)).toInt(), 2);
    applyLimits();
}

DownloadManager::~DownloadManager()
{
    if (!m_shuttingDown)
        shutdown();
}

QString DownloadManager::defaultUserAgent()
{
    return QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) FluxDM/" FLUXDM_VERSION);
}

void DownloadManager::load()
{
    m_nextId = m_db->maxId();
    for (const DownloadRecord& r : m_db->loadDownloads()) {
        auto* d = new Download(r, this);
        if (d->rec.state == Download::Starting || d->rec.state == Download::Downloading)
            d->rec.state = Download::Queued; // resume what was running when we quit
        m_list.append(d);
    }
    schedule();
}

DownloadEngine* DownloadManager::engineFor(const QString& url) const
{
    const QUrl u(url.trimmed(), QUrl::TolerantMode);
    for (DownloadEngine* e : m_engines)
        if (e->canHandle(u))
            return e;
    return nullptr;
}

DownloadEngine* DownloadManager::engineByUid(const QString& uid) const
{
    for (DownloadEngine* e : m_engines)
        if (e->moduleUid() == uid)
            return e;
    return nullptr;
}

bool DownloadManager::canHandle(const QString& url) const { return engineFor(url) != nullptr; }

Download* DownloadManager::find(qint64 id) const
{
    for (Download* d : m_list)
        if (d->id() == id)
            return d;
    return nullptr;
}

Download* DownloadManager::add(const QString& url, const QString& destDir, const QString& fileName, bool start,
                               const QString& referrer, const QString& cookies, const QString& userAgent)
{
    DownloadEngine* e = engineFor(url);
    if (!e)
        return nullptr;
    DownloadRecord r;
    r.id = ++m_nextId;
    r.moduleUid = e->moduleUid();
    r.url = url.trimmed();
    r.destDir = destDir;
    r.preferredName = fileName.trimmed();
    r.title = !r.preferredName.isEmpty() ? r.preferredName : FileNamer::fromUrl(QUrl(r.url));
    if (r.title.isEmpty())
        r.title = QUrl(r.url).host();
    r.referrer = referrer;
    r.cookies = cookies;
    r.userAgent = userAgent;
    r.created = QDateTime::currentDateTime();
    r.state = start ? Download::Queued : Download::Paused;

    auto* d = new Download(r, this);
    m_list.prepend(d);
    persist(d);
    d->addLog(tr("Added %1").arg(r.url));
    emit downloadAdded(d);
    schedule();
    return d;
}

void DownloadManager::persist(Download* d) { m_db->save(d->rec); }

// ------------------------------------------------------------------ queue

void DownloadManager::schedule()
{
    if (m_shuttingDown)
        return;
    const int maxDownloads = limits(m_mode).maxDownloads;
    int running = int(std::count_if(m_list.begin(), m_list.end(), [](Download* d) { return d->running(); }));

    // Too many running after switching to a lower mode: send the newest back to the queue.
    for (Download* d : m_list) {
        if (running <= maxDownloads)
            break;
        if (d->running()) {
            stopDownload(d);
            d->setState(Download::Queued);
            persist(d);
            --running;
        }
    }

    QList<Download*> queue;
    for (Download* d : m_list)
        if (d->queued())
            queue.append(d);
    std::stable_sort(queue.begin(), queue.end(), [](Download* a, Download* b) {
        if (a->priority() != b->priority())
            return a->priority() > b->priority();
        return a->id() < b->id(); // FIFO
    });
    for (Download* d : queue) {
        if (running >= maxDownloads)
            break;
        startDownload(d);
        ++running;
    }
}

void DownloadManager::startDownload(Download* d)
{
    DownloadEngine* e = engineByUid(d->rec.moduleUid);
    if (!e) {
        d->setState(Download::Error, tr("No engine for module %1").arg(d->rec.moduleUid));
        persist(d);
        return;
    }
    TaskParams p;
    p.id = d->id();
    p.url = d->rec.url;
    p.destDir = d->rec.destDir;
    p.fileName = d->rec.preferredName;
    p.referrer = d->rec.referrer;
    p.cookies = d->rec.cookies;
    p.userAgent = d->rec.userAgent.isEmpty() ? defaultUserAgent() : d->rec.userAgent;
    p.proxy = systemProxyFor(d->rec.url);
    p.resumeData = d->rec.resumeData;
    p.maxConnections = limits(m_mode).maxConnections;
    d->setState(Download::Starting);
    d->addLog(tr("Starting"));
    persist(d);
    e->start(p);
}

void DownloadManager::stopDownload(Download* d)
{
    if (!d->running())
        return;
    if (DownloadEngine* e = engineByUid(d->rec.moduleUid)) {
        ++d->pendingStops;
        e->stop(d->id());
    }
}

void DownloadManager::pause(Download* d)
{
    if (!d || d->completed() || d->paused())
        return;
    stopDownload(d);
    d->setState(Download::Paused);
    d->addLog(tr("Paused"));
    persist(d);
    schedule();
}

void DownloadManager::resume(Download* d)
{
    if (!d || !(d->paused() || d->failed()))
        return;
    d->setState(Download::Queued);
    persist(d);
    schedule();
}

void DownloadManager::restart(Download* d)
{
    if (!d)
        return;
    stopDownload(d);
    if (!d->rec.filePath.isEmpty()) {
        QFile::remove(d->rec.filePath + FileNamer::kPartSuffix);
        if (d->completed())
            QFile::remove(d->rec.filePath);
    }
    d->rec.resumeData.clear();
    d->rec.received = 0;
    d->rec.total = -1;
    d->rec.filePath.clear();
    d->rec.finished = {};
    d->rec.resumable = -1;
    d->setSegmentsFromResumeData();
    d->setState(Download::Queued);
    d->addLog(tr("Restarted"));
    persist(d);
    schedule();
}

void DownloadManager::remove(Download* d, bool deleteFiles)
{
    if (!d)
        return;
    stopDownload(d);
    if (deleteFiles && !d->rec.filePath.isEmpty()) {
        QFile::remove(d->rec.filePath + FileNamer::kPartSuffix);
        if (d->completed())
            QFile::remove(d->rec.filePath);
    } else if (!d->completed() && !d->rec.filePath.isEmpty()) {
        QFile::remove(d->rec.filePath + FileNamer::kPartSuffix); // unfinished data is useless without the record
    }
    m_db->remove(d->id());
    m_list.removeOne(d);
    emit downloadRemoved(d);
    d->deleteLater();
    schedule();
}

void DownloadManager::setPriority(Download* d, int priority)
{
    if (!d)
        return;
    d->rec.priority = qBound(0, priority, 2);
    d->notify();
    persist(d);
    schedule();
}

void DownloadManager::pauseAll()
{
    for (Download* d : m_list)
        if (d->running() || d->queued()) {
            stopDownload(d);
            d->setState(Download::Paused);
            persist(d);
        }
}

void DownloadManager::resumeAll()
{
    for (Download* d : m_list)
        if (d->paused() || d->failed()) {
            d->setState(Download::Queued);
            persist(d);
        }
    schedule();
}

// ------------------------------------------------------------------ traffic mode

TrafficMode::Limits DownloadManager::limits(int mode) const
{
    const QString p = TrafficMode::prefix(mode);
    const TrafficMode::Limits def = TrafficMode::defaults(mode);
    TrafficMode::Limits l;
    l.maxDownloads = qMax(1, m_db->setting(p + QStringLiteral("_MaxDownloads"), def.maxDownloads).toInt());
    l.maxConnections = qBound(1, m_db->setting(p + QStringLiteral("_MaxConnections"), def.maxConnections).toInt(), 32);
    l.speedLimitKBps = qMax(0, m_db->setting(p + QStringLiteral("_MaxDownloadSpeed"), def.speedLimitKBps).toInt());
    return l;
}

void DownloadManager::setLimits(int mode, const TrafficMode::Limits& l)
{
    const QString p = TrafficMode::prefix(mode);
    m_db->setSetting(p + QStringLiteral("_MaxDownloads"), l.maxDownloads);
    m_db->setSetting(p + QStringLiteral("_MaxConnections"), l.maxConnections);
    m_db->setSetting(p + QStringLiteral("_MaxDownloadSpeed"), l.speedLimitKBps);
    if (mode == m_mode) {
        applyLimits();
        schedule();
    }
}

void DownloadManager::setTrafficMode(int mode)
{
    mode = qBound(0, mode, 2);
    if (mode == m_mode)
        return;
    m_mode = mode;
    m_db->setSetting(QStringLiteral("TumMode"), mode);
    applyLimits();
    schedule();
}

void DownloadManager::applyLimits()
{
    const TrafficMode::Limits l = limits(m_mode);
    for (DownloadEngine* e : m_engines) {
        e->setSpeedLimit(qint64(l.speedLimitKBps) * 1024);
        e->setMaxConnections(l.maxConnections);
    }
}

void DownloadManager::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    for (DownloadEngine* e : m_engines) {
        const auto finals = e->shutdown();
        for (auto it = finals.begin(); it != finals.end(); ++it) {
            if (Download* d = find(it.key())) {
                if (!it.value().isEmpty())
                    d->rec.resumeData = it.value();
                m_db->saveProgress(d->id(), d->rec.received, d->rec.resumeData);
            }
        }
    }
}

// ------------------------------------------------------------------ engine callbacks

void DownloadManager::onMeta(qint64 id, const QString& fileName, const QString& filePath, qint64 total, bool resumable)
{
    Download* d = find(id);
    if (!d || !d->running())
        return;
    d->rec.filePath = filePath;
    d->rec.total = total;
    d->rec.resumable = resumable ? 1 : 0;
    d->setTitle(fileName);
    if (d->rec.state == Download::Starting) {
        d->rec.state = Download::Downloading;
        emit d->stateChanged();
    }
    d->notify();
    persist(d);
}

void DownloadManager::onStats(const TaskStats& st)
{
    if (Download* d = find(st.id))
        d->applyStats(st);
    emit statsUpdated();
}

void DownloadManager::onResumeData(qint64 id, const QByteArray& data)
{
    Download* d = find(id);
    if (!d || !d->running())
        return;
    d->rec.resumeData = data;
    m_db->saveProgress(id, d->rec.received, data);
}

void DownloadManager::onFinished(qint64 id, const QString& filePath)
{
    Download* d = find(id);
    if (!d)
        return;
    d->rec.filePath = filePath;
    d->setTitle(QFileInfo(filePath).fileName());
    if (d->rec.total < 0)
        d->rec.total = d->rec.received;
    d->rec.received = d->rec.total;
    d->rec.finished = QDateTime::currentDateTime();
    d->rec.resumeData.clear();
    d->setState(Download::Completed);
    persist(d);
    emit downloadFinished(d);
    emit statsUpdated();
    schedule();
}

void DownloadManager::onFailed(qint64 id, const QString& error, const QByteArray& resumeData)
{
    Download* d = find(id);
    if (!d || !d->running())
        return;
    if (!resumeData.isEmpty())
        d->rec.resumeData = resumeData;
    d->setSegmentsFromResumeData();
    d->setState(Download::Error, error);
    persist(d);
    emit statsUpdated();
    schedule();
}

void DownloadManager::onStopped(qint64 id, const QByteArray& resumeData)
{
    Download* d = find(id);
    if (!d)
        return;
    if (d->pendingStops > 0)
        --d->pendingStops;
    // If the download was already restarted, this stale data belongs to the old task.
    if (!d->running() && d->pendingStops == 0 && !resumeData.isEmpty() && d->rec.state != Download::Queued) {
        d->rec.resumeData = resumeData;
        d->setSegmentsFromResumeData();
        d->notify();
        m_db->saveProgress(id, d->rec.received, resumeData);
    } else if (d->rec.state == Download::Queued && !resumeData.isEmpty() && !d->rec.resumeData.isEmpty()) {
        // Re-queued by a traffic-mode switch: keep the freshest resume point.
        d->rec.resumeData = resumeData;
        m_db->saveProgress(id, d->rec.received, resumeData);
    }
    emit statsUpdated();
}

void DownloadManager::onLog(qint64 id, const QString& line)
{
    if (Download* d = find(id))
        d->addLog(line);
}
