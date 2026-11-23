#include "core/ExternalTools.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>

#include "core/Updater.h"

ExternalTools::ExternalTools(const QString& profileToolsDir, const QString& bundledToolsDir, QObject* parent)
    : QObject(parent), m_profileDir(profileToolsDir), m_bundledDir(bundledToolsDir)
{
    m_api[YtDlp] = QUrl(QStringLiteral("https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest"));
    m_api[Ffmpeg] = QUrl(QStringLiteral("https://api.github.com/repos/GyanD/codexffmpeg/releases/latest"));
}

QString ExternalTools::key(Tool t) { return t == YtDlp ? QStringLiteral("yt-dlp") : QStringLiteral("ffmpeg"); }
QString ExternalTools::exeName(Tool t) { return key(t) + QStringLiteral(".exe"); }

int ExternalTools::compareVersions(const QString& a, const QString& b)
{
    // Only the leading "1.2.3" part counts: "7.1.1-essentials_build" → 7.1.1.
    static const QRegularExpression lead(QStringLiteral(R"(^\s*v?(\d+(?:\.\d+)*))"));
    const auto num = [](const QString& s) { const auto m = lead.match(s); return m.hasMatch() ? m.captured(1) : QString(); };
    return Updater::compareVersions(num(a), num(b));
}

QJsonObject ExternalTools::readVersions(const QString& dir)
{
    QFile f(QDir(dir).filePath(QStringLiteral("versions.json")));
    if (dir.isEmpty() || !f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.read(64 * 1024)).object();
}

ExternalTools::Copy ExternalTools::managed(Tool t) const
{
    Copy best;
    for (const QString& dir : {m_profileDir, m_bundledDir}) {
        if (dir.isEmpty())
            continue;
        const QString exe = QDir(dir).filePath(exeName(t));
        if (!QFileInfo(exe).isFile())
            continue;
        const QString v = readVersions(dir).value(key(t)).toString();
        // The profile copy is checked first: it wins unless the bundled one is newer (an app update).
        if (best.path.isEmpty() || compareVersions(v, best.version) > 0)
            best = {QDir::toNativeSeparators(exe), v, dir == m_bundledDir};
    }
    return best;
}

bool ExternalTools::updateAvailable(Tool t) const
{
    if (m_rel[t].version.isEmpty())
        return false;
    const Copy c = managed(t);
    return c.path.isEmpty() || compareVersions(m_rel[t].version, c.version) > 0;
}

void ExternalTools::setStatus(Tool t, const QString& s)
{
    m_status[t] = s;
    emit changed();
}

// ------------------------------------------------------------------ release lookup

void ExternalTools::checkLatest()
{
    if (m_pendingChecks > 0)
        return;
    m_pendingChecks = kCount;
    for (int i = 0; i < kCount; ++i)
        fetchRelease(Tool(i), false);
}

void ExternalTools::fetchRelease(Tool t, bool thenInstall)
{
    const auto done = [this, thenInstall](Tool tool, const QString& error) {
        if (!thenInstall) {
            if (--m_pendingChecks == 0)
                emit checked();
            emit changed();
            return;
        }
        if (!error.isEmpty())
            finish(tool, false, error);
        else if (!updateAvailable(tool))
            finish(tool, true, tr("%1 is up to date").arg(key(tool)));
        else
            download(tool);
    };
    if (!Updater::allowedUrl(m_api[t])) {
        done(t, tr("Updates are only downloaded over HTTPS"));
        return;
    }
    QNetworkRequest req(m_api[t]);
    req.setTransferTimeout(30000);
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("FluxDM"));
    QNetworkReply* r = m_nam.get(req);
    connect(r, &QNetworkReply::finished, this, [this, r, t, done] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            done(t, tr("Couldn't look for a new %1: %2").arg(key(t), r->errorString()));
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(r->read(4 * 1024 * 1024)).object();
        Release rel;
        rel.version = o.value(QStringLiteral("tag_name")).toString().trimmed();
        // yt-dlp: the Windows program. ffmpeg: the "essentials" build, 7z (a third of the zip) when
        // the extractor can read it — Windows' tar and 7-Zip both can.
        static const QRegularExpression ffmpegAsset(QStringLiteral(R"(^ffmpeg-[\w.\-]*essentials_build\.(7z|zip)$)"));
        int bestRank = -1;
        for (const QJsonValue& v : o.value(QStringLiteral("assets")).toArray()) {
            const QJsonObject a = v.toObject();
            const QString name = a.value(QStringLiteral("name")).toString();
            int rank = -1;
            if (t == YtDlp && name == QLatin1String("yt-dlp.exe"))
                rank = 1;
            else if (t == Ffmpeg) {
                const auto m = ffmpegAsset.match(name);
                if (m.hasMatch())
                    rank = m.captured(1) == QLatin1String("7z") && !m_extractor.isEmpty() ? 2 : 1;
            }
            if (rank <= bestRank)
                continue;
            const QString digest = a.value(QStringLiteral("digest")).toString();
            if (!digest.startsWith(QLatin1String("sha256:")))
                continue; // no checksum, no install
            bestRank = rank;
            rel.assetName = name;
            rel.url = r->url().resolved(QUrl(a.value(QStringLiteral("browser_download_url")).toString()));
            rel.sha256 = digest.mid(7).toLatin1().toLower();
        }
        if (rel.version.isEmpty() || rel.url.isEmpty() || rel.sha256.size() != 64) {
            done(t, tr("No %1 download for Windows found in its latest release").arg(key(t)));
            return;
        }
        m_rel[t] = rel;
        done(t, {});
    });
}

