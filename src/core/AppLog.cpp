#include "core/AppLog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QSysInfo>
#include <QTextStream>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

namespace AppLog {
namespace {

QMutex g_lock;
thread_local int t_inside = 0; // this thread is inside the logger: Qt warnings from QFile must not re-enter
struct Locked {
    Locked() { g_lock.lock(); ++t_inside; }
    ~Locked() { --t_inside; g_lock.unlock(); }
};
QString g_dir;
QFile g_file;
bool g_crashedBefore = false;
QtMessageHandler g_previous = nullptr;
constexpr qint64 kMaxSize = 1024 * 1024;
constexpr int kKeep = 3;

QString fileAt(int i)
{
    return QDir(g_dir).filePath(i == 0 ? QStringLiteral("fluxdm.log") : QStringLiteral("fluxdm.%1.log").arg(i));
}
QString sessionMarker() { return QDir(g_dir).filePath(QStringLiteral("session.running")); }

void rotateLocked()
{
    g_file.close();
    QFile::remove(fileAt(kKeep - 1));
    for (int i = kKeep - 2; i >= 0; --i)
        QFile::rename(fileAt(i), fileAt(i + 1));
    g_file.setFileName(fileAt(0));
    g_file.open(QIODevice::Append | QIODevice::Text);
}

void writeLocked(const QString& line)
{
    if (!g_file.isOpen())
        return;
    if (g_file.size() > kMaxSize)
        rotateLocked();
    const QByteArray l = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz ")).toUtf8()
                       + line.toUtf8() + '\n';
    g_file.write(l);
    g_file.flush();
}

void handler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    static const char* kinds[] = {"debug", "warning", "critical", "fatal", "info"};
    if (t_inside == 0) {
        Locked l;
        writeLocked(QStringLiteral("[%1] %2").arg(QLatin1String(kinds[qBound(0, int(type), 4)]), msg));
    }
    if (g_previous)
        g_previous(type, ctx, msg);
}

#ifdef Q_OS_WIN
LONG WINAPI onCrash(EXCEPTION_POINTERS* ep)
{
    const QString path = QDir(g_dir).filePath(QStringLiteral("crash-%1.dmp").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    writeMiniDump(path, ep);
    // The crash may have happened while the lock was held: never wait for it here.
    if (g_lock.tryLock(200)) {
        writeLocked(QStringLiteral("[fatal] crashed (exception 0x%1); dump: %2")
                        .arg(ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0, 8, 16, QLatin1Char('0'))
                        .arg(QFileInfo(path).fileName()));
        g_lock.unlock();
    }
    return EXCEPTION_CONTINUE_SEARCH; // let Windows end the process as usual
}
#endif

} // namespace

void init(const QString& logDir)
{
    {
        Locked l;
        if (g_file.isOpen()) // init() again (tests; a restart in-process): reopen cleanly
            g_file.close();
        g_dir = logDir;
        QDir().mkpath(g_dir);
        // The marker survives only if the last session didn't end through shutdown().
        g_crashedBefore = QFile::exists(sessionMarker());
        QFile marker(sessionMarker());
        if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
            marker.write(QByteArray::number(QCoreApplication::applicationPid()));
        g_file.setFileName(fileAt(0));
        g_file.open(QIODevice::Append | QIODevice::Text);
        writeLocked(QStringLiteral("---- FluxDM %1 started on %2 (Qt %3)%4")
                        .arg(QCoreApplication::applicationVersion(), QSysInfo::prettyProductName(), QString::fromLatin1(qVersion()),
                             g_crashedBefore ? QStringLiteral("; the previous session ended unexpectedly") : QString()));
    }
    const QtMessageHandler prev = qInstallMessageHandler(handler);
    if (prev != handler) // init() twice must not chain the handler to itself
        g_previous = prev;
#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(onCrash);
#endif
}

void shutdown()
{
    Locked l;
    writeLocked(QStringLiteral("---- FluxDM closed"));
    g_file.close();
    QFile::remove(sessionMarker());
}

void write(const QString& line)
{
    Locked l;
    writeLocked(line);
}

QString logDir() { return g_dir; }
QString currentFile() { return fileAt(0); }
bool previousSessionCrashed() { return g_crashedBefore; }

QStringList tail(int lines)
{
    Locked l;
    QStringList out;
    for (int i = 0; i < kKeep && out.size() < lines; ++i) {
        QFile f(fileAt(i));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QStringList all = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        out = all.mid(qMax(0, int(all.size()) - (lines - int(out.size())))) + out;
    }
    return out;
}

QStringList crashDumps()
{
    QStringList out;
    for (const QFileInfo& fi : QDir(g_dir).entryInfoList({QStringLiteral("*.dmp")}, QDir::Files, QDir::Time))
        out << fi.absoluteFilePath();
    return out;
}

bool writeMiniDump(const QString& path, void* exceptionPointers)
{
#ifdef Q_OS_WIN
    HANDLE file = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = static_cast<EXCEPTION_POINTERS*>(exceptionPointers);
    mei.ClientPointers = FALSE;
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                      MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                                      exceptionPointers ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(file);
    if (!ok)
        QFile::remove(path);
    return ok;
#else
    Q_UNUSED(path);
    Q_UNUSED(exceptionPointers);
    return false;
#endif
}

QString report(int logLines)
{
    QString r;
    QTextStream s(&r);
    s << "FluxDM " << QCoreApplication::applicationVersion() << "\n";
    s << "OS: " << QSysInfo::prettyProductName() << " (" << QSysInfo::currentCpuArchitecture() << ", kernel "
      << QSysInfo::kernelVersion() << ")\n";
    s << "Qt: " << qVersion() << "\n";
    s << "Previous session ended unexpectedly: " << (g_crashedBefore ? "yes" : "no") << "\n";
    const QStringList dumps = crashDumps();
    s << "Crash dumps: " << (dumps.isEmpty() ? QStringLiteral("none") : QString()) << "\n";
    for (const QString& d : dumps)
        s << "  " << QDir::toNativeSeparators(d) << " (" << QFileInfo(d).lastModified().toString(Qt::ISODate) << ")\n";
    s << "\nLast " << logLines << " log lines:\n";
    for (const QString& l : tail(logLines))
        s << l << "\n";
    return r;
}

} // namespace AppLog
