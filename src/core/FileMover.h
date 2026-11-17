#pragma once
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QString>

// Moves a file or a whole folder. Same-volume moves are a rename; across
// volumes the data is copied and the source removed afterwards.
namespace FileMover {

inline bool copyTree(const QString& src, const QString& dst, QString* error)
{
    const QFileInfo fi(src);
    if (fi.isFile()) {
        QDir().mkpath(QFileInfo(dst).absolutePath());
        if (!QFile::copy(src, dst)) {
            if (error)
                *error = QStringLiteral("Cannot copy %1").arg(QDir::toNativeSeparators(src));
            return false;
        }
        return true;
    }
    QDir().mkpath(dst);
    QDirIterator it(src, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString rel = QDir(src).relativeFilePath(path);
        const QString target = QDir(dst).filePath(rel);
        if (it.fileInfo().isDir()) {
            QDir().mkpath(target);
        } else if (!QFile::copy(path, target)) {
            if (error)
                *error = QStringLiteral("Cannot copy %1").arg(QDir::toNativeSeparators(path));
            return false;
        }
    }
    return true;
}

// Returns true on success; `dst` must not exist.
inline bool move(const QString& src, const QString& dst, QString* error = nullptr)
{
    if (!QFileInfo::exists(src)) {
        if (error)
            *error = QStringLiteral("%1 does not exist").arg(QDir::toNativeSeparators(src));
        return false;
    }
    QDir().mkpath(QFileInfo(dst).absolutePath());
    if (QDir().rename(src, dst)) // same volume: instant
        return true;
    if (!copyTree(src, dst, error)) {
        QFileInfo(dst).isDir() ? QDir(dst).removeRecursively() : QFile::remove(dst);
        return false;
    }
    if (QFileInfo(src).isDir())
        QDir(src).removeRecursively();
    else
        QFile::remove(src);
    return true;
}

} // namespace FileMover
