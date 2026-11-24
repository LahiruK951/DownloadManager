// AppController: properties and settings (the rest is in AppController*.cpp).
#include "ui/AppController.h"

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

// ------------------------------------------------------------------ properties

QAbstractItemModel* AppController::downloads() const { return m_proxy; }
QObject* AppController::selected() const { return m_selected.data(); }
QString AppController::filter() const { return m_proxy->filter(); }
QString AppController::search() const { return m_proxy->search(); }

void AppController::setFilter(const QString& f)
{
    if (f == m_proxy->filter())
        return;
    m_proxy->setFilter(f);
    emit filterChanged();
}

void AppController::setSearch(const QString& s)
{
    if (s == m_proxy->search())
        return;
    m_proxy->setSearch(s);
    emit searchChanged();
}

int AppController::trafficMode() const { return m_mgr->trafficMode(); }
void AppController::setTrafficMode(int m) { m_mgr->setTrafficMode(m); }
bool AppController::trafficScheduled() const { return m_mgr->trafficModeScheduled(); }

void AppController::setWhenDone(int a)
{
    a = qBound(0, a, 4);
    if (a == m_whenDone)
        return;
    m_whenDone = a;
    emit whenDoneChanged();
}

bool AppController::torrentSupport() const { return !m_mgr->moduleFor(QStringLiteral("magnet:?xt=urn:btih:0000000000000000000000000000000000000000")).isEmpty(); }

#define UI_SETTING(getter, setter, Type, key, def, conv)                 \
    Type AppController::getter() const { return m_ui.value(QStringLiteral(key), def).conv(); } \
    void AppController::setter(Type v)                                   \
    {                                                                    \
        if (getter() == v)                                               \
            return;                                                      \
        m_ui.setValue(QStringLiteral(key), v);                           \
        emit settingsChanged();                                          \
    }

UI_SETTING(compactView, setCompactView, bool, "compactView", false, toBool)
UI_SETTING(closeButtonHidesApp, setCloseButtonHidesApp, bool, "closeButtonHidesApp", true, toBool)
UI_SETTING(monitorClipboard, setMonitorClipboard, bool, "monitorClipboard", true, toBool)
UI_SETTING(notifyOnComplete, setNotifyOnComplete, bool, "notifyOnComplete", true, toBool)

QString AppController::sortKey() const { return m_proxy->sortKey(); }
bool AppController::sortDescending() const { return m_proxy->sortDescending(); }

void AppController::setSort(const QString& key, bool descending)
{
    m_proxy->setSortOrder(key, descending);
    m_ui.setValue(QStringLiteral("sortKey"), m_proxy->sortKey());
    m_ui.setValue(QStringLiteral("sortDescending"), descending);
    emit sortChanged();
}

// The torrent's largest wanted audio/video file: {path, size, done}, empty if none.
QVariantMap AppController::playableFile(const Download* d) const
{
    if (!d || !d->isTorrent())
        return {};
    QVariantMap best;
    const bool folder = d->files().size() > 1 || QFileInfo(d->filePath()).isDir();
    for (const QVariant& v : d->files()) {
        const QVariantMap f = v.toMap();
        const QString name = f.value(QStringLiteral("name")).toString();
        const QString cat = FileNamer::category(name);
        if ((cat != QLatin1String("Video") && cat != QLatin1String("Music")) || !f.value(QStringLiteral("wanted"), true).toBool())
            continue;
        if (f.value(QStringLiteral("size")).toLongLong() <= best.value(QStringLiteral("size"), -1).toLongLong())
            continue;
        best = {{QStringLiteral("path"), folder ? QDir(d->filePath()).filePath(name) : d->filePath()},
                {QStringLiteral("size"), f.value(QStringLiteral("size"))},
                {QStringLiteral("done"), f.value(QStringLiteral("done"))}};
    }
    return best;
}

bool AppController::canPlayWhileDownloading(qint64 id) const
{
    const Download* d = byId(id);
    return d && d->isTorrent() && d->running() && !playableFile(d).isEmpty();
}

void AppController::playWhileDownloading(qint64 id)
{
    Download* d = byId(id);
    if (!canPlayWhileDownloading(id))
        return;
    m_mgr->setSequential(d, true); // the player needs the file from its start
    if (!m_playWhenReady.contains(id))
        m_playWhenReady << id;
    emit notice(tr("Getting the start of the file first; it opens in your player in a moment"));
    updateStats();
}

