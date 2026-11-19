#include "core/TorrentFolderWatcher.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "core/FileNamer.h"

TorrentFolderWatcher::TorrentFolderWatcher(const QString& keepDir, QObject* parent)
    : QObject(parent), m_keepDir(keepDir)
{
    connect(&m_fsw, &QFileSystemWatcher::directoryChanged, this, [this] { QTimer::singleShot(300, this, &TorrentFolderWatcher::scan); });
    m_poll.setInterval(3000);
    connect(&m_poll, &QTimer::timeout, this, &TorrentFolderWatcher::scan);
}

void TorrentFolderWatcher::setFolder(const QString& folder)
{
    if (!m_fsw.directories().isEmpty())
        m_fsw.removePaths(m_fsw.directories());
    m_sizes.clear();
    m_folder = folder.trimmed();
    if (m_folder.isEmpty() || !QFileInfo(m_folder).isDir()) {
        m_poll.stop();
        return;
    }
    m_fsw.addPath(m_folder);
    m_poll.start();
    scan();
}

void TorrentFolderWatcher::scanNow() { scan(); }

void TorrentFolderWatcher::scan()
{
    if (m_folder.isEmpty())
        return;
    const QFileInfoList files = QDir(m_folder).entryInfoList({QStringLiteral("*.torrent")}, QDir::Files);
    QHash<QString, qint64> seen;
    for (const QFileInfo& fi : files) {
        const QString path = fi.absoluteFilePath();
        seen.insert(path, fi.size());
        // Take it only once the size stayed the same between two looks (a browser may still be writing).
        if (m_sizes.value(path, -1) != fi.size())
            continue;
        if (!FileNamer::looksLikeTorrentFile(path)) {
            continue; // not a torrent (or not finished): leave it alone
        }
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray data = f.readAll();
        f.close();
        QDir().mkpath(m_keepDir);
        // Named after the content, so the same torrent dropped twice maps to one copy.
        const QString copy = QDir(m_keepDir).filePath(
            QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex().left(16)) + QStringLiteral(".torrent"));
        QFile out(copy);
        if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size())
            continue;
        out.close();
        QString added = path + QStringLiteral(".added");
        QFile::remove(added); // an older one with the same name
        if (!QFile::rename(path, added)) {
            QFile::remove(copy); // couldn't mark it: better to try again later than to add it twice
            continue;
        }
        seen.remove(path);
        emit torrentAdded(copy, fi.fileName());
    }
    m_sizes = seen;
}
