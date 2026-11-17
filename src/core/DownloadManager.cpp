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
#include "core/FileMover.h"
#include <algorithm>

#include "core/Database.h"
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
        if (++m_tick % 4 == 0) // once a minute
            checkMissingFiles();
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
    applyLimits();
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
    checkMissingFiles();
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

Download* DownloadManager::find(qint64 id) const
{
    for (Download* d : m_list)
        if (d->id() == id)
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
    r.referrer = referrer;
    r.cookies = cookies;
    r.userAgent = userAgent;
    r.created = QDateTime::currentDateTime();
    r.state = start ? Download::Queued : Download::Paused;
    if (sched) {
        r.scheduled = true;
        r.schedule = *sched;
    }

    auto* d = new Download(r, this);
    m_list.prepend(d);
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
        return a->id() < b->id(); // FIFO
    });
    for (Download* d : queue) {
        if (running >= maxDownloads)
            break;
        startDownload(d);
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
    p.maxConnections = limits(m_effectiveMode).maxConnections;
    p.speedLimit = qint64(d->rec.speedLimitKBps) * 1024;
    d->setState(Download::Starting);
    d->addLog(tr("Starting"));
    persist(d);
    e->start(p);
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
    // FDM-style automatic retries for transient failures: 30 s, 1, 2, 5, 10 min...
    if (isRetriable(error) && d->autoRetryCount < autoRetryLimit()) {
        static const int delays[] = {30, 60, 120, 300, 600};
        const int delay = delays[qMin(d->autoRetryCount, 4)];
        d->autoRetryAt = QDateTime::currentDateTime().addSecs(delay);
        d->addLog(tr("Will retry automatically in %1 s (attempt %2 of %3)").arg(delay).arg(d->autoRetryCount + 1).arg(autoRetryLimit()));
        d->notify();
    }
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
    if (d->completed()) { // seeding ended (ratio reached or stopped by the user)
        d->setSeeding(false);
        d->rec.resumeData.clear(); // don't resume seeding on the next start
        persist(d);
        emit statsUpdated();
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
    emit statsUpdated();
}

void DownloadManager::onLog(qint64 id, const QString& line)
{
    if (Download* d = find(id))
        d->addLog(line);
}

// ------------------------------------------------------------------ automatic retries

int DownloadManager::autoRetryLimit() const { return qBound(0, m_db->setting(QStringLiteral("AutoRetryCount"), 5).toInt(), 50); }

void DownloadManager::setAutoRetryLimit(int n) { m_db->setSetting(QStringLiteral("AutoRetryCount"), qBound(0, n, 50)); }

bool DownloadManager::isRetriable(const QString& error)
{
    // Permanent problems: retrying cannot help.
    static const QRegularExpression permanent(QStringLiteral(
        R"(HTTP error 4(0[0-7]|09|1\d|2[0-8]|3\d|4\d|5\d)\b|Disk write|Cannot create|Cannot allocate|Cannot rename|Unsupported|)"
        R"(already being downloaded|Invalid magnet|Not a valid|No engine|keeps changing|Cannot open)"),
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