QVariantMap AppController::proxySettings() const { return m_mgr->proxySettings().toMap(); }

QString AppController::setProxySettings(int mode, const QString& type, const QString& host, int port,
                                        const QString& user, const QString& password, bool keepPassword)
{
    ProxySettings ps = m_mgr->proxySettings();
    ps.mode = qBound(0, mode, 2);
    if (ps.mode == ProxySettings::Manual) {
        if (host.trimmed().isEmpty())
            return tr("Enter the proxy's address.");
        if (port <= 0 || port > 65535)
            return tr("Enter a port between 1 and 65535.");
        ps.type = type == QLatin1String("socks5") ? QStringLiteral("socks5") : QStringLiteral("http");
        ps.host = host.trimmed();
        ps.port = port;
        ps.user = user.trimmed();
        if (!keepPassword || !password.isEmpty())
            ps.password = ps.user.isEmpty() ? QString() : password;
    }
    m_mgr->setProxySettings(ps);
    emit settingsChanged();
    return {};
}

int AppController::maxConnectionsPerServer() const { return m_mgr->maxConnectionsPerServer(); }
void AppController::setMaxConnectionsPerServer(int n) { m_mgr->setMaxConnectionsPerServer(n); }

QVariantMap AppController::convertDefaults(const QString& format) const
{
    const QString p = format == QLatin1String("mp4") ? QStringLiteral("mp4Converter") : QStringLiteral("mp3Converter");
    return {{QStringLiteral("vbr"), !m_ui.value(p + QStringLiteral("ConstantBitrateEnabled"), true).toBool()},
            {QStringLiteral("bitrate"), m_ui.value(p + QStringLiteral("ConstantBitrate"), 192).toInt()},
            {QStringLiteral("quality"), m_ui.value(p + QStringLiteral("VariableQuality"), 2).toInt()},
            {QStringLiteral("folder"), m_ui.value(p + QStringLiteral("DestinationDir")).toString()}};
}

bool AppController::canConvert(qint64 id, const QString& format) const
{
    const QString f = DownloadManager::convertibleFile(byId(id));
    if (f.isEmpty())
        return false;
    if (format == QLatin1String("mp4"))
        return FileNamer::category(f) == QLatin1String("Video") && QFileInfo(f).suffix().compare(QLatin1String("mp4"), Qt::CaseInsensitive) != 0;
    return QFileInfo(f).suffix().compare(QLatin1String("mp3"), Qt::CaseInsensitive) != 0;
}

QString AppController::convertFile(qint64 id, const QString& format, const QVariantMap& options)
{
    ConvertOptions o;
    o.format = format == QLatin1String("mp4") ? QStringLiteral("mp4") : QStringLiteral("mp3");
    o.vbr = options.value(QStringLiteral("vbr")).toBool();
    o.bitrateKbps = options.value(QStringLiteral("bitrate"), 192).toInt();
    o.vbrQuality = options.value(QStringLiteral("quality"), 2).toInt();
    o.outputDir = options.value(QStringLiteral("folder")).toString().trimmed();
    // FDM's settings keys, so the next conversion starts from the same choices.
    const QString p = o.format == QLatin1String("mp4") ? QStringLiteral("mp4Converter") : QStringLiteral("mp3Converter");
    m_ui.setValue(p + QStringLiteral("ConstantBitrateEnabled"), !o.vbr);
    m_ui.setValue(p + QStringLiteral("ConstantBitrate"), o.bitrateKbps);
    m_ui.setValue(p + QStringLiteral("VariableQuality"), o.vbrQuality);
    m_ui.setValue(p + QStringLiteral("DestinationDir"), o.outputDir);
    const QString err = m_mgr->convertFile(byId(id), o);
    if (!err.isEmpty())
        emit notice(err);
    return err;
}

QVariantList AppController::tags() const
{
    QVariantList l;
    for (const Tag& t : m_mgr->tags())
        l << t.toMap();
    return l;
}

