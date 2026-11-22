// AppController: actions on downloads, links and schedules.
#include "ui/AppController.h"
#include "core/Metalink.h"

#include <QClipboard>
#include <cmath>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSEngine>
#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QSet>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <powrprof.h>
#include <mmsystem.h>
#include <shlobj.h>
#endif

#include "core/AppLog.h"
#include "core/BatchLinks.h"
#include "core/Secret.h"
#include "core/Checksum.h"
#include "core/BrowserHost.h"
#include "core/Instance.h"
#include "core/Download.h"
#include "core/DownloadManager.h"
#include "core/FileNamer.h"
#include "ui/DownloadsModel.h"

namespace {
const QStringList kCategories = {QStringLiteral("Video"), QStringLiteral("Music"), QStringLiteral("Archive"),
                                  QStringLiteral("Docs"), QStringLiteral("Programs"), QStringLiteral("Pictures"),
                                  QStringLiteral("Other")};

// Extensions that make a copied link worth offering for download (clipboard monitor).
const QStringList kCatchExtensions = {
    QStringLiteral("zip"), QStringLiteral("rar"), QStringLiteral("7z"), QStringLiteral("gz"), QStringLiteral("xz"),
    QStringLiteral("bz2"), QStringLiteral("tar"), QStringLiteral("iso"), QStringLiteral("exe"), QStringLiteral("msi"),
    QStringLiteral("apk"), QStringLiteral("dmg"), QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("avi"),
    QStringLiteral("mov"), QStringLiteral("webm"), QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("wav"),
    QStringLiteral("m4a"), QStringLiteral("pdf"), QStringLiteral("epub"), QStringLiteral("deb"), QStringLiteral("rpm"),
    QStringLiteral("img"), QStringLiteral("bin"), QStringLiteral("torrent")};
} // namespace

// ------------------------------------------------------------------ actions

QVariantList AppController::speedHistory() const
{
    QVariantList out;
    out.reserve(m_speedHistory.size());
    for (double v : m_speedHistory)
        out << v;
    return out;
}

QVariantMap AppController::usage() const { return m_mgr->usage(); }

QString AppController::verifyChecksum(qint64 id, const QString& expected) { return m_mgr->verifyChecksum(byId(id), expected); }
bool AppController::isChecksum(const QString& text) const { return Checksum::parse(text).valid(); }

QString AppController::addDownloads(const QString& text, const QString& folder, const QString& fileName, bool start,
                                    const QVariantMap& schedule, int existingFile, const QString& checksum)
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList urls = BatchLinks::expandAll(text.split(ws, Qt::SkipEmptyParts));
    if (urls.isEmpty())
        return tr("Enter at least one link.");
    QString dir = folder.trimmed();
    if (dir.isEmpty())
        dir = defaultFolder();
    if (!QDir().mkpath(dir))
        return tr("Cannot create folder %1").arg(dir);
    // Metalink files (dropped or opened): they stand for the downloads they list.
    QStringList links;
    QList<QString> metalinks;
    for (const QString& u : urls) {
        const QString local = u.startsWith(QLatin1String("file:"), Qt::CaseInsensitive) ? QUrl(u).toLocalFile()
                              : QFileInfo(u).isAbsolute()                               ? u : QString();
        if (!local.isEmpty() && QFileInfo(local).isFile() && Metalink::looksLike(local))
            metalinks << local;
        else
            links << u;
    }
    for (const QString& u : links)
        if (!m_mgr->canHandle(u))
            return tr("Unsupported link: %1").arg(u);
    for (const QString& m : metalinks)
        if (m_mgr->addFromMetalink(m, QDir::toNativeSeparators(dir), start).isEmpty())
            return tr("The Metalink file %1 lists nothing FluxDM can download").arg(QFileInfo(m).fileName());
    if (!checksum.trimmed().isEmpty() && !Checksum::parse(checksum).valid())
        return tr("The checksum isn't an MD5, SHA-1, SHA-256 or SHA-512 value.");

    const bool sched = schedule.value(QStringLiteral("enabled")).toBool();
    const ScheduleWindow w = ScheduleWindow::fromMap(schedule);
    if (sched)
        m_ui.setValue(QStringLiteral("lastSchedule"), w.toMap());
    Download* last = nullptr;
    for (const QString& u : links) {
        const QVariantMap meta = m_browserMeta.take(u); // cookies etc. captured by the browser extension
        QString name = links.size() == 1 ? fileName.trimmed() : QString();
        last = m_mgr->add(u, QDir::toNativeSeparators(dir), name, start || sched,
                          meta.value(QStringLiteral("referrer")).toString(), meta.value(QStringLiteral("cookies")).toString(),
                          meta.value(QStringLiteral("userAgent")).toString(), sched ? &w : nullptr);
        if (last && existingFile >= 0)
            m_mgr->setExistingFileReaction(last, existingFile);
        if (last && links.size() == 1 && !checksum.trimmed().isEmpty())
            m_mgr->setExpectedChecksum(last, checksum);
        if (last && !meta.value(QStringLiteral("postData")).toString().isEmpty()) {
            last->rec.postData = meta.value(QStringLiteral("postData")).toString().toUtf8();
            last->addLog(tr("The server sends this file in answer to a form: FluxDM submits the same form"));
        }
    }
    rememberFolder(dir);
    if (last)
        select(last->id());
    return {};
}

