#include "core/Aes128.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

namespace {
QByteArray run(bool encrypt, const QByteArray& key, const QByteArray& iv, const QByteArray& data, bool* ok)
{
    if (ok)
        *ok = false;
    if (key.size() != 16 || iv.size() != 16)
        return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    QByteArray out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
        return {};
    auto cleanup = [&] {
        if (hKey)
            BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(alg, 0);
    };
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0) != 0
        || BCryptGenerateSymmetricKey(alg, &hKey, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(key.constData())),
                                      ULONG(key.size()), 0) != 0) {
        cleanup();
        return {};
    }
    QByteArray ivCopy = iv; // CNG updates the IV in place
    ULONG needed = 0;
    auto call = [&](PUCHAR dst, ULONG dstLen, ULONG* res) {
        return encrypt
            ? BCryptEncrypt(hKey, reinterpret_cast<PUCHAR>(const_cast<char*>(data.constData())), ULONG(data.size()), nullptr,
                            reinterpret_cast<PUCHAR>(ivCopy.data()), ULONG(ivCopy.size()), dst, dstLen, res, BCRYPT_BLOCK_PADDING)
            : BCryptDecrypt(hKey, reinterpret_cast<PUCHAR>(const_cast<char*>(data.constData())), ULONG(data.size()), nullptr,
                            reinterpret_cast<PUCHAR>(ivCopy.data()), ULONG(ivCopy.size()), dst, dstLen, res, BCRYPT_BLOCK_PADDING);
    };
    if (call(nullptr, 0, &needed) != 0) {
        cleanup();
        return {};
    }
    ivCopy = iv;
    out.resize(qsizetype(needed));
    ULONG written = 0;
    const NTSTATUS st = call(reinterpret_cast<PUCHAR>(out.data()), needed, &written);
    cleanup();
    if (st != 0)
        return {};
    out.resize(qsizetype(written));
    if (ok)
        *ok = true;
    return out;
}
} // namespace

QByteArray Aes128::decrypt(const QByteArray& key, const QByteArray& iv, const QByteArray& data, bool* ok)
{
    return run(false, key, iv, data, ok);
}

QByteArray Aes128::encrypt(const QByteArray& key, const QByteArray& iv, const QByteArray& data, bool* ok)
{
    return run(true, key, iv, data, ok);
}

#else
QByteArray Aes128::decrypt(const QByteArray&, const QByteArray&, const QByteArray&, bool* ok)
{
    if (ok)
        *ok = false;
    return {};
}
QByteArray Aes128::encrypt(const QByteArray&, const QByteArray&, const QByteArray&, bool* ok)
{
    if (ok)
        *ok = false;
    return {};
}
#endif
