#include "core/Database.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QDebug>

static const char* kConn = "fluxdm";

Database::~Database()
{
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(QString::fromLatin1(kConn));
}

bool Database::exec(const QString& sql)
{
    QSqlQuery q(m_db);
    if (!q.exec(sql)) {
        qWarning() << "SQL error:" << q.lastError().text() << sql;
        return false;
    }
    return true;
}

bool Database::open(const QString& dir, QString* error)
{
    m_dir = dir;
    QDir().mkpath(dir);
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QString::fromLatin1(kConn));
    m_db.setDatabaseName(QDir(dir).filePath(QStringLiteral("db.sqlite")));
    if (!m_db.open()) {
        if (error)
            *error = m_db.lastError().text();
        return false;
    }
    exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    const char* schema[] = {
        "CREATE TABLE IF NOT EXISTS downloads (id INTEGER PRIMARY KEY, moduleUid TEXT, url TEXT, referrer TEXT,"
        " cookies BLOB, userAgent TEXT, destinationPath TEXT, preferredFileTitle TEXT, title TEXT, filePath TEXT,"
        " creationTime INTEGER, finishedTime INTEGER, resumeData BLOB, lastResult TEXT, state INTEGER,"
        " totalBytes INTEGER, receivedBytes INTEGER, resumeSupport INTEGER, priority INTEGER)",
        "CREATE TABLE IF NOT EXISTS tags (id INTEGER PRIMARY KEY, name TEXT, color TEXT)",
        "CREATE TABLE IF NOT EXISTS downloadsTags (downloadId INTEGER PRIMARY KEY, tagsIds TEXT)",
        "CREATE TABLE IF NOT EXISTS downloadsTagsDetails (tagId INTEGER PRIMARY KEY, defaultExtensions TEXT, defaultDownloadFolder TEXT)",
        "CREATE TABLE IF NOT EXISTS downloadsScheduleRecords (downloadId INTEGER PRIMARY KEY, days INTEGER, fromTime INTEGER, toTime INTEGER)",
        "CREATE TABLE IF NOT EXISTS Settings (Name TEXT PRIMARY KEY, Value TEXT)",
    };
    for (const char* s : schema)
        if (!exec(QString::fromLatin1(s))) {
            if (error)
                *error = QStringLiteral("Cannot create schema");
            return false;
        }

    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT Name, Value FROM Settings")))
        while (q.next())
            m_settings.insert(q.value(0).toString(), q.value(1));
    return true;
}

void Database::backup()
{
    const QString bdir = QDir(m_dir).filePath(QStringLiteral("backup/db"));
    QDir().mkpath(bdir);
    const QString daily = QDir(bdir).filePath(QStringLiteral("db.sqlite.daily"));
    const QFileInfo fi(daily);
    if (fi.exists() && fi.lastModified().secsTo(QDateTime::currentDateTime()) < 24 * 3600)
        return;
    exec(QStringLiteral("PRAGMA wal_checkpoint(FULL)"));
    const QString older = QDir(bdir).filePath(QStringLiteral("db.sqlite.older"));
    const QString newer = QDir(bdir).filePath(QStringLiteral("db.sqlite.newer"));
    QFile::remove(older);
    QFile::rename(newer, older);
    QFile::rename(daily, newer);
    QFile::copy(m_db.databaseName(), daily);
}

QList<DownloadRecord> Database::loadDownloads()
{
    QList<DownloadRecord> out;
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral(
            "SELECT id, moduleUid, url, referrer, cookies, userAgent, destinationPath, preferredFileTitle, title,"
            " filePath, creationTime, finishedTime, resumeData, lastResult, state, totalBytes, receivedBytes,"
            " resumeSupport, priority FROM downloads ORDER BY id DESC"))) {
        qWarning() << q.lastError().text();
        return out;
    }
    while (q.next()) {
        DownloadRecord r;
        r.id = q.value(0).toLongLong();
        r.moduleUid = q.value(1).toString();
        r.url = q.value(2).toString();
        r.referrer = q.value(3).toString();
        r.cookies = QString::fromUtf8(q.value(4).toByteArray());
        r.userAgent = q.value(5).toString();
        r.destDir = q.value(6).toString();
        r.preferredName = q.value(7).toString();
        r.title = q.value(8).toString();
        r.filePath = q.value(9).toString();
        r.created = QDateTime::fromSecsSinceEpoch(q.value(10).toLongLong());
        const qint64 fin = q.value(11).toLongLong();
        if (fin > 0)
            r.finished = QDateTime::fromSecsSinceEpoch(fin);
        r.resumeData = q.value(12).toByteArray();
        r.lastError = q.value(13).toString();
        r.state = q.value(14).toInt();
        r.total = q.value(15).toLongLong();
        r.received = q.value(16).toLongLong();
        r.resumable = q.value(17).toInt();
        r.priority = q.value(18).toInt();
        out.append(r);
    }
    return out;
}

void Database::save(const DownloadRecord& r)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO downloads (id, moduleUid, url, referrer, cookies, userAgent, destinationPath,"
        " preferredFileTitle, title, filePath, creationTime, finishedTime, resumeData, lastResult, state,"
        " totalBytes, receivedBytes, resumeSupport, priority)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(r.id);
    q.addBindValue(r.moduleUid);
    q.addBindValue(r.url);
    q.addBindValue(r.referrer);
    q.addBindValue(r.cookies.toUtf8());
    q.addBindValue(r.userAgent);
    q.addBindValue(r.destDir);
    q.addBindValue(r.preferredName);
    q.addBindValue(r.title);
    q.addBindValue(r.filePath);
    q.addBindValue(r.created.toSecsSinceEpoch());
    q.addBindValue(r.finished.isValid() ? r.finished.toSecsSinceEpoch() : qint64(0));
    q.addBindValue(r.resumeData);
    q.addBindValue(r.lastError);
    q.addBindValue(r.state);
    q.addBindValue(r.total);
    q.addBindValue(r.received);
    q.addBindValue(r.resumable);
    q.addBindValue(r.priority);
    if (!q.exec())
        qWarning() << "save download:" << q.lastError().text();
}

void Database::saveProgress(qint64 id, qint64 received, const QByteArray& resumeData)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE downloads SET receivedBytes = ?, resumeData = ? WHERE id = ?"));
    q.addBindValue(received);
    q.addBindValue(resumeData);
    q.addBindValue(id);
    q.exec();
}

void Database::remove(qint64 id)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM downloads WHERE id = ?"));
    q.addBindValue(id);
    q.exec();
}

qint64 Database::maxId()
{
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT MAX(id) FROM downloads")) && q.next())
        return q.value(0).toLongLong();
    return 0;
}

QVariant Database::setting(const QString& name, const QVariant& def)
{
    return m_settings.value(name, def);
}

void Database::setSetting(const QString& name, const QVariant& value)
{
    m_settings.insert(name, value);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO Settings (Name, Value) VALUES (?, ?)"));
    q.addBindValue(name);
    q.addBindValue(value.toString());
    q.exec();
}
