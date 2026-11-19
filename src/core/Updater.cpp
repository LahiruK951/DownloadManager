#include "core/Updater.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QStandardPaths>

Updater::Updater(const QString& currentVersion, QObject* parent)
    : QObject(parent), m_current(currentVersion)
{
}

int Updater::compareVersions(const QString& a, const QString& b)
{
    const QStringList x = a.trimmed().split(QLatin1Char('.')), y = b.trimmed().split(QLatin1Char('.'));
    for (int i = 0; i < qMax(x.size(), y.size()); ++i) {
        const int p = x.value(i, QStringLiteral("0")).toInt(), q = y.value(i, QStringLiteral("0")).toInt();
        if (p != q)
            return p < q ? -1 : 1;
    }
    return 0;
}

bool Updater::allowedUrl(const QUrl& url)
{
    if (url.scheme() == QLatin1String("https"))
        return true;
    const QString h = url.host();
    return url.scheme() == QLatin1String("http") && (h == QLatin1String("localhost") || h == QLatin1String("127.0.0.1"));
}

void Updater::check(const QUrl& manifestUrl)
{
    if (!allowedUrl(manifestUrl)) {
        emit failed(tr("Updates are only checked over HTTPS"));
        return;
    }
    if (m_reply)
        m_reply->abort();
    QNetworkRequest req(manifestUrl);
    req.setTransferTimeout(30000);
    QNetworkReply* r = m_nam.get(req);
    m_reply = r;
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            emit failed(tr("Couldn't check for updates: %1").arg(r->errorString()));
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
        const QString version = o.value(QStringLiteral("version")).toString();
        const QUrl url = r->url().resolved(QUrl(o.value(QStringLiteral("url")).toString()));
        const QByteArray sha = o.value(QStringLiteral("sha256")).toString().toLatin1().toLower();
        if (version.isEmpty() || !url.isValid() || sha.size() != 64) {
            emit failed(tr("The update information is incomplete"));
            return;
        }
        m_version = version;
        m_notes = o.value(QStringLiteral("notes")).toString();
        m_installer = url;
        m_sha = sha;
        emit checked(compareVersions(version, m_current) > 0, version, m_notes);
    });
}

void Updater::download()
{
    if (m_installer.isEmpty() || !allowedUrl(m_installer)) {
        emit failed(tr("No update to download"));
        return;
    }
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("FluxDM-update"));
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QStringLiteral("FluxDM-%1-setup.exe").arg(m_version));
    QNetworkRequest req(m_installer);
    req.setTransferTimeout(60000);
    QNetworkReply* r = m_nam.get(req);
    m_reply = r;
    connect(r, &QNetworkReply::downloadProgress, this, &Updater::progress);
    connect(r, &QNetworkReply::finished, this, [this, r, path] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            emit failed(tr("Couldn't download the update: %1").arg(r->errorString()));
            return;
        }
        const QByteArray data = r->readAll();
        // Never run something that isn't exactly the published installer.
        if (QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() != m_sha) {
            emit failed(tr("The downloaded update is damaged or not genuine (checksum mismatch); it was discarded"));
            return;
        }
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) {
            emit failed(tr("Cannot save the update to %1").arg(QDir::toNativeSeparators(path)));
            return;
        }
        f.close();
        emit ready(path);
    });
}

bool Updater::install(const QString& installerPath)
{
    // /SILENT shows progress only; the installer closes FluxDM and starts it again when done.
    return QProcess::startDetached(installerPath, {QStringLiteral("/SILENT"), QStringLiteral("/SUPPRESSMSGBOXES"),
                                                   QStringLiteral("/NORESTART"), QStringLiteral("/CLOSEAPPLICATIONS")});
}
