// fluxdm-nativehost: Chrome/Edge native messaging host (FDM's wenativehost.exe equivalent).
// The browser extension talks to it over stdio (4-byte little-endian length + JSON);
// requests are forwarded to the running FluxDM over its single-instance channel,
// starting FluxDM (in the tray) when it isn't running.
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QThread>
#include <cstdio>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "core/Instance.h"

namespace {
constexpr quint32 kMaxMessage = 1024 * 1024;

bool readMessage(QByteArray& out)
{
    quint32 len = 0;
    if (std::fread(&len, sizeof(len), 1, stdin) != 1)
        return false;
    if (len == 0 || len > kMaxMessage)
        return false;
    out.resize(qsizetype(len));
    return std::fread(out.data(), 1, len, stdin) == len;
}

void writeMessage(const QJsonObject& o)
{
    const QByteArray d = QJsonDocument(o).toJson(QJsonDocument::Compact);
    const quint32 len = quint32(d.size());
    std::fwrite(&len, sizeof(len), 1, stdout);
    std::fwrite(d.constData(), 1, len, stdout);
    std::fflush(stdout);
}

bool appRunning()
{
    QLocalSocket s;
    s.connectToServer(Instance::channelName());
    const bool ok = s.waitForConnected(300);
    s.abort();
    return ok;
}

QJsonObject deliver(const QJsonObject& req)
{
    QLocalSocket s;
    s.connectToServer(Instance::channelName());
    if (!s.waitForConnected(1000))
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("FluxDM is not running")}};
    s.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
    s.waitForBytesWritten(2000);
    QElapsedTimer t;
    t.start();
    while (!s.canReadLine() && t.elapsed() < 5000) {
        if (!s.waitForReadyRead(250) && s.state() != QLocalSocket::ConnectedState)
            break;
    }
    const QJsonDocument reply = QJsonDocument::fromJson(s.readLine());
    if (reply.isObject())
        return reply.object();
    return {{QStringLiteral("ok"), true}}; // delivered; no reply from an older app
}

bool launchApp()
{
    const QString exe = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("FluxDM.exe"));
    if (!QProcess::startDetached(exe, {QStringLiteral("--tray")}))
        return false;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 20000) {
        if (appRunning())
            return true;
        QThread::msleep(250);
    }
    return false;
}

QJsonObject handle(const QJsonObject& req)
{
    const QString cmd = req.value(QStringLiteral("cmd")).toString();
    if (cmd == QLatin1String("ping"))
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("version"), QStringLiteral(FLUXDM_VERSION)},
                {QStringLiteral("running"), appRunning()}};
    static const QStringList forwarded{QStringLiteral("download"), QStringLiteral("downloadMany"),
                                       QStringLiteral("torrentFile"), QStringLiteral("show")};
    if (!forwarded.contains(cmd))
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("unknown command")}};
    if (!appRunning() && !launchApp())
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Cannot start FluxDM")}};
    return deliver(req);
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    // The browser has the foreground; let FluxDM bring its Add dialog to the front.
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    QByteArray msg;
    while (readMessage(msg)) {
        const QJsonDocument doc = QJsonDocument::fromJson(msg);
        writeMessage(doc.isObject() ? handle(doc.object())
                                    : QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("bad message")}});
    }
    return 0;
}
