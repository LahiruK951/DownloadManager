#include "core/DiskSpace.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QStorageInfo>

namespace {
std::function<qint64(const QString&)>& override()
{
    static std::function<qint64(const QString&)> fn;
    return fn;
}

QString existingAncestor(const QString& dir)
{
    QString d = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
    while (!d.isEmpty() && !QFileInfo::exists(d)) {
        const QString up = QFileInfo(d).absolutePath();
        if (up == d)
            break;
        d = up;
    }
    return d;
}

QString sizeText(qint64 bytes)
{
    return QLocale::c().formattedDataSize(qMax<qint64>(0, bytes), 1, QLocale::DataSizeTraditionalFormat);
}
} // namespace

namespace DiskSpace {

qint64 freeBytes(const QString& dir)
{
    if (override())
        return override()(dir);
    const QStorageInfo si(existingAncestor(dir)); // the folder may not exist yet
    return si.isValid() && si.isReady() ? si.bytesAvailable() : -1;
}

QString shortageText(qint64 needed, qint64 free, const QString& dir)
{
    const QStorageInfo si(existingAncestor(dir));
    const QString where = si.isValid() ? QDir::toNativeSeparators(si.rootPath()) : QDir::toNativeSeparators(dir);
    return QStringLiteral("Not enough disk space: needs %1, %2 free on %3").arg(sizeText(needed), sizeText(free), where);
}

bool isShortage(const QString& error) { return error.startsWith(QLatin1String("Not enough disk space")); }

bool looksLikeFullDisk(const QString& error)
{
    return isShortage(error) || error.contains(QLatin1String("Disk write"), Qt::CaseInsensitive)
        || error.contains(QLatin1String("Cannot allocate"), Qt::CaseInsensitive)
        || error.contains(QLatin1String("not enough space"), Qt::CaseInsensitive)
        || error.contains(QLatin1String("disk is full"), Qt::CaseInsensitive);
}

void setFreeBytesOverride(std::function<qint64(const QString&)> fn) { override() = std::move(fn); }

} // namespace DiskSpace
