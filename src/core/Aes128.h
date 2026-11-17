#pragma once
#include <QByteArray>

// AES-128-CBC with PKCS#7 padding (HLS "METHOD=AES-128"), via Windows CNG.
namespace Aes128 {
QByteArray decrypt(const QByteArray& key, const QByteArray& iv, const QByteArray& data, bool* ok = nullptr);
QByteArray encrypt(const QByteArray& key, const QByteArray& iv, const QByteArray& data, bool* ok = nullptr);
} // namespace Aes128
