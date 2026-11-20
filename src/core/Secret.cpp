#include "core/Secret.h"

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>
#endif

namespace Secret {

static const QByteArray kDpapi = QByteArrayLiteral("dpapi:");
static const QByteArray kPlain = QByteArrayLiteral("plain:");

QByteArray protect(const QString& secret) { return protectBytes(secret.toUtf8()); }
QString unprotect(const QByteArray& blob) { return QString::fromUtf8(unprotectBytes(blob)); }
bool isProtected(const QByteArray& blob) { return blob.startsWith(kDpapi) || blob.startsWith(kPlain); }

QByteArray protectBytes(const QByteArray& raw)
{
#ifdef Q_OS_WIN
    DATA_BLOB in{DWORD(raw.size()), reinterpret_cast<BYTE*>(const_cast<char*>(raw.constData()))};
    DATA_BLOB out{};
    if (CryptProtectData(&in, L"FluxDM", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        const QByteArray blob = kDpapi + QByteArray(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
        LocalFree(out.pbData);
        return blob;
    }
#endif
    return kPlain + raw;
}

QByteArray unprotectBytes(const QByteArray& blob)
{
    if (blob.startsWith(kPlain))
        return blob.mid(kPlain.size());
#ifdef Q_OS_WIN
    if (blob.startsWith(kDpapi)) {
        const QByteArray enc = blob.mid(kDpapi.size());
        DATA_BLOB in{DWORD(enc.size()), reinterpret_cast<BYTE*>(const_cast<char*>(enc.constData()))};
        DATA_BLOB out{};
        if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            const QByteArray s(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
            SecureZeroMemory(out.pbData, out.cbData);
            LocalFree(out.pbData);
            return s;
        }
    }
#endif
    return {};
}

} // namespace Secret
