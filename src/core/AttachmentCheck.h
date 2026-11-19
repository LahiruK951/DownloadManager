#pragma once
#include <QString>

// Hands a finished download to Windows' attachment services, the way browsers do:
// the registered antivirus scans it and the file gets the "Mark of the Web" (Zone.Identifier,
// with the source URL), so Windows warns before running it. Blocking: call off the GUI thread.
namespace AttachmentCheck {

enum Result { Clean, Blocked, Unavailable };

// message: what the antivirus/Windows said when it was blocked or the check couldn't run.
Result check(const QString& filePath, const QString& sourceUrl, const QString& referrer, QString* message);

} // namespace AttachmentCheck
