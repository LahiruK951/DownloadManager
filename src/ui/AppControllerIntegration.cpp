// AppController: BitTorrent settings, browser integration, folders by type, import/export.
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

// ------------------------------------------------------------------ BitTorrent settings

int AppController::uploadLimitKBps() const { return int(m_mgr->uploadLimitKBps()); }
void AppController::setUploadLimitKBps(int kbps) { m_mgr->setUploadLimitKBps(kbps); }
double AppController::seedRatio() const { return m_mgr->seedRatio(); }
void AppController::setSeedRatio(double r) { m_mgr->setSeedRatio(r); }

bool AppController::torrentAssociated() const
{
#ifdef Q_OS_WIN
    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"), QSettings::NativeFormat);
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return reg.value(QStringLiteral("magnet/shell/open/command/Default")).toString().contains(exe, Qt::CaseInsensitive);
#else
    return false;
#endif
}

void AppController::setTorrentAssociated(bool on)
{
#ifdef Q_OS_WIN
    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"), QSettings::NativeFormat);
    const QString cmd = QStringLiteral("\"%1\" \"%2\"").arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), QStringLiteral("%1"));
    if (on) {
        reg.setValue(QStringLiteral("magnet/Default"), QStringLiteral("URL:Magnet link"));
        reg.setValue(QStringLiteral("magnet/URL Protocol"), QString());
        reg.setValue(QStringLiteral("magnet/shell/open/command/Default"), cmd);
        reg.setValue(QStringLiteral(".torrent/Default"), QStringLiteral("FluxDM.torrent"));
        reg.setValue(QStringLiteral("FluxDM.torrent/Default"), QStringLiteral("BitTorrent file"));
        reg.setValue(QStringLiteral("FluxDM.torrent/shell/open/command/Default"), cmd);
    } else if (torrentAssociated()) {
        reg.remove(QStringLiteral("magnet"));
        reg.remove(QStringLiteral("FluxDM.torrent"));
        if (reg.value(QStringLiteral(".torrent/Default")).toString() == QLatin1String("FluxDM.torrent"))
            reg.remove(QStringLiteral(".torrent"));
    }
    reg.sync();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#else
    Q_UNUSED(on);
#endif
}

// ------------------------------------------------------------------ browser integration

bool AppController::browserAddSilently() const { return m_ui.value(QStringLiteral("browserAddSilently"), false).toBool(); }
void AppController::setBrowserAddSilently(bool v)
{
    if (v == browserAddSilently())
        return;
    m_ui.setValue(QStringLiteral("browserAddSilently"), v);
    emit settingsChanged();
}

bool AppController::startHidden() const { return QCoreApplication::arguments().contains(QStringLiteral("--tray")); }

QString AppController::suggestedName(const QString& url) const
{
    return m_browserMeta.value(url.trimmed()).value(QStringLiteral("fileName")).toString();
}

// Best guess of the saved name before the server is asked: the browser's name, else the link's.
// The server may still name it differently (Content-Disposition).
QString AppController::expectedName(const QString& text) const
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList links = text.split(ws, Qt::SkipEmptyParts);
    if (links.size() != 1)
        return {};
    const QString browser = suggestedName(links.first());
    return browser.isEmpty() ? FileNamer::sanitize(FileNamer::fromUrl(QUrl(links.first()))) : browser;
}

