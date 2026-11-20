#pragma once
#include <QString>
#include <functional>

// Free space checks before and during downloads. A download that runs out of space waits with
// "Not enough disk space: ..." and continues on its own once enough space is free again.
namespace DiskSpace {

constexpr qint64 kMargin = 16LL * 1024 * 1024; // keep a little free beyond what the download needs

// Bytes available to this user on the volume holding `dir` (which may not exist yet); -1 = unknown.
qint64 freeBytes(const QString& dir);

// "Not enough disk space: needs 2.1 GB, 1.0 GB free on D:\"
QString shortageText(qint64 needed, qint64 free, const QString& dir);
bool isShortage(const QString& error); // an error made by shortageText()

// Would this error be explained by the disk running full? (generic write/allocation failures)
bool looksLikeFullDisk(const QString& error);

// The drive or network share of `dir` (D:\, \\server\share) is there. A missing one (USB disk
// unplugged, share offline) makes downloads wait with "Drive not available: ..." instead of
// starting over or failing for good.
bool volumeAvailable(const QString& dir);
QString volumeRoot(const QString& dir); // "D:/", "//server/share", or "" when there is none
QString unavailableText(const QString& dir);
bool isUnavailable(const QString& error); // an error made by unavailableText()
bool isWaiting(const QString& error);     // isShortage() || isUnavailable(): continues by itself

// Tests: pretend a different amount of free space (nullptr = the real volume). Set it before
// downloads start; engines read it on their own threads.
void setFreeBytesOverride(std::function<qint64(const QString&)> fn);

} // namespace DiskSpace
