#pragma once
#include <QByteArray>
#include <QString>
#include <QUrl>

namespace FileNamer {
inline const QString kPartSuffix = QStringLiteral(".fluxpart");

QString fromContentDisposition(const QByteArray& header);
QString fromUrl(const QUrl& url);
QString sanitize(QString name);
// Returns a full path in `dir` that collides neither with an existing file
// nor with an in-progress "<name><partSuffix>" file: "name (1).ext", ...
QString uniquePath(const QString& dir, const QString& name, const QString& partSuffix = kPartSuffix);
// FDM-style automatic tag: Video, Music, Archive, Docs, Programs, Pictures or Other.
QString category(const QString& fileName);
} // namespace FileNamer
