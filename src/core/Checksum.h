#pragma once
#include <QCryptographicHash>
#include <QString>
#include <atomic>

// Checksums published next to downloads (MD5, SHA-1, SHA-256, SHA-512), recognised by length.
namespace Checksum {

struct Expected {
    QCryptographicHash::Algorithm algo = QCryptographicHash::Sha256;
    QString name;  // "SHA-256"
    QString hex;   // lower case
    bool valid() const { return !hex.isEmpty(); }
};

// Accepts "ab12…", "SHA256: AB12…", "sha256:ab12…", "ab12…  file.iso" (sha256sum output).
// Invalid when no hex string of a known length is found.
Expected parse(const QString& text);

// Hex digest of a file; empty on error (error text in *error). Checks *cancel between blocks.
QString ofFile(const QString& path, QCryptographicHash::Algorithm algo, QString* error = nullptr,
               const std::atomic_bool* cancel = nullptr);

} // namespace Checksum