bool AppController::tagNameTaken(const QString& name, int exceptId) const
{
    for (const Tag& t : m_mgr->tags())
        if (t.id != exceptId && t.name.compare(name.trimmed(), Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

int AppController::saveTag(int id, const QString& name, const QString& color, const QString& extensions, const QString& folder)
{
    if (name.trimmed().isEmpty() || tagNameTaken(name, id))
        return 0;
    Tag t = id > 0 ? m_mgr->tag(id) : Tag();
    t.id = id > 0 ? t.id : 0;
    t.name = name.trimmed();
    t.color = color;
    t.extensions = Tag::parseExtensions(extensions);
    t.folder = folder.trimmed().isEmpty() ? QString() : QDir::toNativeSeparators(folder.trimmed());
    return m_mgr->saveTag(t);
}

void AppController::removeTag(int id) { m_mgr->removeTag(id); }
void AppController::setTag(qint64 downloadId, int tagId, bool on) { m_mgr->setTag(byId(downloadId), tagId, on); }

void AppController::setSelectionTag(int tagId, bool on)
{
    for (qint64 id : m_sel)
        m_mgr->setTag(byId(id), tagId, on);
}

int AppController::selectionTagState(int tagId) const
{
    int with = 0;
    for (qint64 id : m_sel)
        if (const Download* d = byId(id); d && d->hasTag(tagId))
            ++with;
    return with == 0 ? 0 : (with == m_sel.size() ? 2 : 1);
}

QString AppController::torrentWatchFolder() const { return m_ui.value(QStringLiteral("torrentWatchFolder")).toString(); }

void AppController::setTorrentWatchFolder(const QString& folder)
{
    const QString f = folder.trimmed().isEmpty() ? QString() : QDir::toNativeSeparators(folder.trimmed());
    if (f == torrentWatchFolder())
        return;
    m_ui.setValue(QStringLiteral("torrentWatchFolder"), f);
    m_torrentWatcher->setFolder(f);
    emit settingsChanged();
}

int AppController::meteredPolicy() const
{
    // FDM's "dontAskMobileDataUsage" means: download as usual.
    if (m_ui.value(QStringLiteral("dontAskMobileDataUsage")).toBool() && !m_ui.contains(QStringLiteral("meteredPolicy")))
        return MeteredGuard::Ignore;
    return qBound(0, m_ui.value(QStringLiteral("meteredPolicy"), int(MeteredGuard::Ask)).toInt(), 2);
}

void AppController::setMeteredPolicy(int p)
{
    m_ui.setValue(QStringLiteral("meteredPolicy"), qBound(0, p, 2));
    if (m_metered)
        m_metered->setPolicy(meteredPolicy());
    emit settingsChanged();
}

void AppController::pauseOnMetered(bool remember)
{
    if (remember)
        setMeteredPolicy(MeteredGuard::Pause);
    m_metered->pauseNow();
}

void AppController::keepDownloadingOnMetered(bool remember)
{
    if (remember)
        setMeteredPolicy(MeteredGuard::Ignore);
    m_metered->keepDownloading();
}

bool AppController::antivirusCheck() const { return m_mgr->antivirusCheck(); }

void AppController::setAntivirusCheck(bool on)
{
    m_mgr->setAntivirusCheck(on);
    emit settingsChanged();
}

bool AppController::chooseTorrentFiles() const { return m_mgr->chooseTorrentFiles(); }

void AppController::setChooseTorrentFiles(bool on)
{
    m_mgr->setChooseTorrentFiles(on);
    emit settingsChanged();
}

void AppController::chooseFiles(qint64 id, const QVariantList& wantedIndexes)
{
    QList<int> wanted;
    for (const QVariant& v : wantedIndexes)
        wanted << v.toInt();
    m_mgr->chooseFiles(byId(id), wanted);
}

void AppController::setSequential(qint64 id, bool on) { m_mgr->setSequential(byId(id), on); }
QVariantMap AppController::torrentDetails(qint64 id) const { return m_mgr->details(byId(id)); }

QString AppController::updateUrl() const
{
    return m_ui.value(QStringLiteral("updateUrl"), QStringLiteral(FLUXDM_UPDATE_URL)).toString();
}

void AppController::setUpdateUrl(const QString& url)
{
    m_ui.setValue(QStringLiteral("updateUrl"), url.trimmed());
    emit updateChanged();
}

bool AppController::autoCheckUpdates() const { return m_ui.value(QStringLiteral("autoCheckUpdates"), true).toBool(); }

void AppController::setAutoCheckUpdates(bool on)
{
    m_ui.setValue(QStringLiteral("autoCheckUpdates"), on);
    emit updateChanged();
}

void AppController::setUpdate(int state, const QString& status)
{
    m_updateState = state;
    m_updateStatus = status;
    emit updateChanged();
}

void AppController::checkForUpdates()
{
    if (updateUrl().isEmpty()) {
        setUpdate(5, tr("No update address is set."));
        return;
    }
    setUpdate(1, tr("Checking…"));
    m_updater->check(QUrl(updateUrl()));
}

void AppController::downloadUpdate()
{
    setUpdate(3, tr("Downloading the update…"));
    m_updater->download();
}

void AppController::installUpdate()
{
    if (m_updateFile.isEmpty())
        return;
    if (Updater::install(m_updateFile))
        QCoreApplication::quit(); // the installer replaces the files and starts FluxDM again
    else
        setUpdate(5, tr("Couldn't start the installer."));
}

bool AppController::remoteEnabled() const { return m_ui.value(QStringLiteral("remoteEnabled"), false).toBool(); }
int AppController::remotePort() const { return m_ui.value(QStringLiteral("remotePort"), 6810).toInt(); }
QString AppController::remoteCode() const { return m_remote ? m_remote->accessCode() : QString(); }

QStringList AppController::remoteAddresses() const
{
    QStringList out;
    if (m_remote && m_remote->isRunning())
        for (const QString& ip : RemoteServer::lanAddresses())
            out << QStringLiteral("http://%1:%2").arg(ip).arg(m_remote->port());
    return out;
}

void AppController::setRemoteEnabled(bool on)
{
    m_ui.setValue(QStringLiteral("remoteEnabled"), on);
    applyRemote();
}

void AppController::setRemotePort(int port)
{
    m_ui.setValue(QStringLiteral("remotePort"), qBound(1024, port, 65535));
    applyRemote();
}

void AppController::newRemoteCode()
{
    const QString code = RemoteServer::newAccessCode();
    m_ui.setValue(QStringLiteral("remoteCodeProtected"), Secret::protect(code).toBase64());
    m_remote->setAccessCode(code); // signed-in phones have to sign in again
    emit remoteChanged();
}

void AppController::applyRemote()
{
    m_remoteError.clear();
    m_remote->stop();
    if (remoteEnabled() && !m_remote->start(quint16(remotePort())))
        m_remoteError = tr("Port %1 is in use by another program. Choose another one.").arg(remotePort());
    emit remoteChanged();
}

bool AppController::previousSessionCrashed() const { return AppLog::previousSessionCrashed(); }

QString AppController::problemReport() const
{
    return AppLog::report(200) + QStringLiteral("\nDownloads: %1 in the list\n").arg(m_mgr->downloads().size());
}

void AppController::copyProblemReport()
{
    m_lastClip = problemReport();
    QGuiApplication::clipboard()->setText(m_lastClip);
    emit notice(tr("Report copied"));
}

QString AppController::saveProblemReport(const QUrl& file)
{
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return tr("Cannot write %1").arg(QDir::toNativeSeparators(path));
    f.write(problemReport().toUtf8());
    emit notice(tr("Saved %1").arg(QFileInfo(path).fileName()));
    return {};
}

void AppController::openLogsFolder() const { QDesktopServices::openUrl(QUrl::fromLocalFile(AppLog::logDir())); }

QString AppController::language() const { return m_ui.value(QStringLiteral("language")).toString(); }

void AppController::setLanguage(const QString& code)
{
    if (code == language())
        return;
    m_ui.setValue(QStringLiteral("language"), code);
    m_ui.sync();
    emit settingsChanged();
}

// "" (Windows' language), English, and every translation built into the app (:/i18n/fluxdm_<code>.qm).
QVariantList AppController::languages() const
{
    const QString sys = QLocale::system().nativeLanguageName();
    QVariantList out{QVariantMap{{QStringLiteral("code"), QString()}, {QStringLiteral("name"), tr("Windows language (%1)").arg(sys)}},
                     QVariantMap{{QStringLiteral("code"), QStringLiteral("en")}, {QStringLiteral("name"), QStringLiteral("English")}}};
    for (const QString& f : QDir(QStringLiteral(":/i18n")).entryList({QStringLiteral("fluxdm_*.qm")})) {
        const QString code = f.mid(7, f.size() - 10);
        QString name = QLocale(code).nativeLanguageName();
        if (!name.isEmpty())
            name[0] = name[0].toUpper();
        out << QVariantMap{{QStringLiteral("code"), code}, {QStringLiteral("name"), name}};
    }
    return out;
}

int AppController::zoom() const { return m_ui.value(QStringLiteral("zoom"), 100).toInt(); }

void AppController::setZoom(int percent)
{
    percent = qBound(50, percent, 300);
    if (percent == zoom())
        return;
    m_ui.setValue(QStringLiteral("zoom"), percent);
    m_ui.sync();
    emit settingsChanged();
}

int AppController::zoomInEffect() const
{
    const double f = qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1")).toDouble();
    return f > 0 ? int(std::lround(f * 100)) : 100;
}

void AppController::restartApp()
{
    m_ui.sync();
    QStringList args{QStringLiteral("--restarted")};
    qunsetenv("QT_SCALE_FACTOR"); // the new process reads the zoom setting itself
    if (QProcess::startDetached(QCoreApplication::applicationFilePath(), args))
        QCoreApplication::quit();
}

// Start with Windows: the per-user Run key, starting minimised to the notification area.
// Dev profiles (FLUXDM_DATA_DIR) never touch the real registry.
static const char* kRunKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static bool devProfile() { return qEnvironmentVariableIsSet("FLUXDM_DATA_DIR"); }

// Run by the uninstaller: undo only what belongs to this copy of FluxDM. Another copy (portable,
// a developer build) shares the profile and may have registered itself since.
void AppController::uninstallCleanup()
{
    setTorrentAssociated(false); // only acts when the association opens this exe
#ifdef Q_OS_WIN
    if (devProfile())
        return;
    const auto here = [](const QString& path) {
        return !path.isEmpty()
               && QFileInfo(path).absolutePath().compare(QCoreApplication::applicationDirPath(), Qt::CaseInsensitive) == 0;
    };
    QFile manifest(QDir(Instance::profileDir()).filePath(QStringLiteral("nativehost.json")));
    if (manifest.open(QIODevice::ReadOnly)) {
        const QString host = QJsonDocument::fromJson(manifest.readAll()).object().value(QStringLiteral("path")).toString();
        manifest.close();
        if (here(host))
            setBrowserIntegrated(false);
    }
    const QString run = QSettings(QString::fromLatin1(kRunKey), QSettings::NativeFormat).value(QStringLiteral("FluxDM")).toString();
    if (run.contains(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), Qt::CaseInsensitive))
        setStartWithWindows(false);
#endif
}

bool AppController::startWithWindows() const
{
#ifdef Q_OS_WIN
    if (devProfile())
        return m_ui.value(QStringLiteral("devStartWithWindows"), false).toBool();
    return QSettings(QString::fromLatin1(kRunKey), QSettings::NativeFormat).contains(QStringLiteral("FluxDM"));
#else
    return false;
#endif
}

void AppController::setStartWithWindows(bool on)
{
#ifdef Q_OS_WIN
    if (devProfile()) {
        m_ui.setValue(QStringLiteral("devStartWithWindows"), on);
    } else {
        QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
        if (on)
            run.setValue(QStringLiteral("FluxDM"), QStringLiteral("\"%1\" --tray").arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath())));
        else
            run.remove(QStringLiteral("FluxDM"));
    }
    emit settingsChanged();
#else
    Q_UNUSED(on)
#endif
}