QVariantMap AppController::duplicateOf(const QString& text) const
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    QVariantMap out;
    int count = 0;
    for (const QString& u : BatchLinks::expandAll(text.split(ws, Qt::SkipEmptyParts))) {
        Download* d = m_mgr->findByUrl(u);
        if (!d)
            continue;
        if (++count == 1) {
            out[QStringLiteral("id")] = d->id();
            out[QStringLiteral("title")] = d->title();
            out[QStringLiteral("state")] = d->stateText();
        }
    }
    if (count)
        out[QStringLiteral("count")] = count;
    return out;
}

QString AppController::addSilently(const QString& text, const QString& folder, const QString& fileName)
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    QStringList fresh;
    Download* existing = nullptr;
    for (const QString& u : BatchLinks::expandAll(text.split(ws, Qt::SkipEmptyParts))) {
        Download* d = m_mgr->findByUrl(u);
        if (d && !d->completed() && d->rec.state != Download::Error) {
            existing = d;
            continue;
        }
        fresh << u;
    }
    if (fresh.isEmpty() && existing) {
        select(existing->id());
        emit notice(tr("Already in the list: %1").arg(existing->title()));
        return {};
    }
    if (fresh.isEmpty())
        return tr("Enter at least one link.");
    return addDownloads(fresh.join(QLatin1Char('\n')), folder, fresh.size() == 1 ? fileName : QString(), true);
}

void AppController::selectionUpdated()
{
    ++m_selRev;
    emit selectionChanged();
}

void AppController::select(qint64 id)
{
    Download* d = byId(id);
    m_sel.clear();
    if (d)
        m_sel.append(id);
    selectionUpdated();
    if (m_selected == d)
        return;
    m_selected = d;
    emit selectedChanged();
}

void AppController::toggleSelect(qint64 id)
{
    Download* d = byId(id);
    if (!d)
        return;
    if (m_sel.contains(id)) {
        m_sel.removeAll(id);
        if (m_selected == d) {
            m_selected = m_sel.isEmpty() ? nullptr : byId(m_sel.last());
            emit selectedChanged();
        }
    } else {
        m_sel.append(id);
        m_selected = d;
        emit selectedChanged();
    }
    selectionUpdated();
}

void AppController::selectRange(qint64 id)
{
    const int to = m_proxy->rowOf(id);
    const int from = m_selected ? m_proxy->rowOf(m_selected->id()) : -1;
    if (to < 0)
        return;
    if (from < 0) {
        select(id);
        return;
    }
    m_sel.clear();
    for (int r = qMin(from, to); r <= qMax(from, to); ++r)
        m_sel.append(m_proxy->idAt(r));
    selectionUpdated(); // the current item stays where the range started
}

void AppController::selectAll()
{
    m_sel.clear();
    for (int r = 0; r < m_proxy->rowCount(); ++r)
        m_sel.append(m_proxy->idAt(r));
    if (!m_selected && !m_sel.isEmpty()) {
        m_selected = byId(m_sel.first());
        emit selectedChanged();
    }
    selectionUpdated();
}

void AppController::clearSelection()
{
    m_sel.clear();
    selectionUpdated();
    if (m_selected) {
        m_selected = nullptr;
        emit selectedChanged();
    }
}

QVariantList AppController::selectedIds() const
{
    QVariantList out;
    for (qint64 id : m_sel)
        out << id;
    return out;
}

void AppController::pauseSelected()
{
    for (qint64 id : m_sel)
        if (Download* d = byId(id); d && (d->running() || d->queued()))
            m_mgr->pause(d);
}

void AppController::resumeSelected()
{
    for (qint64 id : m_sel)
        m_mgr->resume(byId(id));
}

void AppController::removeSelected(bool deleteFiles)
{
    const QList<qint64> ids = m_sel; // remove() prunes m_sel through downloadRemoved
    for (qint64 id : ids)
        m_mgr->remove(byId(id), deleteFiles);
}

