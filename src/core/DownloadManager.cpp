#include "core/DownloadManager.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxyFactory>
#include <QDir>
#include <QUrl>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QJsonObject>
#include <QPointer>
#include <thread>
#include <QCoreApplication>
#include <QProcess>
#include <QStandardPaths>
#include "core/FileMover.h"
#include <algorithm>

#include "core/AppLog.h"
#include "core/AttachmentCheck.h"
#include "core/Checksum.h"
#include "core/DiskSpace.h"
#include <QThread>
#include "core/Database.h"
#include "core/Secret.h"
#include "core/DownloadEngine.h"
#include "core/FileNamer.h"
#include "engines/www/WwwEngine.h"

namespace {
QString systemProxyFor(const QString& url)
{
    if (!url.startsWith(QLatin1String("http"), Qt::CaseInsensitive) && !url.startsWith(QLatin1String("ftp"), Qt::CaseInsensitive))
        return {};
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
    m_statsTimer.setSingleShot(true);
    m_statsTimer.setInterval(100);
    connect(&m_statsTimer, &QTimer::timeout, this, &DownloadManager::statsUpdated);
    m_db->setErrorHandler([this](const QString& err) {
        static const qint64 kQuietMs = 60 * 1000;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_lastStorageError && now - m_lastStorageError < kQuietMs)
            return;
        m_lastStorageError = now;
        emit storageError(err);
    });
    m_manualMode = qBound(0, m_db->setting(QStringLiteral("TumMode"), int(TrafficMode::High)).toInt(), 2);
    m_effectiveMode = m_manualMode;
    const QJsonArray rules = QJsonDocument::fromJson(m_db->setting(QStringLiteral("TumSchedule")).toString().toUtf8()).array();
    for (const auto& v : rules)
        m_rules.append(TrafficRule::fromMap(v.toObject().toVariantMap()));

    addEngine(new WwwEngine(this));

    // Schedules are minute-granular; a 15 s tick keeps transitions prompt.
    m_clock.setInterval(15000);
    connect(&m_clock, &QTimer::timeout, this, [this] {
        evaluateTrafficRules();
        runAutoRetries();
        recheckDiskSpace();
        if (++m_tick % 4 == 0) { // once a minute
            checkMissingFiles();
            removeOldFinished();
        }
        schedule();
    });
    m_clock.start();
    evaluateTrafficRules();
    applyLimits();
}

DownloadManager::~DownloadManager()
{
    if (!m_shuttingDown)
        shutdown();
    m_db->setErrorHandler({}); // the database may outlive this manager
}

void DownloadManager::addEngine(DownloadEngine* e)
{
    e->setParent(this);
    // Specialised engines (e.g. BitTorrent for .torrent URLs) take precedence over the generic one.
    m_engines.prepend(e);
    connect(e, &DownloadEngine::meta, this, &DownloadManager::onMeta);
    connect(e, &DownloadEngine::stats, this, &DownloadManager::onStats);
    connect(e, &DownloadEngine::resumeData, this, &DownloadManager::onResumeData);
    connect(e, &DownloadEngine::finished, this, &DownloadManager::onFinished);
    connect(e, &DownloadEngine::failed, this, &DownloadManager::onFailed);
    connect(e, &DownloadEngine::stopped, this, &DownloadManager::onStopped);
    connect(e, &DownloadEngine::log, this, &DownloadManager::onLog);
    connect(e, &DownloadEngine::filesAwaiting, this, [this](qint64 id, const QVariantList& files) {
        if (Download* d = find(id))
            d->setPendingFiles(files);
    });
    applyLimits();
}

QString DownloadManager::defaultUserAgent()
{
    return QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) FluxDM/" FLUXDM_VERSION);
}

QString DownloadManager::userAgent() const { return m_db->setting(QStringLiteral("UserAgent")).toString(); }
void DownloadManager::setUserAgent(const QString& ua) { m_db->setSetting(QStringLiteral("UserAgent"), headerSafe(ua)); }
int DownloadManager::ipVersion() const
{
    const int v = m_db->setting(QStringLiteral("IpVersion"), 0).toInt();
    return v == 4 || v == 6 ? v : 0;
}
void DownloadManager::setIpVersion(int v) { m_db->setSetting(QStringLiteral("IpVersion"), v == 4 || v == 6 ? v : 0); }
int DownloadManager::autoRemoveDays() const { return qBound(0, m_db->setting(QStringLiteral("AutoRemoveDays"), 0).toInt(), 3650); }
void DownloadManager::setAutoRemoveDays(int days)
{
    m_db->setSetting(QStringLiteral("AutoRemoveDays"), qBound(0, days, 3650));
    removeOldFinished();
}

int DownloadManager::btListenPort() const { return m_db->setting(QStringLiteral("BtListenPort"), 0).toInt(); }
void DownloadManager::setBtListenPort(int port) { m_db->setSetting(QStringLiteral("BtListenPort"), qBound(1024, port, 65535)); }

int DownloadManager::removeOldFinished()
{
    const int days = autoRemoveDays();
    if (days <= 0)
        return 0;
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-days);
    QList<Download*> old;
    for (Download* d : m_list)
        if (d->completed() && !d->seeding() && !d->converting() && !d->moving() && d->rec.finished.isValid() && d->rec.finished < cutoff)
            old << d;
    for (Download* d : old) {
        AppLog::write(QStringLiteral("Removed from the list after %1 days: %2").arg(days).arg(d->title()));
        remove(d, false);
    }
    return int(old.size());
}

void DownloadManager::load()
{
    m_nextId = m_db->maxId();
    m_tags = m_db->loadTags();
    applyQtProxy(proxySettings());
    for (const DownloadRecord& r : m_db->loadDownloads()) {
        auto* d = new Download(r, this);
        if (d->rec.state == Download::Starting || d->rec.state == Download::Downloading)
            d->rec.state = Download::Queued; // resume what was running when we quit
        if (d->rec.hashStatus == Download::HashChecking)
            d->rec.hashStatus = Download::HashNone; // interrupted by quitting
        m_list.append(d);
        m_byId.insert(d->id(), d);
    }
    checkMissingFiles();
    removeOldFinished();
    schedule();
    // Completed torrents keep seeding across restarts until the ratio is reached.
    for (Download* d : m_list)
        if (d->completed() && d->isTorrent() && !d->rec.resumeData.isEmpty())
            if (DownloadEngine* e = engineByUid(d->rec.moduleUid)) {
                TaskParams p;
                p.id = d->id();
                p.url = d->rec.url;
                p.destDir = d->rec.destDir;
                p.resumeData = d->rec.resumeData;
                p.seedOnly = true;
                d->setSeeding(true);
                e->start(p);
            }
}

DownloadEngine* DownloadManager::engineFor(const QString& url) const
{
    const QString t = url.trimmed();
    const QUrl u = t.contains(QLatin1String("://")) || t.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)
                       ? QUrl(t, QUrl::TolerantMode) : QUrl::fromLocalFile(t);
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

QString DownloadManager::moduleFor(const QString& url) const
{
    DownloadEngine* e = engineFor(url);
    return e ? e->moduleUid() : QString();
}

Download* DownloadManager::find(qint64 id) const { return m_byId.value(id); }