QString AppController::handleBrowserRequest(const QVariantMap& r)
{
    const QString cmd = r.value(QStringLiteral("cmd")).toString();
    if (cmd == QLatin1String("ping"))
        return {};
    if (cmd == QLatin1String("show")) {
        emit showWindowRequested();
        return {};
    }
    if (cmd == QLatin1String("downloadMany")) { // "Download all links on this page"
        QStringList urls;
        for (const QVariant& v : r.value(QStringLiteral("urls")).toList()) {
            const QString u = v.toString().trimmed();
            if (m_mgr->canHandle(u) && !QFileInfo(u).isAbsolute() && urls.size() < 500)
                urls << u;
        }
        if (urls.isEmpty())
            return tr("No links to download");
        const QVariantMap meta{{QStringLiteral("referrer"), r.value(QStringLiteral("referrer"))},
                               {QStringLiteral("userAgent"), r.value(QStringLiteral("userAgent"))}};
        for (const QString& u : urls)
            m_browserMeta.insert(u, meta);
        emit addDialogRequested(urls.join(QLatin1Char('\n')));
        emit showWindowRequested();
        return {};
    }
    if (cmd != QLatin1String("download"))
        return tr("Unknown command");
    const QString url = r.value(QStringLiteral("url")).toString().trimmed();
    if (!m_mgr->canHandle(url))
        return tr("Unsupported link");
    QVariantMap meta;
    for (const char* k : {"referrer", "cookies", "userAgent", "fileName", "postData"})
        meta.insert(QString::fromLatin1(k), r.value(QString::fromLatin1(k)));
    meta[QStringLiteral("fileName")] = FileNamer::sanitize(QFileInfo(meta.value(QStringLiteral("fileName")).toString()).fileName());
    if (meta.value(QStringLiteral("fileName")).toString() == QLatin1String("download"))
        meta.remove(QStringLiteral("fileName"));
    m_browserMeta.insert(url, meta);

    if (browserAddSilently() || r.value(QStringLiteral("silent")).toBool()) {
        const QString err = addSilently(url, folderFor(url), suggestedName(url));
        return err;
    }
    emit addDialogRequested(url);
    return {};
}

QString AppController::extensionDir() const
{
    return QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("browser/chromium")));
}

void AppController::openExtensionDir() const { QDesktopServices::openUrl(QUrl::fromLocalFile(extensionDir())); }

QString AppController::firefoxExtensionDir() const
{
    return QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("browser/firefox")));
}

void AppController::openFirefoxExtensionDir() const { QDesktopServices::openUrl(QUrl::fromLocalFile(firefoxExtensionDir())); }

#ifdef Q_OS_WIN
static const char* kBrowserKeys[] = {
    "HKEY_CURRENT_USER\\Software\\Google\\Chrome\\NativeMessagingHosts",
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Edge\\NativeMessagingHosts",
    "HKEY_CURRENT_USER\\Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts",
};
static const char* kFirefoxKey = "HKEY_CURRENT_USER\\Software\\Mozilla\\NativeMessagingHosts";
#endif

bool AppController::browserIntegrated() const
{
#ifdef Q_OS_WIN
    QSettings reg(QString::fromLatin1(kBrowserKeys[0]), QSettings::NativeFormat);
    const QString manifest = reg.value(QString::fromLatin1(Instance::kNativeHostName) + QStringLiteral("/Default")).toString();
    return !manifest.isEmpty() && QFile::exists(manifest);
#else
    return false;
#endif
}

QString AppController::setBrowserIntegrated(bool on)
{
#ifdef Q_OS_WIN
    const QString hostName = QString::fromLatin1(Instance::kNativeHostName);
    const QString manifestPath = QDir(Instance::profileDir()).filePath(QStringLiteral("nativehost.json"));
    if (on) {
        const QString host = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("fluxdm-nativehost.exe"));
        if (!QFile::exists(host))
            return tr("fluxdm-nativehost.exe is missing next to FluxDM.exe");
        // The extension ID is fixed by the public key in its manifest (see browser/chromium/manifest.json).
        QFile idFile(QDir(extensionDir()).filePath(QStringLiteral("extension-id.txt")));
        QString extId = idFile.open(QIODevice::ReadOnly) ? QString::fromLatin1(idFile.readAll()).trimmed() : QString();
        if (extId.isEmpty())
            return tr("Browser extension files are missing (%1)").arg(extensionDir());
        const QJsonObject manifest = BrowserHost::chromiumManifest(hostName, QDir::toNativeSeparators(host), extId);
        QFile f(manifestPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return tr("Cannot write %1").arg(manifestPath);
        f.write(QJsonDocument(manifest).toJson());
        f.close();
        for (const char* key : kBrowserKeys) {
            QSettings reg(QString::fromLatin1(key), QSettings::NativeFormat);
            reg.setValue(hostName + QStringLiteral("/Default"), QDir::toNativeSeparators(manifestPath));
        }
        // Firefox: its own manifest (allowed by add-on ID) under the Mozilla key.
        const QString geckoId = BrowserHost::geckoId(QDir(firefoxExtensionDir()).filePath(QStringLiteral("manifest.json")));
        if (!geckoId.isEmpty()) {
            const QString ffPath = QDir(Instance::profileDir()).filePath(QStringLiteral("nativehost-firefox.json"));
            QFile ff(ffPath);
            if (ff.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                ff.write(QJsonDocument(BrowserHost::firefoxManifest(hostName, QDir::toNativeSeparators(host), geckoId)).toJson());
                ff.close();
                QSettings reg(QString::fromLatin1(kFirefoxKey), QSettings::NativeFormat);
                reg.setValue(hostName + QStringLiteral("/Default"), QDir::toNativeSeparators(ffPath));
            }
        }
    } else {
        for (const char* key : kBrowserKeys) {
            QSettings reg(QString::fromLatin1(key), QSettings::NativeFormat);
            reg.remove(hostName);
        }
        QSettings ffReg(QString::fromLatin1(kFirefoxKey), QSettings::NativeFormat);
        ffReg.remove(hostName);
        QFile::remove(manifestPath);
        QFile::remove(QDir(Instance::profileDir()).filePath(QStringLiteral("nativehost-firefox.json")));
    }
    emit settingsChanged();
    return {};
#else
    Q_UNUSED(on);
    return tr("Not supported on this platform");
#endif
}
int AppController::autoRetryLimit() const { return m_mgr->autoRetryLimit(); }
void AppController::setAutoRetryLimit(int n) { m_mgr->setAutoRetryLimit(n); }
// ------------------------------------------------------------------ folders by file type (FDM: tag default folders)

