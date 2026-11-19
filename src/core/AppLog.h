#pragma once
#include <QString>
#include <QStringList>

// Application-wide log: Qt messages and download events go to <profile>/logs/fluxdm.log
// (rotated at 1 MB: fluxdm.log, fluxdm.1.log, fluxdm.2.log). Thread-safe.
// Also detects whether the previous session ended unexpectedly (crash, killed, power loss).
namespace AppLog {

void init(const QString& logDir);  // installs the Qt message handler, starts the session
void shutdown();                   // clean exit: ends the session marker
void write(const QString& line);   // one line, timestamped
QString logDir();
QString currentFile();
QStringList tail(int lines);       // newest last, across rotated files
bool previousSessionCrashed();     // set by init()
QStringList crashDumps();          // *.dmp in the log dir, newest first

// Windows: writes a minidump of the running process (used by the crash handler; testable).
bool writeMiniDump(const QString& path, void* exceptionPointers = nullptr);
// Report for "Report a problem": version, OS, Qt, crash files, recent log.
QString report(int logLines = 200);

} // namespace AppLog