void AppController::copySelectedLinks()
{
    QStringList links;
    for (qint64 id : m_sel)
        if (Download* d = byId(id))
            links << d->sourceUrl();
    m_lastClip = links.join(QLatin1Char('\n'));
    QGuiApplication::clipboard()->setText(m_lastClip);
}

void AppController::pause(qint64 id) { m_mgr->pause(byId(id)); }
void AppController::resume(qint64 id) { m_mgr->resume(byId(id)); }

void AppController::togglePause(qint64 id)
{
    Download* d = byId(id);
    if (!d)
        return;
    if (d->completed() && d->seeding())
        m_mgr->stopSeeding(d);
    else if (d->running() || d->queued())
        m_mgr->pause(d);
    else
        m_mgr->resume(d);
}

void AppController::restart(qint64 id) { m_mgr->restart(byId(id)); }
void AppController::remove(qint64 id, bool deleteFiles) { m_mgr->remove(byId(id), deleteFiles); }
void AppController::setPriority(qint64 id, int priority) { m_mgr->setPriority(byId(id), priority); }
void AppController::setDownloadSpeedLimit(qint64 id, int kbps) { m_mgr->setSpeedLimit(byId(id), kbps); }
void AppController::pauseAll() { m_mgr->pauseAll(); }
void AppController::resumeAll() { m_mgr->resumeAll(); }

// What "Open" would act on, or an error explaining why it can't.
static QString openablePath(Download* d, QString* error)
{
    if (!d)
        return {};
    if (!d->completed()) {
        *error = QCoreApplication::translate("AppController", "The download hasn't finished yet");
        return {};
    }
    if (d->converting() || d->moving()) {
        *error = QCoreApplication::translate("AppController", "Wait until the file is ready");
        return {};
    }
    const QString path = d->filePath();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        d->setFileMissing(true);
        *error = QCoreApplication::translate("AppController", "The file was moved or deleted");
        return {};
    }
    return path;
}

QString AppController::openFile(qint64 id)
{
    const QString err = doOpen(id);
    if (!err.isEmpty())
        emit notice(err);
    return err;
}

QString AppController::openWith(qint64 id)
{
    const QString err = doOpenWith(id);
    if (!err.isEmpty())
        emit notice(err);
    return err;
}

QString AppController::doOpen(qint64 id)
{
    QString error;
    const QString path = openablePath(byId(id), &error);
    if (path.isEmpty())
        return error;
    if (QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        return {};
    // No program is associated with this type: let the user pick one.
    return QFileInfo(path).isFile() ? doOpenWith(id) : tr("Cannot open %1").arg(path);
}

QString AppController::doOpenWith(qint64 id)
{
    QString error;
    const QString path = openablePath(byId(id), &error);
    if (path.isEmpty())
        return error;
    if (!QFileInfo(path).isFile())
        return tr("“Open with” works on files, not folders");
#ifdef Q_OS_WIN
    // OpenAs_RunDLL takes the rest of the command line as the path: pass it unquoted.
    QProcess p;
    p.setProgram(QStringLiteral("rundll32.exe"));
    p.setNativeArguments(QStringLiteral("shell32.dll,OpenAs_RunDLL ") + QDir::toNativeSeparators(path));
    return p.startDetached() ? QString() : tr("Cannot show the “Open with” dialog");
#else
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path)) ? QString() : tr("Cannot open %1").arg(path);
#endif
}

static bool isWebLink(const QString& s)
{
    return s.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) || s.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
}

// The referrer is the page the link was on (browser integration, HLS sniffing). For a torrent
// fetched from the web it is the .torrent link itself, which is not a page.
bool AppController::hasPage(qint64 id) const
{
    const Download* d = byId(id);
    return d && !d->isTorrent() && isWebLink(d->referrer()) && d->referrer() != d->url();
}

void AppController::openPage(qint64 id)
{
    if (hasPage(id))
        QDesktopServices::openUrl(QUrl(byId(id)->referrer()));
}

void AppController::copyPath(qint64 id)
{
    if (Download* d = byId(id)) {
        m_lastClip = d->filePath().isEmpty() ? d->destDir() : d->filePath();
        QGuiApplication::clipboard()->setText(m_lastClip);
    }
}

