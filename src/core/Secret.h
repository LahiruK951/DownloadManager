#pragma once
#include <QByteArray>
#include <QString>

// Protects small secrets (saved server passwords) before they are written to disk.
// Windows: DPAPI, so only the same Windows user on the same machine can read them back.
// Elsewhere: stored as-is (marked "plain:") until a keychain backend exists.
namespace Secret {
QByteArray protect(const QString& secret);
QString unprotect(const QByteArray& blob); // "" if it cannot be decrypted
} // namespace Secret