Download* DownloadManager::findByUrl(const QString& url) const
{
    const auto key = [](const QString& u) { return QUrl(u.trimmed()).adjusted(QUrl::RemoveFragment).toString(); };
    const QString k = key(url);
    if (k.isEmpty())
        return nullptr;
    for (Download* d : m_list)
        if (key(d->rec.url) == k)
            return d;
    return nullptr;
}

Download* DownloadManager::add(const QString& url, const QString& destDir, const QString& fileName, bool start,
                               const QString& referrer, const QString& cookies, const QString& userAgent,
                               const ScheduleWindow* sched)
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
    if (!r.preferredName.isEmpty())
        r.title = r.preferredName;
    else if (r.url.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive))
        r.title = QUrlQuery(QUrl(r.url)).queryItemValue(QStringLiteral("dn"), QUrl::FullyDecoded);
    else
        r.title = FileNamer::fromUrl(QUrl(r.url));
    if (r.title.isEmpty())
        r.title = r.url.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive) ? tr("Magnet link") : QUrl(r.url).host();
    r.referrer = headerSafe(referrer);
    r.cookies = headerSafe(cookies);
    r.existingFileReaction = existingFileDefault();
    r.pickFiles = r.moduleUid == QLatin1String("downloadsbt") && chooseTorrentFiles();
    r.tags = tagsFor(r.title);
    qint64 last = 0;
    for (Download* x : m_list)
        last = qMax(last, x->queueOrder());
    r.queueOrder = qMax(last + 1, r.id); // the end of the queue
    r.userAgent = headerSafe(userAgent);
    r.created = QDateTime::currentDateTime();
    r.state = start ? Download::Queued : Download::Paused;
    if (sched) {
        r.scheduled = true;
        r.schedule = *sched;
    }

    auto* d = new Download(r, this);
    m_list.prepend(d);
    m_byId.insert(d->id(), d);
    persist(d);
    d->addLog(tr("Added %1").arg(r.url));
    if (r.scheduled)
        d->addLog(tr("Scheduled: %1").arg(r.schedule.text()));
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
    const QDateTime now = QDateTime::currentDateTime();
    const int maxDownloads = limits(m_effectiveMode).maxDownloads;

    // Scheduled downloads whose window closed go back to waiting.
    for (Download* d : m_list)
        if (d->running() && !d->inScheduleWindow(now)) {
            stopDownload(d);
            d->setState(Download::Queued);
            d->addLog(tr("Outside the schedule window (%1); waiting").arg(d->scheduleText()));
            persist(d);
        }

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
        if (d->queued() && d->inScheduleWindow(now) && !d->moving())
            queue.append(d);
    std::stable_sort(queue.begin(), queue.end(), [](Download* a, Download* b) {
        if (a->priority() != b->priority())
            return a->priority() > b->priority();
        return a->queueOrder() < b->queueOrder(); // the user's order (first come, first served by default)
    });
    for (Download* d : queue) {
        if (running >= maxDownloads)
            break;
        if (startDownload(d))
            ++running;
    }
    for (Download* d : m_list)
        if (d->queued() && d->scheduled())
            d->notify(); // refresh "Scheduled"/"Queued" text
    checkAllDone();
}

void DownloadManager::checkAllDone()
{
    // "When done" fires once after the last active download finishes
    // (scheduled downloads still waiting for their window don't count).
    const bool active = std::any_of(m_list.begin(), m_list.end(), [](Download* d) {
        return d->running() || (d->queued() && !d->waitingForSchedule());
    });
    if (m_hadActive && !active)
        emit allDownloadsFinished();
    m_hadActive = active;
}

bool DownloadManager::startDownload(Download* d)
{
    DownloadEngine* e = engineByUid(d->rec.moduleUid);
    if (!e) {
        d->setState(Download::Error, tr("No engine for module %1").arg(d->rec.moduleUid));
        persist(d);
        return false;
    }
    if (!DiskSpace::volumeAvailable(d->rec.destDir)) {
        const QString why = DiskSpace::unavailableText(d->rec.destDir);
        if (d->rec.lastError != why) {
            d->addLog(why);
            d->addLog(tr("The download continues when the drive is back"));
        }
        d->setState(Download::Error, why);
        persist(d);
        return false;
    }
    if (d->rec.total > 0) {
        const qint64 need = qMax<qint64>(0, d->rec.total - d->rec.received);
        const qint64 free = DiskSpace::freeBytes(d->rec.destDir);
        if (need > 0 && free >= 0 && free < need + DiskSpace::kMargin) {
            const QString why = DiskSpace::shortageText(need, free, d->rec.destDir);
            d->addLog(why);
            d->addLog(tr("Waiting for free disk space; the download continues by itself"));
            d->setState(Download::Error, why);
            persist(d);
            return false;
        }
    }
    TaskParams p;
    p.id = d->id();
    p.url = d->rec.url;
    p.destDir = d->rec.destDir;
    p.fileName = d->rec.preferredName;
    p.referrer = d->rec.referrer;
    p.cookies = d->rec.cookies;
    p.userAgent = !d->rec.userAgent.isEmpty() ? d->rec.userAgent : !userAgent().isEmpty() ? userAgent() : defaultUserAgent();
    p.ipVersion = ipVersion();
    p.proxy = proxyFor(d->rec.url);
    if (QUrl(d->rec.url).userName().isEmpty()) // credentials in the URL win
        loginFor(d->rec.url, &p.user, &p.password);
    p.existingFile = d->rec.existingFileReaction;
    p.ffmpeg = ffmpegPath();
    p.postData = d->rec.postData;
    p.mirrors = d->rec.mirrors;
    p.trustCertificate = m_db->trustedHosts().contains(QUrl(d->rec.url).host().toLower());
    if (const QString y = ytdlpPath(); !y.isEmpty())
        p.siteTool = {y};
    p.pickFiles = d->rec.pickFiles;
    p.sequential = d->rec.sequential;
    p.resumeData = d->rec.resumeData;
    p.maxConnections = limits(m_effectiveMode).maxConnections;
    p.speedLimit = qint64(d->rec.speedLimitKBps) * 1024;
    d->setState(Download::Starting);
    d->addLog(tr("Starting"));
    persist(d);
    e->start(p);
    return true;
}

void DownloadManager::stopDownload(Download* d)
{
    if (!d->running() && !d->seeding())
        return;
    if (DownloadEngine* e = engineByUid(d->rec.moduleUid)) {
        ++d->pendingStops;
        e->stop(d->id());
    }
    d->setSeeding(false);
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
    d->autoRetryCount = 0; // a manual resume starts a fresh retry budget
    d->autoRetryAt = {};
    d->setState(Download::Queued);
    persist(d);
    schedule();
}

void DownloadManager::stopSeeding(Download* d)
{
    if (!d || !d->seeding())
        return;
    stopDownload(d);
    d->addLog(tr("Seeding stopped"));
}

