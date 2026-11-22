// DownloadManager: logins, tags, network settings, FDM import, trusted certificates.
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
