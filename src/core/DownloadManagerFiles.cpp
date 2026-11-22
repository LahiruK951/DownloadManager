// DownloadManager: moving files, stream conversion and the MP3/MP4 converter.
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
