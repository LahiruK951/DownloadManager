#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// Metalink files (RFC 5854 .meta4, and the older 3.0 .metalink): one or more files, each with
// several download addresses (mirrors) and checksums. FluxDM turns each entry into a download
// with its mirrors and the strongest checksum to verify.
namespace Metalink {

struct File {
    QString name;        // file name only (any folders in it are dropped)
    qint64 size = -1;
    QStringList urls;    // http/https/ftp, best first (priority / preference)
    QString hash;        // "sha-256:<hex>" style text for Checksum::parse; "" = none
};

bool looksLike(const QString& path);            // by extension, or XML naming the metalink namespace
QList<File> parse(const QByteArray& xml, QString* error = nullptr);

} // namespace Metalink
