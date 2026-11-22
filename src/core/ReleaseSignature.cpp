#include "core/ReleaseSignature.h"

#include <openssl/evp.h>

namespace ReleaseSignature {

namespace {
struct KeyDeleter {
    void operator()(EVP_PKEY* k) const { EVP_PKEY_free(k); }
};
struct CtxDeleter {
    void operator()(EVP_MD_CTX* c) const { EVP_MD_CTX_free(c); }
};
using Key = std::unique_ptr<EVP_PKEY, KeyDeleter>;
using Ctx = std::unique_ptr<EVP_MD_CTX, CtxDeleter>;

QByteArray rawKey(EVP_PKEY* k, bool isPrivate)
{
    size_t len = 32;
    QByteArray out(32, Qt::Uninitialized);
    const int ok = isPrivate ? EVP_PKEY_get_raw_private_key(k, reinterpret_cast<unsigned char*>(out.data()), &len)
                             : EVP_PKEY_get_raw_public_key(k, reinterpret_cast<unsigned char*>(out.data()), &len);
    return ok == 1 && len == 32 ? out : QByteArray();
}

Key privateKey(const QByteArray& b64)
{
    const QByteArray raw = QByteArray::fromBase64(b64.trimmed());
    if (raw.size() != 32)
        return {};
    return Key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(raw.constData()), 32));
}
} // namespace

QByteArray message(const QString& version, const QByteArray& sha256Hex)
{
    return "FluxDM " + version.trimmed().toUtf8() + ' ' + sha256Hex.trimmed().toLower();
}

bool verify(const QByteArray& publicKeyB64, const QByteArray& msg, const QByteArray& signatureB64)
{
    const QByteArray pub = QByteArray::fromBase64(publicKeyB64.trimmed());
    const QByteArray sig = QByteArray::fromBase64(signatureB64.trimmed());
    if (pub.size() != 32 || sig.size() != 64)
        return false;
    Key key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(pub.constData()), 32));
    Ctx ctx(EVP_MD_CTX_new());
    if (!key || !ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1)
        return false;
    return EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char*>(sig.constData()), size_t(sig.size()),
                            reinterpret_cast<const unsigned char*>(msg.constData()), size_t(msg.size())) == 1;
}

bool generateKey(QByteArray* privateKeyB64, QByteArray* publicKeyB64)
{
    EVP_PKEY* raw = nullptr;
    std::unique_ptr<EVP_PKEY_CTX, void (*)(EVP_PKEY_CTX*)> pctx(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), EVP_PKEY_CTX_free);
    if (!pctx || EVP_PKEY_keygen_init(pctx.get()) != 1 || EVP_PKEY_keygen(pctx.get(), &raw) != 1)
        return false;
    Key key(raw);
    const QByteArray priv = rawKey(key.get(), true), pub = rawKey(key.get(), false);
    if (priv.isEmpty() || pub.isEmpty())
        return false;
    *privateKeyB64 = priv.toBase64();
    *publicKeyB64 = pub.toBase64();
    return true;
}

QByteArray publicKeyOf(const QByteArray& privateKeyB64)
{
    Key key = privateKey(privateKeyB64);
    return key ? rawKey(key.get(), false).toBase64() : QByteArray();
}

QByteArray sign(const QByteArray& privateKeyB64, const QByteArray& msg)
{
    Key key = privateKey(privateKeyB64);
    Ctx ctx(EVP_MD_CTX_new());
    if (!key || !ctx || EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1)
        return {};
    QByteArray sig(64, Qt::Uninitialized);
    size_t len = 64;
    if (EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char*>(sig.data()), &len,
                       reinterpret_cast<const unsigned char*>(msg.constData()), size_t(msg.size())) != 1 || len != 64)
        return {};
    return sig.toBase64();
}

} // namespace ReleaseSignature
