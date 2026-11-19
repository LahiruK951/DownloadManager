#pragma once
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

// Reads the download list of an installed Free Download Manager 6 (its db.sqlite), for
// "Import from Free Download Manager". Works on a temporary copy, read-only: FDM's own
// database is never opened for writing or locked.
namespace FdmImport {

struct Item {
    QString moduleUid; // downloadswww / downloadsbt
    QString url, referrer, userAgent;
    QString destDir, title;
    QDateTime created, finished; // finished invalid = not finished
    int priority = 1;
    QStringList tags;            // FDM tag names
    QString tagColor(const QString&) const;
    QHash<QString, QString> tagColors;
};

QString defaultDatabase(); // %LOCALAPPDATA%\Softdeluxe\Free Download Manager\db.sqlite
QList<Item> read(const QString& dbPath, QString* error);

} // namespace FdmImport