QString AppController::userAgent() const { return m_mgr->userAgent(); }
QString AppController::defaultUserAgent() const { return DownloadManager::defaultUserAgent(); }
void AppController::setUserAgent(const QString& ua)
{
    m_mgr->setUserAgent(ua.trimmed() == DownloadManager::defaultUserAgent() ? QString() : ua.trimmed());
    emit settingsChanged();
}
int AppController::ipVersion() const { return m_mgr->ipVersion(); }
void AppController::setIpVersion(int v) { m_mgr->setIpVersion(v); emit settingsChanged(); }
int AppController::autoRemoveDays() const { return m_mgr->autoRemoveDays(); }
void AppController::setAutoRemoveDays(int days) { m_mgr->setAutoRemoveDays(days); emit settingsChanged(); }
bool AppController::extractArchives() const { return m_mgr->extractArchives(); }
void AppController::setExtractArchives(bool on) { m_mgr->setExtractArchives(on); emit settingsChanged(); }
QString AppController::extractorName() const
{
    const QString tool = m_mgr->extractorPath();
    if (tool.isEmpty())
        return {};
    return QFileInfo(tool).baseName().startsWith(QLatin1String("7z"), Qt::CaseInsensitive) ? QStringLiteral("7-Zip")
                                                                                            : tr("Windows (zip and tar only)");
}
bool AppController::canExtract(qint64 id) const { return m_mgr->canExtract(byId(id)); }
QString AppController::extract(qint64 id) { return m_mgr->extract(byId(id)); }
bool AppController::canCreateTorrent(qint64 id) const { return m_mgr->canCreateTorrent(byId(id)); }
QString AppController::createTorrent(qint64 id) { return m_mgr->createTorrent(byId(id)); }

