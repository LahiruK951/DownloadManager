#include "core/Checksum.h"

#include <QFile>
#include <QRegularExpression>

namespace Checksum {

Expected parse(const QString& text)
{
    static const QRegularExpression hexRun(QStringLiteral("\\b([0-9a-fA-F]{32,128})\\b"));
    Expected e;
    const QRegularExpressionMatch m = hexRun.match(text);
    if (!m.hasMatch())
        return e;
    const QString hex = m.captured(1).toLower();
    switch (hex.size()) {
    case 32: e.algo = QCryptographicHash::Md5; e.name = QStringLiteral("MD5"); break;
    case 40: e.algo = QCryptographicHash::Sha1; e.name = QStringLiteral("SHA-1"); break;
    case 64: e.algo = QCryptographicHash::Sha256; e.name = QStringLiteral("SHA-256"); break;
    case 128: e.algo = QCryptographicHash::Sha512; e.name = QStringLiteral("SHA-512"); break;
    default: return e;
    }
    e.hex = hex;
    return e;
}

QString ofFile(const QString& path, QCryptographicHash::Algorithm algo, QString* error, const std::atomic_bool* cancel)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return {};
    }
    QCryptographicHash h(algo);
    QByteArray buf(1 << 20, Qt::Uninitialized);
    for (;;) {
        if (cancel && cancel->load()) {
            if (error)
                *error = QStringLiteral("Cancelled");
            return {};
        }
        const qint64 n = f.read(buf.data(), buf.size());
        if (n < 0) {
            if (error)
                *error = f.errorString();
            return {};
        }
        if (n == 0)
            break;
        h.addData(QByteArrayView(buf.constData(), n));
    }
    return QString::fromLatin1(h.result().toHex());
}

} // namespace Checksum