void AppController::watch(Download* d)
{
    connect(d, &Download::stateChanged, this, &AppController::scheduleRecount);
    connect(d, &Download::categoryChanged, this, &AppController::scheduleRecount);
    connect(d, &Download::tagsChanged, this, &AppController::scheduleRecount);
    connect(d, &Download::stateChanged, this, [this, d] {
        if (d->needsLogin())
            emit loginRequired(d);
        else if (d->fileExists())
            emit fileConflict(d);
        else if (d->certificateProblem())
            emit certificateRequired(d);
        else if (d->awaitingFiles())
            emit filesRequired(d);
    });
}

QString AppController::loginServer(qint64 id) const
{
    const Download* d = byId(id);
    return d ? DownloadManager::loginServer(d->url()) : QString();
}

QString AppController::loginUser(qint64 id) const
{
    const Download* d = byId(id);
    QString user, password;
    if (d)
        m_mgr->loginFor(d->url(), &user, &password);
    return user;
}

void AppController::signIn(qint64 id, const QString& user, const QString& password, bool remember)
{
    Download* d = byId(id);
    if (!d || user.trimmed().isEmpty())
        return;
    m_mgr->setLogin(d->url(), user.trimmed(), password, remember);
    d->addLog(tr("Signing in as %1").arg(user.trimmed()));
    m_mgr->resume(d);
    emit settingsChanged(); // saved-login count
}

int AppController::existingFileDefault() const { return m_mgr->existingFileDefault(); }

void AppController::setExistingFileDefault(int reaction)
{
    m_mgr->setExistingFileDefault(reaction);
    emit settingsChanged();
}

bool AppController::fileExistsIn(const QString& folder, const QString& name) const
{
    const QString n = name.trimmed();
    return !n.isEmpty() && QFileInfo::exists(QDir(folder.trimmed().isEmpty() ? defaultFolder() : folder.trimmed()).filePath(n));
}

void AppController::resolveExisting(qint64 id, int reaction)
{
    Download* d = byId(id);
    if (!d || !d->fileExists() || (reaction != FileNamer::KeepBoth && reaction != FileNamer::Overwrite))
        return;
    m_mgr->setExistingFileReaction(d, reaction);
    d->addLog(reaction == FileNamer::Overwrite ? tr("Replacing the existing file") : tr("Keeping both files"));
    m_mgr->resume(d);
}

QString AppController::mirrors(qint64 id) const
{
    const Download* d = byId(id);
    return d ? d->rec.mirrors.join(QLatin1Char('\n')) : QString();
}

QString AppController::setMirrors(qint64 id, const QString& text)
{
    Download* d = byId(id);
    if (!d)
        return {};
    QStringList list;
    for (const QString& l : text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
        const QUrl u(l);
        if (!u.isValid() || (u.scheme() != QLatin1String("http") && u.scheme() != QLatin1String("https") && u.scheme() != QLatin1String("ftp")))
            return tr("Not a web or FTP address: %1").arg(l);
        if (l != d->rec.url && !list.contains(l))
            list << l;
    }
    d->rec.mirrors = list;
    d->addLog(list.isEmpty() ? tr("Mirrors removed") : tr("%n mirror(s) set; used from the next start", "", int(list.size())));
    m_mgr->persistRecord(d);
    return {};
}

void AppController::trustCertificate(qint64 id) { m_mgr->trustCertificate(byId(id)); }
int AppController::trustedHostCount() const { return int(m_mgr->trustedHosts().size()); }

void AppController::forgetTrustedHosts()
{
    m_mgr->forgetTrustedHosts();
    emit settingsChanged();
}

int AppController::savedLoginCount() const { return m_mgr->savedLoginCount(); }

void AppController::forgetLogins()
{
    m_mgr->forgetLogins();
    emit settingsChanged();
}

QString AppController::rename(qint64 id, const QString& newName) { return m_mgr->rename(byId(id), newName); }
bool AppController::canRename(qint64 id) const { return DownloadManager::canRename(byId(id)); }

QString AppController::openSelected()
{
    constexpr int kMax = 10; // don't launch a wall of players by accident
    int opened = 0, skipped = 0;
    for (qint64 id : m_sel) {
        Download* d = byId(id);
        if (!d || !d->completed() || opened >= kMax) {
            ++skipped;
            continue;
        }
        if (doOpen(id).isEmpty())
            ++opened;
        else
            ++skipped;
    }
    const QString msg = opened == 0 ? tr("None of the selected downloads can be opened yet")
        : skipped ? tr("Opened %1, skipped %2 (not finished, missing, or over %3)").arg(opened).arg(skipped).arg(kMax)
        : QString();
    if (!msg.isEmpty())
        emit notice(msg);
    return msg;
}

void AppController::moveInQueue(qint64 id, const QString& where)
{
    if (Download* d = byId(id))
        m_mgr->moveInQueue({d}, where);
}

