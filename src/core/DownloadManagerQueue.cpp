// DownloadManager: the queue, traffic modes and manual queue order.
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
    flushUsage();
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