void DownloadManager::restart(Download* d)
{
    if (!d)
        return;
    stopDownload(d);
    if (!d->rec.filePath.isEmpty()) {
        QFile::remove(d->rec.filePath + FileNamer::kPartSuffix);
        if (d->completed() && QFileInfo(d->rec.filePath).isFile())
            QFile::remove(d->rec.filePath);
    }
    d->rec.resumeData.clear();
    d->rec.received = 0;
    d->rec.total = -1;
    d->rec.filePath.clear();
    d->rec.finished = {};
    d->rec.resumable = -1;
    d->setSegmentsFromResumeData();
    d->setHashResult(Download::HashNone, {});
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
    if (!d->rec.filePath.isEmpty()) {
        const QFileInfo fi(d->rec.filePath);
        if (deleteFiles || !d->completed()) {
            QFile::remove(d->rec.filePath + FileNamer::kPartSuffix);
            if (d->isTorrent() && fi.isDir())
                QDir(d->rec.filePath).removeRecursively(); // multi-file torrent folder
            else if (deleteFiles || d->isTorrent())
                QFile::remove(d->rec.filePath);
        }
    }
    m_db->remove(d->id());
    m_list.removeOne(d);
    m_byId.remove(d->id());
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

void DownloadManager::setSchedule(Download* d, bool enabled, const ScheduleWindow& w)
{
    if (!d)
        return;
    d->rec.scheduled = enabled;
    d->rec.schedule = w;
    d->addLog(enabled ? tr("Scheduled: %1").arg(w.text()) : tr("Schedule removed"));
    if (enabled && d->paused())
        d->setState(Download::Queued); // a schedule implies "start when the window opens"
    d->notify();
    persist(d);
    schedule();
}

void DownloadManager::setSpeedLimit(Download* d, int kbps)
{
    if (!d)
        return;
    d->rec.speedLimitKBps = qMax(0, kbps);
    d->addLog(kbps > 0 ? tr("Speed limited to %1 KB/s").arg(kbps) : tr("Speed limit removed"));
    d->notify();
    persist(d);
    if (DownloadEngine* e = engineByUid(d->rec.moduleUid))
        e->setTaskSpeedLimit(d->id(), qint64(d->rec.speedLimitKBps) * 1024);
}

void DownloadManager::setFileWanted(Download* d, int index, bool wanted)
{
    if (!d)
        return;
    if (DownloadEngine* e = engineByUid(d->rec.moduleUid))
        e->setFileWanted(d->id(), index, wanted);
}

void DownloadManager::pauseAll()
{
    for (Download* d : m_list)
        if (d->running() || d->queued()) {
            stopDownload(d);
            d->setState(Download::Paused);
            persist(d);
        }
    m_hadActive = false;
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

// ------------------------------------------------------------------ traffic modes

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
    if (mode == m_effectiveMode) {
        applyLimits();
        schedule();
    }
}

void DownloadManager::setTrafficMode(int mode)
{
    mode = qBound(0, mode, 2);
    m_manualMode = mode;
    m_db->setSetting(QStringLiteral("TumMode"), mode);
    // Clicking a mode while a schedule rule is active overrides that rule until its window ends.
    if (m_activeRule >= 0)
        m_overriddenRule = m_activeRule;
    evaluateTrafficRules();
}

void DownloadManager::setTrafficRules(const QList<TrafficRule>& rules)
{
    m_rules = rules;
    QJsonArray arr;
    for (const TrafficRule& r : rules)
        arr.append(QJsonObject::fromVariantMap(r.toMap()));
    m_db->setSetting(QStringLiteral("TumSchedule"), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    m_activeRule = m_overriddenRule = -1;
    evaluateTrafficRules();
}

void DownloadManager::evaluateTrafficRules()
{
    const QDateTime now = QDateTime::currentDateTime();
    int active = -1;
    for (int i = 0; i < m_rules.size(); ++i)
        if (m_rules[i].enabled && m_rules[i].window.contains(now)) {
            active = i;
            break;
        }
    if (active != m_activeRule) {
        m_activeRule = active;
        if (m_overriddenRule != active)
            m_overriddenRule = -1; // the overridden window ended
    }
    const int mode = trafficModeScheduled() ? m_rules[m_activeRule].mode : m_manualMode;
    const bool modeChanged = mode != m_effectiveMode;
    m_effectiveMode = mode;
    if (modeChanged) {
        applyLimits();
        schedule();
    }
    emit trafficModeChanged();
}

void DownloadManager::applyLimits()
{
    const TrafficMode::Limits l = limits(m_effectiveMode);
    for (DownloadEngine* e : m_engines) {
        e->setSpeedLimit(qint64(l.speedLimitKBps) * 1024);
        e->setMaxConnections(l.maxConnections);
        e->setMaxConnectionsPerServer(maxConnectionsPerServer());
        e->setUploadLimit(uploadLimitKBps() * 1024);
        e->setSeedRatio(seedRatio());
    }
}

qint64 DownloadManager::uploadLimitKBps() const
{
    // In snail (Low) mode uploads are throttled too, like FDM.
    const qint64 user = m_db->setting(QStringLiteral("MaxUploadSpeed"), 0).toLongLong();
    if (m_effectiveMode == TrafficMode::Low) {
        const qint64 lowCap = qMax<qint64>(16, limits(TrafficMode::Low).speedLimitKBps / 4);
        return user > 0 ? qMin(user, lowCap) : lowCap;
    }
    return user;
}

void DownloadManager::setUploadLimitKBps(qint64 kbps)
{
    m_db->setSetting(QStringLiteral("MaxUploadSpeed"), qMax<qint64>(0, kbps));
    applyLimits();
}

double DownloadManager::seedRatio() const { return m_db->setting(QStringLiteral("MaxURatio"), 1.0).toDouble(); }

void DownloadManager::setSeedRatio(double r)
{
    m_db->setSetting(QStringLiteral("MaxURatio"), qMax(0.0, r));
    applyLimits();
}

void DownloadManager::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    m_clock.stop();
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
    if (!d || !(d->running() || d->seeding()))
        return;
    d->rec.filePath = filePath;
    d->rec.total = total;
    d->rec.resumable = resumable ? 1 : 0;
    d->setTitle(fileName);
    // The real name is known now (Content-Disposition, torrent name): add matching tags.
    QList<int> tags = d->rec.tags;
    for (int t : tagsFor(fileName))
        if (!tags.contains(t))
            tags << t;
    d->setTags(tags);
    if (d->rec.state == Download::Starting) {
        d->rec.state = Download::Downloading;
        emit d->stateChanged();
    }
    d->notify();
    persist(d);
}

void DownloadManager::onStats(const TaskStats& st)
{
    if (Download* d = find(st.id)) {
        const QString cat = d->rec.contentCategory;
        d->applyStats(st);
        if (d->rec.contentCategory != cat)
            persist(d); // keep it when the torrent is not loaded (finished, or before metadata)
    }
    statsChanged();
}

void DownloadManager::onResumeData(qint64 id, const QByteArray& data)
{
    Download* d = find(id);
    if (!d || !(d->running() || d->seeding()) || data.isEmpty())
        return;
    d->rec.resumeData = data;
    m_db->saveProgress(id, d->rec.received, data);
}

void DownloadManager::onFinished(qint64 id, const QString& filePath)
{
    Download* d = find(id);
    if (!d || !d->running())
        return;
    d->rec.filePath = filePath;
    d->setTitle(QFileInfo(filePath).fileName());
    if (d->rec.total < 0)
        d->rec.total = d->rec.received;
    d->rec.received = d->rec.total;
    d->rec.finished = QDateTime::currentDateTime();
    d->autoRetryCount = 0;
    d->autoRetryAt = {};
    if (!d->isTorrent())
        d->rec.resumeData.clear(); // torrents keep it to seed after restart
    d->setFileMissing(false);
    d->setState(Download::Completed);
    if (d->isTorrent())
        d->setSeeding(true);
    persist(d);
    emit downloadFinished(d);
    statsChanged();
    schedule();
    if (d->rec.moduleUid == QLatin1String("downloadsm3u") && filePath.endsWith(QLatin1String(".ts"), Qt::CaseInsensitive)
        && convertStreams())
        convertToMp4(d);
    if (d->rec.moduleUid == QLatin1String("downloadswww") && FileNamer::looksLikeTorrentFile(filePath))
        openDownloadedTorrent(d);
    else if (!d->isTorrent() && antivirusCheck() && QFileInfo(filePath).isFile())
        checkWithAntivirus(d);
    if (!d->isTorrent() && !d->rec.expectedHash.isEmpty() && QFileInfo(filePath).isFile())
        verifyChecksum(d);
}

bool DownloadManager::antivirusCheck() const { return m_db->setting(QStringLiteral("AntivirusCheck"), true).toBool(); }
void DownloadManager::setAntivirusCheck(bool on) { m_db->setSetting(QStringLiteral("AntivirusCheck"), on); }

// Like a browser: the antivirus scans the new file and Windows marks it as coming from the internet.
// Runs on its own thread (a scan of a large file takes a while).
void DownloadManager::checkWithAntivirus(Download* d)
{
    const QString path = d->rec.filePath, url = d->rec.url, ref = d->rec.referrer;
    auto result = std::make_shared<std::pair<int, QString>>(AttachmentCheck::Unavailable, QString());
    QThread* th = QThread::create([path, url, ref, result] { result->first = AttachmentCheck::check(path, url, ref, &result->second); });
    QPointer<Download> guard(d);
    connect(th, &QThread::finished, this, [this, th, guard, path, result] {
        th->deleteLater();
        if (!guard || guard->rec.filePath != path)
            return;
        if (result->first == AttachmentCheck::Blocked) {
            const QString msg = tr("Blocked by your antivirus: %1").arg(result->second);
            guard->addLog(msg);
            guard->setFileMissing(!QFileInfo::exists(path));
            guard->setState(Download::Error, msg);
            persist(guard);
            emit antivirusChecked(guard, true, msg);
        } else if (result->first == AttachmentCheck::Clean) {
            guard->addLog(tr("Checked by your antivirus"));
            emit antivirusChecked(guard, false, {});
        } else {
            guard->addLog(tr("Not checked by an antivirus (%1)").arg(result->second));
        }
    });
    th->start();
}

void DownloadManager::setExpectedChecksum(Download* d, const QString& text)
{
    if (!d)
        return;
    d->rec.expectedHash = Checksum::parse(text).hex.isEmpty() ? QString() : text.trimmed();
    d->setHashResult(Download::HashNone, {});
    persist(d);
}

QString DownloadManager::verifyChecksum(Download* d, const QString& expectedText)
{
    if (!d || !d->completed() || d->isTorrent())
        return tr("Only finished downloads can be checked");
    if (d->hashStatus() == Download::HashChecking)
        return tr("Already checking");
    const QString text = expectedText.trimmed().isEmpty() ? d->rec.expectedHash : expectedText.trimmed();
    const Checksum::Expected want = Checksum::parse(text);
    if (!want.valid())
        return tr("Paste an MD5, SHA-1, SHA-256 or SHA-512 checksum");
    const QString path = d->rec.filePath;
    if (!QFileInfo(path).isFile())
        return tr("The file is missing");
    d->rec.expectedHash = text;
    d->setHashResult(Download::HashChecking, tr("Computing %1…").arg(want.name));
    persist(d);
    auto result = std::make_shared<std::pair<QString, QString>>(); // digest, error
    QThread* th = QThread::create([path, want, result] { result->first = Checksum::ofFile(path, want.algo, &result->second); });
    QPointer<Download> guard(d);
    connect(th, &QThread::finished, this, [this, th, guard, path, want, result] {
        th->deleteLater();
        if (!guard)
            return;
        if (result->first.isEmpty()) {
            guard->setHashResult(Download::HashFailed, tr("Could not read the file: %1").arg(result->second));
        } else if (result->first == want.hex) {
            guard->setHashResult(Download::HashMatch, tr("%1 matches").arg(want.name));
            guard->addLog(tr("%1 checksum matches").arg(want.name));
            emit checksumChecked(guard, true, guard->hashText());
        } else {
            guard->setHashResult(Download::HashMismatch, tr("%1 does not match: the file is %2").arg(want.name, result->first));
            guard->addLog(tr("%1 checksum does NOT match (expected %2, file is %3)").arg(want.name, want.hex, result->first));
            emit checksumChecked(guard, false, guard->hashText());
        }
        persist(guard);
    });
    th->start();
    return {};
}

void DownloadManager::openDownloadedTorrent(Download* d)
{
    // Links like ".../torrent/download/<hash>" deliver a .torrent file without saying so in the URL.
    // Like FDM, start it as a torrent instead of leaving the small .torrent file in the list.
    QString path = d->rec.filePath;
    if (!path.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive)) {
        const QString renamed = FileNamer::uniquePath(QFileInfo(path).absolutePath(), QFileInfo(path).fileName() + QStringLiteral(".torrent"));
        if (QFile::rename(path, renamed))
            path = renamed;
    }
    if (!canHandle(path)) {
        d->addLog(tr("This is a .torrent file, but BitTorrent support is not available"));
        return;
    }
    Download* t = add(path, d->rec.destDir, {}, true, d->rec.url);
    if (!t)
        return;
    t->addLog(tr("Started from the torrent file downloaded from %1").arg(d->rec.url));
    const qint64 oldId = d->id();
    QTimer::singleShot(0, this, [this, oldId] { remove(find(oldId), false); });
}

