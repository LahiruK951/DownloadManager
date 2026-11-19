#include "core/FdmImport.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

namespace FdmImport {

QString Item::tagColor(const QString& name) const { return tagColors.value(name); }

QString defaultDatabase()
{
    const QString local = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation); // %LOCALAPPDATA%
    return QDir(local).filePath(QStringLiteral("Softdeluxe/Free Download Manager/db.sqlite"));
}

QList<Item> read(const QString& dbPath, QString* error)
{
    QList<Item> out;
    if (!QFile::exists(dbPath)) {
        *error = QStringLiteral("Free Download Manager's download list was not found");
        return out;
    }
    QTemporaryDir tmp;
    const QString copy = tmp.filePath(QStringLiteral("fdm.sqlite"));
    if (!QFile::copy(dbPath, copy)) {
        *error = QStringLiteral("Cannot read %1").arg(QDir::toNativeSeparators(dbPath));
        return out;
    }
    const QString conn = QStringLiteral("fdm-import-") + QUuid::createUuid().toString(QUuid::Id128);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(copy);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) {
            *error = QStringLiteral("Cannot open Free Download Manager's download list");
        } else {
            QHash<int, QPair<QString, QString>> tags; // id -> name, colour
            QSqlQuery t(db);
            if (t.exec(QStringLiteral("SELECT id, name, color FROM tags")))
                while (t.next())
                    tags.insert(t.value(0).toInt(), {t.value(1).toString(), t.value(2).toString()});
            QHash<qint64, QList<int>> assigned;
            QSqlQuery a(db);
            if (a.exec(QStringLiteral("SELECT downloadId, tagsIds FROM downloadsTags")))
                while (a.next())
                    for (const QString& id : a.value(1).toString().split(QRegularExpression(QStringLiteral("[,; ]+")), Qt::SkipEmptyParts))
                        assigned[a.value(0).toLongLong()] << id.toInt();

            QSqlQuery q(db);
            if (!q.exec(QStringLiteral("SELECT id, moduleUid, url, referrer, userAgent, destinationPath, title, creationTime,"
                                       " finishedTime, priority FROM downloads ORDER BY id"))) {
                *error = QStringLiteral("Unexpected format of Free Download Manager's download list");
            } else {
                while (q.next()) {
                    Item it;
                    it.moduleUid = q.value(1).toString();
                    it.url = q.value(2).toString().trimmed();
                    if (it.url.isEmpty())
                        continue;
                    it.referrer = q.value(3).toString();
                    it.userAgent = q.value(4).toString();
                    it.destDir = QDir::toNativeSeparators(q.value(5).toString());
                    it.title = q.value(6).toString();
                    const qint64 created = q.value(7).toLongLong(), finished = q.value(8).toLongLong();
                    // FDM stores seconds; be tolerant of milliseconds too.
                    auto when = [](qint64 v) { return v > 100000000000LL ? QDateTime::fromMSecsSinceEpoch(v) : QDateTime::fromSecsSinceEpoch(v); };
                    it.created = created > 0 ? when(created) : QDateTime::currentDateTime();
                    if (finished > 0)
                        it.finished = when(finished);
                    it.priority = qBound(0, q.value(9).toInt(), 2);
                    for (int tagId : assigned.value(q.value(0).toLongLong()))
                        if (tags.contains(tagId) && !tags.value(tagId).first.isEmpty()) {
                            it.tags << tags.value(tagId).first;
                            it.tagColors.insert(tags.value(tagId).first, tags.value(tagId).second);
                        }
                    out << it;
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return out;
}

} // namespace FdmImport