int AppController::btListenPort() const { return m_mgr->btListenPort(); }
void AppController::setBtListenPort(int port) { m_mgr->setBtListenPort(port); emit settingsChanged(); }

bool AppController::preventSleep() const { return m_ui.value(QStringLiteral("preventSleep"), true).toBool(); }

void AppController::setPreventSleep(bool on)
{
    if (preventSleep() == on)
        return;
    m_ui.setValue(QStringLiteral("preventSleep"), on);
    m_sleepGuard.setActive(on && m_activeCount > 0);
    emit settingsChanged();
}
UI_SETTING(soundOnComplete, setSoundOnComplete, bool, "soundOnComplete", true, toBool)
UI_SETTING(bottomPanelOpen, setBottomPanelOpen, bool, "bottomPanelOpened", true, toBool)
UI_SETTING(bottomPanelTab, setBottomPanelTab, int, "bottomPanelCurrentTab", 0, toInt)
UI_SETTING(bottomPanelHeight, setBottomPanelHeight, double, "bottomPanelHeight", 230.0, toDouble)
#undef UI_SETTING

QString AppController::theme() const
{
    const QString forced = qEnvironmentVariable("FLUXDM_THEME"); // dev/screenshots
    return !forced.isEmpty() ? forced : m_ui.value(QStringLiteral("theme"), QStringLiteral("system")).toString();
}
void AppController::setTheme(const QString& t)
{
    if (t == theme())
        return;
    m_ui.setValue(QStringLiteral("theme"), t);
    emit settingsChanged();
}