void DownloadManager::onFailed(qint64 id, const QString& error, const QByteArray& resumeData)
{
    Download* d = find(id);
    if (!d || !d->running())
        return;
    if (!resumeData.isEmpty())
        d->rec.resumeData = resumeData;
    d->setSegmentsFromResumeData();
    d->setPendingFiles({});
    QString why = error;
    if (!DiskSpace::isWaiting(error) && !DiskSpace::volumeAvailable(d->rec.destDir)) {
        d->addLog(error);
        why = DiskSpace::unavailableText(d->rec.destDir); // the drive went away mid-download
    } else if (DiskSpace::looksLikeFullDisk(error) && !DiskSpace::isShortage(error)) {
        const qint64 free = DiskSpace::freeBytes(d->rec.destDir);
        const qint64 need = d->rec.total > 0 ? qMax<qint64>(0, d->rec.total - d->rec.received) : 0;
        if (free >= 0 && free < need + DiskSpace::kMargin) {
            d->addLog(error);
            why = DiskSpace::shortageText(need, free, d->rec.destDir);
        }
    }
    d->setState(Download::Error, why);
    if (DiskSpace::isWaiting(why)) {
        d->addLog(DiskSpace::isShortage(why) ? tr("Waiting for free disk space; the download continues by itself")
                                             : tr("The download continues when the drive is back"));
        persist(d);
        statsChanged();
        schedule();
        return;
    }
    // FDM-style automatic retries for transient failures: 30 s, 1, 2, 5, 10 min...
    if (isRetriable(error) && d->autoRetryCount < autoRetryLimit()) {
        static const int delays[] = {30, 60, 120, 300, 600};
        const int delay = delays[qMin(d->autoRetryCount, 4)];
        d->autoRetryAt = QDateTime::currentDateTime().addSecs(delay);
        d->addLog(tr("Will retry automatically in %1 s (attempt %2 of %3)").arg(delay).arg(d->autoRetryCount + 1).arg(autoRetryLimit()));
        d->notify();
    }
    persist(d);
    statsChanged();
    schedule();
}