// ------------------------------------------------------------------ download + install

void ExternalTools::update(Tool t)
{
    if (m_busy[t])
        return;
    m_busy[t] = true;
    setStatus(t, tr("Looking for the latest version…"));
    fetchRelease(t, true);
}

void ExternalTools::download(Tool t)
{
    const Release rel = m_rel[t];
    if (!Updater::allowedUrl(rel.url)) {
        finish(t, false, tr("Updates are only downloaded over HTTPS"));
        return;
    }
    if (!QDir().mkpath(m_profileDir)) {
        finish(t, false, tr("Cannot create %1").arg(QDir::toNativeSeparators(m_profileDir)));
        return;
    }
    const QString part = QDir(m_profileDir).filePath(QStringLiteral(".download-") + rel.assetName);
    auto* file = new QFile(part, this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete file;
        finish(t, false, tr("Cannot write %1").arg(QDir::toNativeSeparators(part)));
        return;
    }
    setStatus(t, tr("Downloading %1 %2…").arg(key(t), rel.version));
    QNetworkRequest req(rel.url);
    req.setTransferTimeout(60000);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("FluxDM"));
    QNetworkReply* r = m_nam.get(req);
    connect(r, &QNetworkReply::readyRead, this, [r, file] { file->write(r->readAll()); });
    connect(r, &QNetworkReply::downloadProgress, this, [this, t, rel](qint64 got, qint64 total) {
        if (total > 0)
            setStatus(t, tr("Downloading %1 %2… %3%").arg(key(t), rel.version).arg(got * 100 / total));
    });
    connect(r, &QNetworkReply::finished, this, [this, r, file, t, rel, part] {
        r->deleteLater();
        file->write(r->readAll());
        const bool writeOk = file->error() == QFileDevice::NoError;
        file->close();
        file->deleteLater();
        if (r->error() != QNetworkReply::NoError || !writeOk) {
            QFile::remove(part);
            finish(t, false, tr("Couldn't download %1: %2").arg(key(t), writeOk ? r->errorString() : file->errorString()));
            return;
        }
        QFile f(part);
        QCryptographicHash h(QCryptographicHash::Sha256);
        if (!f.open(QIODevice::ReadOnly) || !h.addData(&f) || h.result().toHex() != rel.sha256) {
            f.close();
            QFile::remove(part);
            finish(t, false, tr("The downloaded %1 didn't match its checksum; nothing was changed").arg(key(t)));
            return;
        }
        f.close();
        install(t, part);
    });
}

// Puts `src` in place of <profile>/tools/<exe>. Fails (and keeps the old copy) while it is running.
static QString replaceExe(const QString& src, const QString& dst)
{
    const QString old = dst + QStringLiteral(".old");
    QFile::remove(old);
    if (QFile::exists(dst) && !QFile::rename(dst, old))
        return QStringLiteral("in use");
    if (!QFile::rename(src, dst)) {
        QFile::rename(old, dst);
        return QStringLiteral("cannot move");
    }
    QFile::remove(old);
    return {};
}

