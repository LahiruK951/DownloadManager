#pragma once
#include <QByteArray>
#include <QString>

// Ed25519 signatures on release manifests: the update manifest (latest.json) carries a signature
// of "FluxDM <version> <sha256>" made with the release key, and the app checks it with the public
// key built into it (CMake FLUXDM_UPDATE_PUBLIC_KEY). Someone who can change the manifest on the
// server still can't make FluxDM run their installer. Keys and signatures are base64.
namespace ReleaseSignature {

QByteArray message(const QString& version, const QByteArray& sha256Hex);
bool verify(const QByteArray& publicKeyB64, const QByteArray& message, const QByteArray& signatureB64);

// For scripts/fluxdm-sign: a new key pair (32-byte raw keys) and signing.
bool generateKey(QByteArray* privateKeyB64, QByteArray* publicKeyB64);
QByteArray sign(const QByteArray& privateKeyB64, const QByteArray& message); // "" on error
QByteArray publicKeyOf(const QByteArray& privateKeyB64);

} // namespace ReleaseSignature