void DownloadManager::onStopped(qint64 id, const QByteArray& resumeData)
{
    Download* d = find(id);
    if (!d)
        return;
    if (d->pendingStops > 0)
        --d->pendingStops;
    d->setPendingFiles({}); // the engine let go; it asks again on the next start
    if (d->completed()) { // seeding ended (ratio reached or stopped by the user)
        d->setSeeding(false);
        d->rec.resumeData.clear(); // don't resume seeding on the next start
        persist(d);
        statsChanged();
        return;
    }
    // If the download was already restarted, this stale data belongs to the old task.
    if (!d->running() && d->pendingStops == 0 && !resumeData.isEmpty() && d->rec.state != Download::Queued) {
        d->rec.resumeData = resumeData;
        d->setSegmentsFromResumeData();
        d->notify();
        m_db->saveProgress(id, d->rec.received, resumeData);
    } else if (d->rec.state == Download::Queued && !resumeData.isEmpty() && !d->rec.resumeData.isEmpty()) {
        // Re-queued by a traffic-mode/schedule switch: keep the freshest resume point.
        d->rec.resumeData = resumeData;
        m_db->saveProgress(id, d->rec.received, resumeData);
    }
    statsChanged();
}

void DownloadManager::onLog(qint64 id, const QString& line)
{
    if (Download* d = find(id))
        d->addLog(line);
}

// ------------------------------------------------------------------ automatic retries

bool DownloadManager::chooseTorrentFiles() const { return m_db->setting(QStringLiteral("BtChooseFiles"), true).toBool(); }
void DownloadManager::setChooseTorrentFiles(bool on) { m_db->setSetting(QStringLiteral("BtChooseFiles"), on); }

void DownloadManager::chooseFiles(Download* d, const QList<int>& wanted)
{
    if (!d || !d->awaitingFiles() || wanted.isEmpty())
        return;
    d->rec.pickFiles = false;
    d->setPendingFiles({});
    persist(d);
    if (DownloadEngine* e = engineByUid(d->rec.moduleUid))
        e->chooseFiles(d->id(), wanted);
}

void DownloadManager::setSequential(Download* d, bool on)
{
    if (!d || d->rec.sequential == on)
        return;
    d->rec.sequential = on;
    d->addLog(on ? tr("Downloading in order") : tr("Downloading rarest pieces first"));
    d->notify();
    persist(d);
    if (DownloadEngine* e = engineByUid(d->rec.moduleUid))
        e->setSequential(d->id(), on);
}

QVariantMap DownloadManager::details(Download* d) const
{
    DownloadEngine* e = d ? engineByUid(d->rec.moduleUid) : nullptr;
    return e ? e->details(d->id()) : QVariantMap();
}

int DownloadManager::existingFileDefault() const
{
    return qBound(0, m_db->setting(QStringLiteral("ExistingFileReaction"), int(FileNamer::KeepBoth)).toInt(), 2);
}

void DownloadManager::setExistingFileDefault(int reaction)
{
    m_db->setSetting(QStringLiteral("ExistingFileReaction"), qBound(0, reaction, 2));
}

void DownloadManager::setExistingFileReaction(Download* d, int reaction)
{
    if (!d)
        return;
    d->rec.existingFileReaction = qBound(0, reaction, 2);
    persist(d);
}

int DownloadManager::autoRetryLimit() const { return qBound(0, m_db->setting(QStringLiteral("AutoRetryCount"), 5).toInt(), 50); }

void DownloadManager::setAutoRetryLimit(int n) { m_db->setSetting(QStringLiteral("AutoRetryCount"), qBound(0, n, 50)); }

bool DownloadManager::isRetriable(const QString& error)
{
    // Permanent problems: retrying cannot help.
    static const QRegularExpression permanent(QStringLiteral(
        R"(HTTP error 4(0[0-7]|09|1\d|2[0-8]|3\d|4\d|5\d)\b|Disk write|Cannot create|Cannot allocate|Cannot rename|Unsupported|)"
        R"(already being downloaded|Untrusted certificate|web page instead|File already exists|Login required|Login failed|Invalid magnet|Not a valid|No engine|keeps changing|Cannot open)"),
        QRegularExpression::CaseInsensitiveOption);
    return !error.isEmpty() && !permanent.match(error).hasMatch();
}

void DownloadManager::runAutoRetries()
{
    const QDateTime now = QDateTime::currentDateTime();
    for (Download* d : m_list) {
        if (d->failed() && d->autoRetryAt.isValid()) {
            if (d->autoRetryAt <= now) {
                d->autoRetryAt = {};
                ++d->autoRetryCount;
                d->addLog(tr("Retrying automatically"));
                d->setState(Download::Queued);
                persist(d);
            } else {
                d->notify(); // refresh the countdown
            }
        }
    }
}
void DownloadManager::recheckDiskSpace()
{
    bool any = false;
    for (Download* d : m_list) {
        if (!d->failed() || !DiskSpace::isWaiting(d->rec.lastError))
            continue;
        if (DiskSpace::isUnavailable(d->rec.lastError)) {
            if (!DiskSpace::volumeAvailable(d->rec.destDir))
                continue;
            d->addLog(tr("The drive is back; continuing"));
            d->setState(Download::Queued);
            persist(d);
            any = true;
            continue;
        }
        const qint64 free = DiskSpace::freeBytes(d->rec.destDir);
        const qint64 need = (d->rec.total > 0 ? qMax<qint64>(0, d->rec.total - d->rec.received) : 0) + DiskSpace::kMargin;
        if (free >= 0 && free < need)
            continue;
        d->addLog(tr("Enough free disk space again; continuing"));
        d->setState(Download::Queued);
        persist(d);
        any = true;
    }
    if (any)
        schedule();
}

void DownloadManager::checkMissingFiles()
{
    // FDM marks completed downloads whose file was moved or deleted.
    for (Download* d : m_list)
        if (d->completed() && !d->rec.filePath.isEmpty())
            d->setFileMissing(!QFileInfo::exists(d->rec.filePath));
        else
            d->setFileMissing(false);
}
// ------------------------------------------------------------------ move files (FDM: moveFilesTasks)

bool DownloadManager::canMove(const Download* d)
{
    return d && !d->running() && !d->seeding() && !d->moving();
}

bool DownloadManager::canRename(const Download* d)
{
    return d && d->completed() && !d->isTorrent() && !d->moving() && !d->converting() && !d->fileMissing()
        && !d->rec.filePath.isEmpty();
}

