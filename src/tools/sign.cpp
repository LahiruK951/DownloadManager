// fluxdm-sign: release signing for the updater.
//   fluxdm-sign keygen <private-key-file>          new key pair; prints the public key for
//                                                  CMake -DFLUXDM_UPDATE_PUBLIC_KEY=<it>
//   fluxdm-sign sign <private-key-file> <version> <sha256>   prints the manifest signature
// Keep the private key file off the build machine's shared folders and out of the repository.
#include <QCoreApplication>
#include <QFile>
#include <cstdio>

#include "core/ReleaseSignature.h"

static int usage()
{
    std::fputs("usage: fluxdm-sign keygen <private-key-file>\n"
               "       fluxdm-sign sign <private-key-file> <version> <sha256>\n", stderr);
    return 2;
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList a = app.arguments().mid(1);
    if (a.size() == 2 && a[0] == QLatin1String("keygen")) {
        if (QFile::exists(a[1])) {
            std::fputs("refusing to overwrite an existing key file\n", stderr);
            return 1;
        }
        QByteArray priv, pub;
        QFile f(a[1]);
        if (!ReleaseSignature::generateKey(&priv, &pub) || !f.open(QIODevice::WriteOnly) || f.write(priv + '\n') < 0) {
            std::fputs("cannot create the key\n", stderr);
            return 1;
        }
        std::printf("%s\n", pub.constData());
        return 0;
    }
    if (a.size() == 4 && a[0] == QLatin1String("sign")) {
        QFile f(a[1]);
        if (!f.open(QIODevice::ReadOnly)) {
            std::fputs("cannot read the key file\n", stderr);
            return 1;
        }
        const QByteArray sig = ReleaseSignature::sign(f.readAll().trimmed(), ReleaseSignature::message(a[2], a[3].toLatin1()));
        if (sig.isEmpty()) {
            std::fputs("signing failed (not an Ed25519 key file?)\n", stderr);
            return 1;
        }
        std::printf("%s\n", sig.constData());
        return 0;
    }
    return usage();
}