void ExternalTools::install(Tool t, const QString& file)
{
    const QString target = QDir(m_profileDir).filePath(exeName(t));
    const QString version = m_rel[t].version;
    const auto place = [this, t, target, version](const QString& exe) {
        const QString err = replaceExe(exe, target);
        if (!err.isEmpty()) {
            QFile::remove(exe);
            finish(t, false, err == QLatin1String("in use")
                                 ? tr("%1 is in use; try again when your downloads have finished").arg(key(t))
                                 : tr("Couldn't install %1").arg(key(t)));
            return;
        }
        writeVersion(t, version);
        finish(t, true, tr("%1 %2 is installed").arg(key(t), version));
    };
    if (t == YtDlp) {
        place(file);
        return;
    }

    // ffmpeg comes as an archive: take bin/ffmpeg.exe (and its licence) out of it.
    if (m_extractor.isEmpty()) {
        QFile::remove(file);
        finish(t, false, tr("Can't unpack ffmpeg: no extractor (Windows' tar or 7-Zip) found"));
        return;
    }
    const QString tmp = QDir(m_profileDir).filePath(QStringLiteral(".unpack-ffmpeg"));
    QDir(tmp).removeRecursively();
    QDir().mkpath(tmp);
    setStatus(t, tr("Unpacking ffmpeg %1…").arg(version));
    auto* p = new QProcess(this);
    const bool sevenZip = QFileInfo(m_extractor).baseName().compare(QLatin1String("7z"), Qt::CaseInsensitive) == 0;
    const QStringList args = sevenZip
        ? QStringList{QStringLiteral("e"), QStringLiteral("-y"), QStringLiteral("-o") + QDir::toNativeSeparators(tmp),
                      QDir::toNativeSeparators(file), QStringLiteral("ffmpeg.exe"), QStringLiteral("LICENSE"), QStringLiteral("-r")}
        : QStringList{QStringLiteral("-xf"), QDir::toNativeSeparators(file), QStringLiteral("-C"), QDir::toNativeSeparators(tmp),
                      QStringLiteral("*/bin/ffmpeg.exe"), QStringLiteral("*/LICENSE")};
    connect(p, &QProcess::finished, this, [this, p, t, tmp, file, place](int code, QProcess::ExitStatus st) {
        p->deleteLater();
        QFile::remove(file);
        QString exe, license;
        for (QDirIterator it(tmp, QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
            const QString f = it.next();
            const QString name = QFileInfo(f).fileName();
            if (name.compare(QLatin1String("ffmpeg.exe"), Qt::CaseInsensitive) == 0 && exe.isEmpty())
                exe = f;
            else if (name == QLatin1String("LICENSE") && license.isEmpty())
                license = f;
        }
        if (st != QProcess::NormalExit || code != 0 || exe.isEmpty()) {
            QDir(tmp).removeRecursively();
            finish(t, false, tr("Couldn't unpack ffmpeg"));
            return;
        }
        const QString staged = QDir(m_profileDir).filePath(QStringLiteral(".ffmpeg-new.exe"));
        QFile::remove(staged);
        if (!QFile::rename(exe, staged)) {
            QDir(tmp).removeRecursively();
            finish(t, false, tr("Couldn't install %1").arg(key(t)));
            return;
        }
        if (!license.isEmpty()) {
            const QString dst = QDir(m_profileDir).filePath(QStringLiteral("ffmpeg-LICENSE.txt"));
            QFile::remove(dst);
            QFile::rename(license, dst);
        }
        QDir(tmp).removeRecursively();
        place(staged);
    });
    p->start(m_extractor, args);
}

bool ExternalTools::writeVersion(Tool t, const QString& version)
{
    QJsonObject o = readVersions(m_profileDir);
    o.insert(key(t), version);
    QSaveFile f(QDir(m_profileDir).filePath(QStringLiteral("versions.json")));
    return f.open(QIODevice::WriteOnly) && f.write(QJsonDocument(o).toJson()) > 0 && f.commit();
}

void ExternalTools::finish(Tool t, bool ok, const QString& message)
{
    m_busy[t] = false;
    m_status[t] = ok ? QString() : message;
    emit changed();
    emit updated(t, ok, message);
}