QString DownloadManager::rename(Download* d, const QString& newNameIn)
{
    if (!canRename(d))
        return tr("Only finished downloads can be renamed");
    const QString newName = newNameIn.trimmed();
    if (newName.isEmpty())
        return tr("Enter a file name");
    if (FileNamer::sanitize(newName) != newName)
        return tr("A file name can't contain any of these characters: \\ / : * ? \" < > |");
    const QString oldPath = d->rec.filePath;
    if (!QFileInfo::exists(oldPath)) {
        d->setFileMissing(true);
        return tr("The file was moved or deleted");
    }
    const QString newPath = QDir::toNativeSeparators(QFileInfo(oldPath).dir().filePath(newName));
    if (newName == QFileInfo(oldPath).fileName())
        return {};
    // A case-only change ("a.mp4" -> "A.mp4") is the same file on Windows, not a collision.
    const bool sameFile = newPath.compare(QDir::toNativeSeparators(oldPath), Qt::CaseInsensitive) == 0;
    if (!sameFile && (QFileInfo::exists(newPath) || QFileInfo::exists(newPath + FileNamer::kPartSuffix)))
        return tr("%1 already exists in this folder").arg(newName);
    if (!QDir().rename(oldPath, newPath))
        return tr("Cannot rename the file (is it open in another program?)");

    d->rec.filePath = QDir::toNativeSeparators(newPath);
    d->rec.preferredName = newName; // "Restart" downloads under the new name too
    QJsonObject rd = QJsonDocument::fromJson(d->rec.resumeData).object();
    if (rd.contains(QStringLiteral("finalPath"))) {
        rd[QStringLiteral("finalPath")] = d->rec.filePath;
        rd[QStringLiteral("partPath")] = d->rec.filePath + FileNamer::kPartSuffix;
        rd[QStringLiteral("fileName")] = newName;
        d->rec.resumeData = QJsonDocument(rd).toJson(QJsonDocument::Compact);
    }
    d->addLog(tr("Renamed to %1").arg(newName));
    d->setTitle(newName);
    d->notify();
    persist(d);
    return {};
}

QString DownloadManager::moveTo(Download* d, const QString& newDirIn)
{
    if (!d)
        return tr("No download");
    if (!canMove(d))
        return tr("Pause the download (or stop seeding) before moving it");
    const QString newDir = QDir::toNativeSeparators(QDir::cleanPath(newDirIn));
    if (QDir::cleanPath(newDir).compare(QDir::cleanPath(d->rec.destDir), Qt::CaseInsensitive) == 0)
        return {};
    if (!QDir().mkpath(newDir))
        return tr("Cannot create folder %1").arg(newDir);

    // What is on disk now: the final file/folder when finished, else the partial file.
    QString src, dst;
    const QString name = QFileInfo(d->rec.filePath).fileName();
    if (!d->rec.filePath.isEmpty()) {
        if (d->completed()) {
            src = d->rec.filePath;
            dst = FileNamer::uniquePath(newDir, name);
        } else if (QFileInfo::exists(d->rec.filePath + FileNamer::kPartSuffix)) {
            src = d->rec.filePath + FileNamer::kPartSuffix;
            dst = FileNamer::uniquePath(newDir, name) + FileNamer::kPartSuffix;
        } else if (d->isTorrent() && QFileInfo::exists(d->rec.filePath)) {
            src = d->rec.filePath; // partially downloaded torrent data
            dst = QDir(newDir).filePath(name);
        }
    }

    auto applyPaths = [this, d, newDir](const QString& movedTo) {
        d->rec.destDir = newDir;
        if (!movedTo.isEmpty()) {
            const QString finalPath = movedTo.endsWith(FileNamer::kPartSuffix)
                                          ? movedTo.left(movedTo.size() - FileNamer::kPartSuffix.size()) : movedTo;
            d->rec.filePath = finalPath;
            d->setTitle(QFileInfo(finalPath).fileName());
            // HTTP/HLS resume data remembers where the partial file lives.
            QJsonObject rd = QJsonDocument::fromJson(d->rec.resumeData).object();
            if (rd.contains(QStringLiteral("partPath"))) {
                rd[QStringLiteral("finalPath")] = finalPath;
                rd[QStringLiteral("partPath")] = finalPath + FileNamer::kPartSuffix;
                rd[QStringLiteral("fileName")] = QFileInfo(finalPath).fileName();
                d->rec.resumeData = QJsonDocument(rd).toJson(QJsonDocument::Compact);
            } else if (d->isTorrent()) {
                d->rec.resumeData.clear(); // libtorrent re-checks the data in its new location
            }
        }
        d->addLog(tr("Moved to %1").arg(newDir));
        d->setFileMissing(false);
        d->notify();
        persist(d);
    };

    if (src.isEmpty()) { // nothing on disk yet: just change where it will be saved
        applyPaths({});
        return {};
    }

    // Same volume: rename right away. Otherwise copy in the background.
    if (QDir().rename(src, dst)) {
        applyPaths(dst);
        return {};
    }
    d->setMoving(true);
    d->addLog(tr("Moving to %1…").arg(newDir));
    QPointer<Download> guard(d);
    QPointer<DownloadManager> self(this);
    std::thread([self, guard, src, dst, applyPaths] {
        QString error;
        const bool ok = FileMover::move(src, dst, &error);
        // qApp outlives the manager; both pointers are re-checked on the GUI thread.
        QMetaObject::invokeMethod(qApp, [self, guard, ok, error, dst, applyPaths] {
            if (!self || !guard)
                return;
            guard->setMoving(false);
            if (ok)
                applyPaths(dst);
            else
                guard->addLog(tr("Move failed: %1").arg(error));
        }, Qt::QueuedConnection);
    }).detach();
    return {};
}
// ------------------------------------------------------------------ stream conversion (ffmpeg)

bool DownloadManager::convertStreams() const { return m_db->setting(QStringLiteral("ConvertStreamsToMp4"), true).toBool(); }
void DownloadManager::setConvertStreams(bool on) { m_db->setSetting(QStringLiteral("ConvertStreamsToMp4"), on); }
QString DownloadManager::ffmpegSetting() const { return m_db->setting(QStringLiteral("FfmpegPath")).toString(); }
void DownloadManager::setFfmpegSetting(const QString& path) { m_db->setSetting(QStringLiteral("FfmpegPath"), QDir::toNativeSeparators(path.trimmed())); }

QString DownloadManager::ytdlpSetting() const { return m_db->setting(QStringLiteral("YtDlpPath")).toString(); }
void DownloadManager::setYtdlpSetting(const QString& path) { m_db->setSetting(QStringLiteral("YtDlpPath"), QDir::toNativeSeparators(path.trimmed())); }

QString DownloadManager::ytdlpPath() const
{
    for (const QString& candidate : {ytdlpSetting(), qEnvironmentVariable("FLUXDM_YTDLP")})
        if (!candidate.isEmpty() && QFileInfo(candidate).isExecutable())
            return candidate;
    const QString beside = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("yt-dlp.exe"));
    if (QFileInfo(beside).isExecutable())
        return beside;
    return QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
}

QString DownloadManager::ffmpegPath() const
{
    for (const QString& candidate : {ffmpegSetting(), qEnvironmentVariable("FLUXDM_FFMPEG")})
        if (!candidate.isEmpty() && QFileInfo(candidate).isExecutable())
            return candidate;
    const QString beside = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("ffmpeg.exe"));
    if (QFileInfo(beside).isExecutable())
        return beside;
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