QString AppController::categoryFolder(const QString& category) const
{
    return m_ui.value(QStringLiteral("categoryFolders/") + category).toString();
}

void AppController::setCategoryFolder(const QString& category, const QString& folder)
{
    const QString f = QDir::toNativeSeparators(folder.trimmed());
    if (f.isEmpty())
        m_ui.remove(QStringLiteral("categoryFolders/") + category);
    else
        m_ui.setValue(QStringLiteral("categoryFolders/") + category, f);
    emit settingsChanged();
}

QString AppController::categoryOf(const QString& links) const
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList l = BatchLinks::expandAll(links.split(ws, Qt::SkipEmptyParts), 1);
    if (l.isEmpty())
        return {};
    const QString url = l.first();
    QString name = suggestedName(url);
    if (name.isEmpty())
        name = FileNamer::fromUrl(QUrl(url));
    return FileNamer::category(name);
}

QString AppController::folderFor(const QString& links) const
{
    // A user tag with a folder for this file type wins over the folder by category.
    const QString tagFolder = m_mgr->tagFolderFor(expectedName(links));
    if (!tagFolder.isEmpty())
        return tagFolder;
    const QString folder = categoryFolder(categoryOf(links));
    return folder.isEmpty() ? defaultFolder() : folder;
}
// ------------------------------------------------------------------ import / export (FDM: .fdd)