QString AppController::defaultFolder() const
{
    return m_ui.value(QStringLiteral("defaultFolder"),
                      QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)))
        .toString();
}

void AppController::setDefaultFolder(const QString& f)
{
    if (f == defaultFolder())
        return;
    m_ui.setValue(QStringLiteral("defaultFolder"), QDir::toNativeSeparators(f));
    emit settingsChanged();
}

// The last 5 folders downloads were saved to, newest first (just the default folder before the first).
QStringList AppController::folderHistory() const
{
    QStringList h = m_ui.value(QStringLiteral("folderHistory")).toStringList();
    h.removeAll(QString());
    if (h.size() > kFolderHistorySize)
        h = h.mid(0, kFolderHistorySize);
    if (h.isEmpty())
        h << defaultFolder();
    return h;
}

void AppController::rememberFolder(const QString& dir)
{
    QStringList h = m_ui.value(QStringLiteral("folderHistory")).toStringList();
    const QString n = QDir::toNativeSeparators(dir);
    h.removeAll(n);
    h.prepend(n);
    while (h.size() > kFolderHistorySize)
        h.removeLast();
    m_ui.setValue(QStringLiteral("folderHistory"), h);
    emit settingsChanged();
}

QUrl AppController::settingsFileUrl() const { return QUrl::fromLocalFile(m_ui.fileName()); }
QString AppController::version() const { return QStringLiteral(FLUXDM_VERSION); }