void AppController::moveSelectionInQueue(const QString& where)
{
    QList<Download*> ds;
    for (qint64 id : m_sel)
        if (Download* d = byId(id))
            ds << d;
    m_mgr->moveInQueue(ds, where);
}

void AppController::setPrioritySelected(int priority)
{
    for (qint64 id : m_sel)
        m_mgr->setPriority(byId(id), priority);
}

void AppController::openFolder(qint64 id)
{
    Download* d = byId(id);
    if (!d)
        return;
    QString path = d->filePath();
    if (!path.isEmpty() && !d->completed())
        path += FileNamer::kPartSuffix;
#ifdef Q_OS_WIN
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        QProcess::startDetached(QStringLiteral("explorer.exe"),
                                {QStringLiteral("/select,") + QDir::toNativeSeparators(path)});
        return;
    }
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(d->destDir()));
}

void AppController::copyLink(qint64 id)
{
    if (Download* d = byId(id)) { // a torrent from a downloaded .torrent: its web link, not the temp file
        m_lastClip = d->sourceUrl();
        QGuiApplication::clipboard()->setText(m_lastClip);
    }
}

void AppController::quit() { QCoreApplication::quit(); }

void AppController::notifyHiddenToTray()
{
    if (m_trayHintShown)
        return;
    m_trayHintShown = true;
    emit hiddenToTray();
}

// ------------------------------------------------------------------ links / torrents

int AppController::countLinks(const QString& text) const
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    return int(BatchLinks::expandAll(text.split(ws, Qt::SkipEmptyParts)).size());
}

bool AppController::isTorrentLink(const QString& text) const
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList l = text.split(ws, Qt::SkipEmptyParts);
    return !l.isEmpty() && m_mgr->moduleFor(l.first()) == QLatin1String("downloadsbt");
}

void AppController::stopSeeding(qint64 id) { m_mgr->stopSeeding(byId(id)); }
void AppController::setFileWanted(qint64 id, int index, bool wanted) { m_mgr->setFileWanted(byId(id), index, wanted); }

// ------------------------------------------------------------------ scheduling

QVariantList AppController::trafficRules() const
{
    QVariantList out;
    for (const TrafficRule& r : m_mgr->trafficRules())
        out << r.toMap();
    return out;
}

void AppController::setTrafficRules(const QVariantList& rules)
{
    QList<TrafficRule> list;
    for (const QVariant& v : rules)
        list << TrafficRule::fromMap(v.toMap());
    m_mgr->setTrafficRules(list);
}

QVariantMap AppController::downloadSchedule(qint64 id) const
{
    Download* d = byId(id);
    QVariantMap m = d && d->scheduled() ? d->rec.schedule.toMap() : defaultSchedule();
    m[QStringLiteral("enabled")] = d && d->scheduled();
    return m;
}

void AppController::setDownloadSchedule(qint64 id, bool enabled, const QVariantMap& window)
{
    const ScheduleWindow w = ScheduleWindow::fromMap(window);
    if (enabled)
        m_ui.setValue(QStringLiteral("lastSchedule"), w.toMap());
    m_mgr->setSchedule(byId(id), enabled, w);
}

QVariantMap AppController::defaultSchedule() const
{
    // Last used window, else FDM's classic "night" window 01:00-07:00 every day.
    QVariantMap m = m_ui.value(QStringLiteral("lastSchedule")).toMap();
    if (m.isEmpty())
        m = ScheduleWindow{0x7F, 60, 7 * 60}.toMap();
    m[QStringLiteral("enabled")] = false;
    return m;
}

QString AppController::scheduleText(const QVariantMap& window) const { return ScheduleWindow::fromMap(window).text(); }

void AppController::performWhenDone(int action)
{
    setWhenDone(0); // one-shot, like FDM
    if (qEnvironmentVariableIsSet("FLUXDM_DEV_OPEN") || qEnvironmentVariableIsSet("FLUXDM_DEV_DEMO")) {
        QCoreApplication::quit(); // dev/screenshot runs must never sleep or shut the machine down
        return;
    }
#ifdef Q_OS_WIN
    switch (action) {
    case 1: SetSuspendState(FALSE, FALSE, FALSE); return;
    case 2: SetSuspendState(TRUE, FALSE, FALSE); return;
    case 3:
        m_mgr->shutdown();
        QProcess::startDetached(QStringLiteral("shutdown.exe"), {QStringLiteral("/s"), QStringLiteral("/t"), QStringLiteral("0")});
        QCoreApplication::quit();
        return;
    default: break;
    }
#endif
    if (action == 4 || action == 3)
        QCoreApplication::quit();
}