QString AppController::exportDownloads(const QUrl& file) const
{
    QJsonArray items;
    for (Download* d : m_mgr->downloads()) {
        QJsonObject o{{QStringLiteral("url"), d->rec.url},
                      {QStringLiteral("folder"), d->destDir()},
                      {QStringLiteral("title"), d->title()},
                      {QStringLiteral("completed"), d->completed()},
                      {QStringLiteral("added"), d->rec.created.toString(Qt::ISODate)}};
        if (!d->rec.preferredName.isEmpty())
            o[QStringLiteral("fileName")] = d->rec.preferredName;
        if (!d->rec.referrer.isEmpty())
            o[QStringLiteral("referrer")] = d->rec.referrer;
        items.append(o);
    }
    const QJsonObject doc{{QStringLiteral("format"), QStringLiteral("fluxdm-downloads")},
                          {QStringLiteral("version"), 1},
                          {QStringLiteral("downloads"), items}};
    QFile f(file.toLocalFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return tr("Cannot write %1").arg(QDir::toNativeSeparators(f.fileName()));
    f.write(QJsonDocument(doc).toJson());
    return tr("Exported %n download(s).", nullptr, int(items.size()));
}

bool AppController::fdmAvailable() const { return QFile::exists(FdmImport::defaultDatabase()); }

QString AppController::importFromFdm()
{
    QString err;
    const QList<FdmImport::Item> items = FdmImport::read(FdmImport::defaultDatabase(), &err);
    if (!err.isEmpty())
        return err;
    const auto [added, skipped] = m_mgr->importFdm(items);
    if (added == 0)
        return skipped ? tr("All %1 downloads from Free Download Manager are already in the list.").arg(skipped)
                       : tr("Free Download Manager's list is empty.");
    return skipped ? tr("Imported %1 download(s) from Free Download Manager; %2 were already here.").arg(added).arg(skipped)
                   : tr("Imported %1 download(s) from Free Download Manager.").arg(added);
}

QString AppController::importDownloads(const QUrl& file)
{
    QFile f(file.toLocalFile());
    if (!f.open(QIODevice::ReadOnly))
        return tr("Cannot open %1").arg(QDir::toNativeSeparators(f.fileName()));
    const QByteArray data = f.readAll();

    struct Item { QString url, folder, fileName, referrer; };
    QList<Item> items;
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isObject() && doc.object().value(QStringLiteral("format")).toString() == QLatin1String("fluxdm-downloads")) {
        for (const auto& v : doc.object().value(QStringLiteral("downloads")).toArray()) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("completed")).toBool())
                continue; // nothing to do for finished ones
            items.append({o.value(QStringLiteral("url")).toString(), o.value(QStringLiteral("folder")).toString(),
                          o.value(QStringLiteral("fileName")).toString(), o.value(QStringLiteral("referrer")).toString()});
        }
    } else { // plain list of links, one per line
        static const QRegularExpression ws(QStringLiteral("\\s+"));
        for (const QString& l : BatchLinks::expandAll(QString::fromUtf8(data).split(ws, Qt::SkipEmptyParts)))
            items.append({l, {}, {}, {}});
    }

    int added = 0, skipped = 0;
    QSet<QString> known;
    for (Download* d : m_mgr->downloads())
        known.insert(d->rec.url);
    for (const Item& it : items) {
        if (!m_mgr->canHandle(it.url) || known.contains(it.url)) {
            ++skipped;
            continue;
        }
        const QString folder = it.folder.isEmpty() ? folderFor(it.url) : it.folder;
        if (m_mgr->add(it.url, folder, it.fileName, false, it.referrer)) { // imported paused, like FDM
            known.insert(it.url);
            ++added;
        }
    }
    if (added == 0)
        return tr("No new downloads found in the file.");
    return skipped > 0 ? tr("Imported %1 download(s) (paused); skipped %2.").arg(added).arg(skipped)
                       : tr("Imported %1 download(s) (paused).").arg(added);
}
QString AppController::linkModule(const QString& text) const
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList l = text.split(ws, Qt::SkipEmptyParts);
    return l.isEmpty() ? QString() : m_mgr->moduleFor(l.first());
}
QString AppController::moveSelected(const QUrl& folder)
{
    const QString dir = folder.toLocalFile();
    QStringList errors;
    for (qint64 id : m_sel)
        if (Download* d = byId(id)) {
            const QString err = m_mgr->moveTo(d, dir);
            if (!err.isEmpty())
                errors << d->title() + QStringLiteral(": ") + err;
        }
    return errors.isEmpty() ? QString() : errors.join(QLatin1Char('\n'));
}

bool AppController::canMove(qint64 id) const { return DownloadManager::canMove(byId(id)); }
bool AppController::convertStreams() const { return m_mgr->convertStreams(); }
void AppController::setConvertStreams(bool on) { m_mgr->setConvertStreams(on); }
QString AppController::ffmpegSetting() const { return m_mgr->ffmpegSetting(); }
void AppController::setFfmpegSetting(const QString& path) { m_mgr->setFfmpegSetting(path); }
QString AppController::ffmpegFound() const { return QDir::toNativeSeparators(m_mgr->ffmpegPath()); }
QString AppController::ytdlpSetting() const { return m_mgr->ytdlpSetting(); }
void AppController::setYtdlpSetting(const QString& path) { m_mgr->setYtdlpSetting(path); }
QString AppController::ytdlpFound() const { return QDir::toNativeSeparators(m_mgr->ytdlpPath()); }