void DownloadManager::convertToMp4(Download* d)
{
    const QString ffmpeg = ffmpegPath();
    if (ffmpeg.isEmpty()) {
        d->addLog(tr("Not converted to MP4: ffmpeg was not found (Settings > Downloads)"));
        return;
    }
    const QString src = d->rec.filePath;
    const QString dst = FileNamer::uniquePath(QFileInfo(src).absolutePath(), QFileInfo(src).completeBaseName() + QStringLiteral(".mp4"));
    d->setConverting(true);
    d->addLog(tr("Converting to MP4…"));

    auto* proc = new QProcess(this);
    QPointer<Download> guard(d);
    connect(proc, &QProcess::finished, this, [this, proc, guard, src, dst](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        if (!guard)
            return;
        guard->setConverting(false);
        if (status == QProcess::NormalExit && code == 0 && QFileInfo(dst).size() > 0) {
            QFile::remove(src);
            guard->rec.filePath = dst;
            guard->rec.total = guard->rec.received = QFileInfo(dst).size();
            guard->setTitle(QFileInfo(dst).fileName());
            guard->addLog(tr("Converted to %1").arg(QFileInfo(dst).fileName()));
            persist(guard);
        } else {
            QFile::remove(dst);
            const QString err = QString::fromLocal8Bit(proc->readAllStandardError()).trimmed().section(QLatin1Char('\n'), -1);
            guard->addLog(tr("MP4 conversion failed; keeping the .ts file (%1)").arg(err.isEmpty() ? tr("exit code %1").arg(code) : err));
        }
        guard->notify();
    });
    // Lossless remux: no re-encoding, just a new container (+faststart for streaming players).
    proc->start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y"),
                         QStringLiteral("-i"), src, QStringLiteral("-map"), QStringLiteral("0:v?"), QStringLiteral("-map"), QStringLiteral("0:a?"),
                         QStringLiteral("-c"), QStringLiteral("copy"),
                         QStringLiteral("-bsf:a"), QStringLiteral("aac_adtstoasc"), QStringLiteral("-movflags"), QStringLiteral("+faststart"), dst});
}
// ------------------------------------------------------------------ server logins

QString DownloadManager::loginServer(const QString& url)
{
    const QUrl u(url);
    if (u.host().isEmpty())
        return {};
    const QString scheme = u.scheme().toLower();
    const int port = u.port(scheme == QLatin1String("https") ? 443 : scheme == QLatin1String("ftp") ? 21
                            : scheme == QLatin1String("sftp") ? 22 : 80);
    return QStringLiteral("%1://%2:%3").arg(scheme, u.host().toLower()).arg(port);
}

bool DownloadManager::loginFor(const QString& url, QString* user, QString* password) const
{
    const QString server = loginServer(url);
    if (server.isEmpty())
        return false;
    if (m_sessionLogins.contains(server)) {
        *user = m_sessionLogins.value(server).first;
        *password = m_sessionLogins.value(server).second;
        return true;
    }
    return m_db->login(server, user, password);
}

void DownloadManager::setLogin(const QString& url, const QString& user, const QString& password, bool remember)
{
    const QString server = loginServer(url);
    if (server.isEmpty())
        return;
    m_sessionLogins.insert(server, {user, password});
    if (remember)
        m_db->saveLogin(server, user, password);
    else
        m_db->removeLogin(server); // "don't remember" also forgets an older saved one
}

int DownloadManager::savedLoginCount() const { return m_db->loginCount(); }

void DownloadManager::forgetLogins()
{
    m_sessionLogins.clear();
    m_db->clearLogins();
}

// ------------------------------------------------------------------ user tags

Tag DownloadManager::tag(int id) const
{
    for (const Tag& t : m_tags)
        if (t.id == id)
            return t;
    return {};
}

int DownloadManager::saveTag(Tag t)
{
    t.name = t.name.trimmed();
    if (t.name.isEmpty())
        return 0;
    const int id = m_db->saveTag(t);
    if (id <= 0)
        return 0;
    t.id = id;
    bool found = false;
    for (Tag& x : m_tags)
        if (x.id == id) {
            x = t;
            found = true;
        }
    if (!found)
        m_tags << t;
    emit tagsChanged();
    return id;
}

void DownloadManager::removeTag(int id)
{
    m_db->removeTag(id);
    m_tags.erase(std::remove_if(m_tags.begin(), m_tags.end(), [id](const Tag& t) { return t.id == id; }), m_tags.end());
    for (Download* d : m_list)
        if (d->hasTag(id)) {
            QList<int> l = d->rec.tags;
            l.removeAll(id);
            d->setTags(l);
            persist(d);
        }
    emit tagsChanged();
}

void DownloadManager::setTag(Download* d, int tagId, bool on)
{
    if (!d || tag(tagId).id == 0 || d->hasTag(tagId) == on)
        return;
    QList<int> l = d->rec.tags;
    if (on)
        l << tagId;
    else
        l.removeAll(tagId);
    d->setTags(l);
    persist(d);
}

QList<int> DownloadManager::tagsFor(const QString& fileName) const
{
    QList<int> out;
    for (const Tag& t : m_tags)
        if (t.matches(fileName))
            out << t.id;
    return out;
}

QString DownloadManager::tagFolderFor(const QString& fileName) const
{
    for (const Tag& t : m_tags)
        if (!t.folder.isEmpty() && t.matches(fileName))
            return t.folder;
    return {};
}

// ------------------------------------------------------------------ MP3/MP4 converter

QString DownloadManager::convertibleFile(const Download* d)
{
    if (!d || !d->completed() || d->fileMissing() || d->moving() || d->converting())
        return {};
    const QFileInfo fi(d->rec.filePath);
    if (fi.isFile()) {
        const QString cat = FileNamer::category(fi.fileName());
        return cat == QLatin1String("Video") || cat == QLatin1String("Music") ? fi.absoluteFilePath() : QString();
    }
    // A finished torrent folder: its largest audio/video file.
    QString best;
    qint64 size = -1;
    for (const QVariant& v : d->files()) {
        const QVariantMap f = v.toMap();
        const QString cat = FileNamer::category(f.value(QStringLiteral("name")).toString());
        if ((cat == QLatin1String("Video") || cat == QLatin1String("Music")) && f.value(QStringLiteral("size")).toLongLong() > size) {
            const QString path = QDir(fi.absoluteFilePath()).filePath(f.value(QStringLiteral("name")).toString());
            if (QFileInfo(path).isFile()) {
                best = path;
                size = f.value(QStringLiteral("size")).toLongLong();
            }
        }
    }
    return best;
}

QString DownloadManager::convertFile(Download* d, const ConvertOptions& o)
{
    const QString input = convertibleFile(d);
    if (input.isEmpty())
        return tr("Only finished audio and video files can be converted");
    if (o.format == QLatin1String("mp4") && FileNamer::category(input) != QLatin1String("Video"))
        return tr("Only videos can be converted to MP4");
    const QString ffmpeg = ffmpegPath();
    if (ffmpeg.isEmpty())
        return tr("ffmpeg was not found. Install it or set its location in Settings > Downloads.");

    auto* job = new MediaConvert(ffmpeg, input, o, this);
    QPointer<Download> guard(d);
    d->setConverting(true);
    d->addLog(tr("Converting to %1: %2").arg(o.format.toUpper(), QDir::toNativeSeparators(job->outputPath())));
    connect(job, &MediaConvert::progress, this, [guard](double f) {
        if (guard)
            guard->setConvertProgress(f);
    });
    connect(job, &MediaConvert::finished, this, [this, job, guard](const QString& out, const QString& err) {
        job->deleteLater();
        if (!guard)
            return;
        guard->setConverting(false);
        guard->addLog(err.isEmpty() ? tr("Converted: %1").arg(QDir::toNativeSeparators(out)) : tr("Conversion failed: %1").arg(err));
        emit conversionFinished(guard, out, err);
    });
    job->start();
    return {};
}

// ------------------------------------------------------------------ network settings

