#pragma once
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVariantList>

namespace FileNamer {
inline const QString kPartSuffix = QStringLiteral(".fluxpart");

QString fromContentDisposition(const QByteArray& header);
QString fromUrl(const QUrl& url);
QString sanitize(QString name);
// Returns a full path in `dir` that collides neither with an existing file
// nor with an in-progress "<name><partSuffix>" file: "name (1).ext", ...
QString uniquePath(const QString& dir, const QString& name, const QString& partSuffix = kPartSuffix);
// What to do when the target file already exists (FDM: existingFileReaction).
enum ExistingFile { KeepBoth = 0, Overwrite = 1, Ask = 2 };
// Where to save `name` in `dir` under `reaction`. Never collides with another download's
// in-progress part file. With Ask and an existing file, sets *conflict and returns the path.
QString resolveTarget(const QString& dir, const QString& name, int reaction, bool* conflict,
                      const QString& partSuffix = kPartSuffix);
// Final name when a finished part file is renamed into place: `target` itself (removing an
// existing file first when overwriting), or a numbered name if it is taken/locked.
QString completionTarget(const QString& target, int reaction);
// True for a bencoded .torrent file (checked by content, not by name).
bool looksLikeTorrentFile(const QString& path);
// FDM-style automatic tag: Video, Music, Archive, Docs, Programs, Pictures or Other.
QString category(const QString& fileName);
// Category of a multi-file download ([{name, size}], e.g. a torrent): the one holding the most
// bytes, so a movie with subtitles and a poster is Video. "" when the list is empty.
QString categoryOfFiles(const QVariantList& files);
} // namespace FileNamer
