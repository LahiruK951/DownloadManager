// Writes resources/fluxdm.ico (the exe, shortcut and uninstaller icon) from the same drawing the
// app uses at runtime (src/ui/AppIcon.h). Not shipped; run after changing the icon:
//   set QT_QPA_PLATFORM=offscreen && build\release\fluxdm-makeicon.exe resources\fluxdm.ico
#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QtEndian>
#include <cstdio>

#include "ui/AppIcon.h"

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: fluxdm-makeicon <out.ico>\n");
        return 2;
    }
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    QList<QByteArray> pngs;
    for (int s : sizes) {
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        AppIcon::render(s).toImage().convertToFormat(QImage::Format_ARGB32).save(&buf, "PNG");
        pngs << png;
    }
    // ICO with PNG-compressed images (Windows Vista and later): ICONDIR, one ICONDIRENTRY each, data.
    QByteArray ico;
    auto u16 = [&ico](quint16 v) { const quint16 le = qToLittleEndian(v); ico.append(reinterpret_cast<const char*>(&le), 2); };
    auto u32 = [&ico](quint32 v) { const quint32 le = qToLittleEndian(v); ico.append(reinterpret_cast<const char*>(&le), 4); };
    const int n = int(std::size(sizes));
    u16(0);
    u16(1);
    u16(quint16(n));
    quint32 offset = 6 + 16 * n;
    for (int i = 0; i < n; ++i) {
        ico.append(char(sizes[i] >= 256 ? 0 : sizes[i])); // width (0 = 256)
        ico.append(char(sizes[i] >= 256 ? 0 : sizes[i])); // height
        ico.append(char(0));                              // palette colours
        ico.append(char(0));                              // reserved
        u16(1);                                           // planes
        u16(32);                                          // bits per pixel
        u32(quint32(pngs[i].size()));
        u32(offset);
        offset += quint32(pngs[i].size());
    }
    for (const QByteArray& p : pngs)
        ico += p;
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::WriteOnly) || f.write(ico) != ico.size()) {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    return 0;
}