ProxySettings DownloadManager::proxySettings() const
{
    ProxySettings ps;
    ps.mode = qBound(0, m_db->setting(QStringLiteral("ProxyMode"), 0).toInt(), 2);
    ps.type = m_db->setting(QStringLiteral("ProxyType"), QStringLiteral("http")).toString();
    ps.host = m_db->setting(QStringLiteral("ProxyHost")).toString();
    ps.port = m_db->setting(QStringLiteral("ProxyPort"), 8080).toInt();
    ps.user = m_db->setting(QStringLiteral("ProxyUser")).toString();
    ps.password = Secret::unprotect(QByteArray::fromBase64(m_db->setting(QStringLiteral("ProxyPassword")).toByteArray()));
    return ps;
}

void DownloadManager::setProxySettings(const ProxySettings& ps)
{
    m_db->setSetting(QStringLiteral("ProxyMode"), ps.mode);
    m_db->setSetting(QStringLiteral("ProxyType"), ps.type);
    m_db->setSetting(QStringLiteral("ProxyHost"), ps.host.trimmed());
    m_db->setSetting(QStringLiteral("ProxyPort"), ps.port);
    m_db->setSetting(QStringLiteral("ProxyUser"), ps.user);
    m_db->setSetting(QStringLiteral("ProxyPassword"), ps.password.isEmpty() ? QByteArray() : Secret::protect(ps.password).toBase64());
    applyQtProxy(ps);
}

// Qt's network stack (HLS/DASH segments, .torrent files) follows the same choice as curl.
void DownloadManager::applyQtProxy(const ProxySettings& ps)
{
    if (ps.mode == ProxySettings::None || ps.manualValid()) {
        QNetworkProxyFactory::setUseSystemConfiguration(false);
        QNetworkProxy::setApplicationProxy(ps.qtProxy());
    } else {
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::DefaultProxy));
        QNetworkProxyFactory::setUseSystemConfiguration(true);
    }
}

QString DownloadManager::proxyFor(const QString& url) const
{
    const ProxySettings ps = proxySettings();
    if (ps.mode == ProxySettings::System)
        return systemProxyFor(url);
    return ps.curlProxy(); // "none" or the manual proxy ("" if it's incomplete)
}

int DownloadManager::maxConnectionsPerServer() const
{
    return qBound(0, m_db->setting(QStringLiteral("MaxConnectionsPerServer"), 0).toInt(), 64);
}

void DownloadManager::setMaxConnectionsPerServer(int n)
{
    m_db->setSetting(QStringLiteral("MaxConnectionsPerServer"), qBound(0, n, 64));
    for (DownloadEngine* e : m_engines)
        e->setMaxConnectionsPerServer(maxConnectionsPerServer());
}

// ------------------------------------------------------------------ manual queue order

QList<Download*> DownloadManager::queueOrder() const
{
    QList<Download*> q;
    for (Download* d : m_list)
        if (!d->completed())
            q << d;
    std::stable_sort(q.begin(), q.end(), [](Download* a, Download* b) { return a->queueOrder() < b->queueOrder(); });
    return q;
}

void DownloadManager::moveInQueue(const QList<Download*>& ds, const QString& where)
{
    QList<Download*> q = queueOrder();
    QList<Download*> moving;
    for (Download* d : q) // in queue order, so a group keeps its relative order
        if (ds.contains(d))
            moving << d;
    if (moving.isEmpty())
        return;
    if (where == QLatin1String("top") || where == QLatin1String("bottom")) {
        for (Download* d : moving)
            q.removeOne(d);
        if (where == QLatin1String("top"))
            q = moving + q;
        else
            q += moving;
    } else if (where == QLatin1String("up")) {
        for (Download* d : moving) {
            const int i = int(q.indexOf(d));
            if (i > 0 && !moving.contains(q[i - 1]))
                q.swapItemsAt(i, i - 1);
        }
    } else if (where == QLatin1String("down")) {
        for (int k = int(moving.size()) - 1; k >= 0; --k) {
            const int i = int(q.indexOf(moving[k]));
            if (i >= 0 && i + 1 < q.size() && !moving.contains(q[i + 1]))
                q.swapItemsAt(i, i + 1);
        }
    } else {
        return;
    }
    // Renumber the whole queue: positions stay small and unique.
    for (int i = 0; i < q.size(); ++i) {
        if (q[i]->rec.queueOrder != i + 1) {
            q[i]->rec.queueOrder = i + 1;
            q[i]->notify();
            persist(q[i]);
        }
    }
    emit queueReordered();
    schedule();
}

// ------------------------------------------------------------------ import from Free Download Manager

QPair<int, int> DownloadManager::importFdm(const QList<FdmImport::Item>& items)
{
    int added = 0, skipped = 0;
    qint64 lastOrder = 0;
    for (Download* x : m_list)
        lastOrder = qMax(lastOrder, x->queueOrder());
    for (const FdmImport::Item& it : items) {
        const bool known = std::any_of(m_list.begin(), m_list.end(), [&](Download* d) { return d->rec.url == it.url; });
        if (known) {
            ++skipped;
            continue;
        }
        DownloadRecord r;
        r.id = ++m_nextId;
        DownloadEngine* e = engineFor(it.url);
        r.moduleUid = it.moduleUid == QLatin1String("downloadsbt") || !e ? it.moduleUid : e->moduleUid();
        r.url = it.url;
        r.referrer = it.referrer;
        r.userAgent = it.userAgent;
        r.destDir = it.destDir;
        r.title = it.title.isEmpty() ? FileNamer::fromUrl(QUrl(it.url)) : it.title;
        r.created = it.created;
        r.priority = it.priority;
        r.queueOrder = ++lastOrder + m_nextId; // after everything already queued
        if (it.finished.isValid()) {
            r.state = Download::Completed;
            r.finished = it.finished;
            r.filePath = QDir::toNativeSeparators(QDir(it.destDir).filePath(r.title));
            const QFileInfo fi(r.filePath);
            r.total = r.received = fi.isFile() ? fi.size() : -1;
        } else {
            r.state = Download::Paused;
        }
        for (const QString& name : it.tags) {
            int id = 0;
            for (const Tag& t : m_tags)
                if (t.name.compare(name, Qt::CaseInsensitive) == 0)
                    id = t.id;
            if (id == 0) {
                Tag t;
                t.name = name;
                t.color = it.tagColor(name).isEmpty() ? QStringLiteral("#539bf5") : it.tagColor(name);
                id = saveTag(t);
            }
            if (id > 0 && !r.tags.contains(id))
                r.tags << id;
        }
        auto* d = new Download(r, this);
        m_list.prepend(d);
        m_byId.insert(d->id(), d);
        d->addLog(tr("Imported from Free Download Manager"));
        persist(d);
        emit downloadAdded(d);
        ++added;
    }
    checkMissingFiles();
    return {added, skipped};
}

// ------------------------------------------------------------------ trusted certificates

void DownloadManager::trustCertificate(Download* d)
{
    if (!d)
        return;
    const QString host = QUrl(d->rec.url).host().toLower();
    if (host.isEmpty())
        return;
    m_db->addTrustedHost(host);
    d->addLog(tr("Trusting the certificate of %1 from now on").arg(host));
    resume(d);
}

QStringList DownloadManager::trustedHosts() const { return m_db->trustedHosts(); }
void DownloadManager::forgetTrustedHosts() { m_db->clearTrustedHosts(); }
