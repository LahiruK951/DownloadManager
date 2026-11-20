#include "core/Database.h"
#include "core/Secret.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
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
        "CREATE TABLE IF NOT EXISTS networkPasswords (server TEXT PRIMARY KEY, user TEXT, password BLOB)",
        "CREATE TABLE IF NOT EXISTS sslKnownHosts (host TEXT PRIMARY KEY, since INTEGER)",
    };
    for (const char* s : schema)
        if (!exec(QString::fromLatin1(s))) {
            if (error)
                *error = QStringLiteral("Cannot create schema");
            return false;
        }

    // Schema migrations: columns added after the first release.
    {
        QSet<QString> cols;
        QSqlQuery info(m_db);
        if (info.exec(QStringLiteral("PRAGMA table_info(downloads)")))
            while (info.next())
                cols.insert(info.value(1).toString());
        if (!cols.contains(QStringLiteral("speedLimit")))
            exec(QStringLiteral("ALTER TABLE downloads ADD COLUMN speedLimit INTEGER DEFAULT 0"));
        if (!cols.contains(QStringLiteral("existingFileReaction")))
            exec(QStringLiteral("ALTER TABLE downloads ADD COLUMN existingFileReaction INTEGER DEFAULT 0"));
        if (!cols.contains(QStringLiteral("pickFiles")))
            exec(QStringLiteral("ALTER TABLE downloads ADD COLUMN pickFiles INTEGER DEFAULT 0"));
        if (!cols.contains(QStringLiteral("sequential")))
            exec(QStringLiteral("ALTER TABLE downloads ADD COLUMN sequential INTEGER DEFAULT 0"));
        if (!cols.contains(QStringLiteral("downloadOrder"))) {
            exec(QStringLiteral("ALTER TABLE downloads ADD COLUMN downloadOrder INTEGER"));
            exec(QStringLiteral("UPDATE downloads SET downloadOrder = id")); // keep today's first-come order
        }
        if (!cols.contains(QStringLiteral("postData")))
            exec(QStringLiteral("ALTER TABLE downloads ADD COLUMN postData BLOB"));
        if (!cols.contains(QStringLiteral("contentCategory")))
            exec(QStringLiteral("ALTER TABLE downloads ADD COLUMN contentCategory TEXT"));
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
            " resumeSupport, priority, speedLimit, contentCategory, existingFileReaction, pickFiles, sequential, downloadOrder, postData FROM downloads ORDER BY id DESC"))) {
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
        r.speedLimitKBps = q.value(19).toInt();
        r.contentCategory = q.value(20).toString();
        r.existingFileReaction = q.value(21).toInt();
        r.pickFiles = q.value(22).toBool();
        r.sequential = q.value(23).toBool();
        r.queueOrder = q.value(24).isNull() ? r.id : q.value(24).toLongLong();
        r.postData = q.value(25).toByteArray();
        out.append(r);
    }

    QSqlQuery s(m_db);
    if (s.exec(QStringLiteral("SELECT downloadId, days, fromTime, toTime FROM downloadsScheduleRecords"))) {
        QHash<qint64, ScheduleWindow> sched;
        while (s.next()) {
            ScheduleWindow w;
            w.days = s.value(1).toInt();
            w.fromMin = s.value(2).toInt();
            w.toMin = s.value(3).toInt();
            sched.insert(s.value(0).toLongLong(), w);
        }
        for (DownloadRecord& r : out)
            if (sched.contains(r.id)) {
                r.scheduled = true;
                r.schedule = sched.value(r.id);
            }
    }

    QSqlQuery tq(m_db);
    if (tq.exec(QStringLiteral("SELECT downloadId, tagsIds FROM downloadsTags"))) {
        QHash<qint64, QList<int>> tags;
        while (tq.next())
            for (const QString& s : tq.value(1).toString().split(QLatin1Char(','), Qt::SkipEmptyParts))
                tags[tq.value(0).toLongLong()] << s.toInt();
        for (DownloadRecord& r : out)
            r.tags = tags.value(r.id);
    }
    return out;
}

void Database::save(const DownloadRecord& r)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO downloads (id, moduleUid, url, referrer, cookies, userAgent, destinationPath,"
        " preferredFileTitle, title, filePath, creationTime, finishedTime, resumeData, lastResult, state,"
        " totalBytes, receivedBytes, resumeSupport, priority, speedLimit, contentCategory, existingFileReaction, pickFiles, sequential, downloadOrder, postData)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
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
    q.addBindValue(r.speedLimitKBps);
    q.addBindValue(r.contentCategory);
    q.addBindValue(r.existingFileReaction);
    q.addBindValue(r.pickFiles ? 1 : 0);
    q.addBindValue(r.sequential ? 1 : 0);
    q.addBindValue(r.queueOrder ? r.queueOrder : r.id);
    q.addBindValue(r.postData);
    if (!q.exec())
        qWarning() << "save download:" << q.lastError().text();

    QSqlQuery s(m_db);
    if (r.scheduled) {
        s.prepare(QStringLiteral("INSERT OR REPLACE INTO downloadsScheduleRecords (downloadId, days, fromTime, toTime) VALUES (?,?,?,?)"));
        s.addBindValue(r.id);
        s.addBindValue(r.schedule.days);
        s.addBindValue(r.schedule.fromMin);
        s.addBindValue(r.schedule.toMin);
    } else {
        s.prepare(QStringLiteral("DELETE FROM downloadsScheduleRecords WHERE downloadId = ?"));
        s.addBindValue(r.id);
    }
    s.exec();
    QSqlQuery tg(m_db);
    if (r.tags.isEmpty()) {
        tg.prepare(QStringLiteral("DELETE FROM downloadsTags WHERE downloadId = ?"));
        tg.addBindValue(r.id);
    } else {
        QStringList ids;
        for (int t : r.tags)
            ids << QString::number(t);
        tg.prepare(QStringLiteral("INSERT OR REPLACE INTO downloadsTags (downloadId, tagsIds) VALUES (?, ?)"));
        tg.addBindValue(r.id);
        tg.addBindValue(ids.join(QLatin1Char(',')));
    }
    tg.exec();
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
    QSqlQuery s(m_db);
    s.prepare(QStringLiteral("DELETE FROM downloadsScheduleRecords WHERE downloadId = ?"));
    s.addBindValue(id);
    s.exec();
    QSqlQuery t(m_db);
    t.prepare(QStringLiteral("DELETE FROM downloadsTags WHERE downloadId = ?"));
    t.addBindValue(id);
    t.exec();
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

// ------------------------------------------------------------------ server logins

bool Database::login(const QString& server, QString* user, QString* password)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT user, password FROM networkPasswords WHERE server = ?"));
    q.addBindValue(server);
    if (!q.exec() || !q.next())
        return false;
    *user = q.value(0).toString();
    *password = Secret::unprotect(q.value(1).toByteArray());
    return !user->isEmpty();
}

void Database::saveLogin(const QString& server, const QString& user, const QString& password)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO networkPasswords (server, user, password) VALUES (?,?,?)"));
    q.addBindValue(server);
    q.addBindValue(user);
    q.addBindValue(Secret::protect(password));
    if (!q.exec())
        qWarning() << "save login:" << q.lastError().text();
}

void Database::removeLogin(const QString& server)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM networkPasswords WHERE server = ?"));
    q.addBindValue(server);
    q.exec();
}

int Database::loginCount()
{
    QSqlQuery q(m_db);
    return q.exec(QStringLiteral("SELECT COUNT(*) FROM networkPasswords")) && q.next() ? q.value(0).toInt() : 0;
}

void Database::clearLogins() { exec(QStringLiteral("DELETE FROM networkPasswords")); }

// ------------------------------------------------------------------ tags

QList<Tag> Database::loadTags()
{
    QList<Tag> out;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT t.id, t.name, t.color, d.defaultExtensions, d.defaultDownloadFolder FROM tags t"
                               " LEFT JOIN downloadsTagsDetails d ON d.tagId = t.id ORDER BY t.id")))
        return out;
    while (q.next()) {
        Tag t;
        t.id = q.value(0).toInt();
        t.name = q.value(1).toString();
        t.color = q.value(2).toString();
        t.extensions = Tag::parseExtensions(q.value(3).toString());
        t.folder = q.value(4).toString();
        out << t;
    }
    return out;
}

int Database::saveTag(const Tag& tag)
{
    int id = tag.id;
    QSqlQuery q(m_db);
    if (id <= 0) {
        q.prepare(QStringLiteral("INSERT INTO tags (name, color) VALUES (?, ?)"));
        q.addBindValue(tag.name);
        q.addBindValue(tag.color);
        if (!q.exec())
            return 0;
        id = q.lastInsertId().toInt();
    } else {
        q.prepare(QStringLiteral("UPDATE tags SET name = ?, color = ? WHERE id = ?"));
        q.addBindValue(tag.name);
        q.addBindValue(tag.color);
        q.addBindValue(id);
        q.exec();
    }
    QSqlQuery d(m_db);
    d.prepare(QStringLiteral("INSERT OR REPLACE INTO downloadsTagsDetails (tagId, defaultExtensions, defaultDownloadFolder) VALUES (?,?,?)"));
    d.addBindValue(id);
    d.addBindValue(tag.extensions.join(QLatin1Char(',')));
    d.addBindValue(tag.folder);
    d.exec();
    return id;
}

void Database::removeTag(int id)
{
    for (const char* sql : {"DELETE FROM tags WHERE id = ?", "DELETE FROM downloadsTagsDetails WHERE tagId = ?"}) {
        QSqlQuery q(m_db);
        q.prepare(QString::fromLatin1(sql));
        q.addBindValue(id);
        q.exec();
    }
}

// ------------------------------------------------------------------ trusted certificates

QStringList Database::trustedHosts()
{
    QStringList out;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT host FROM sslKnownHosts ORDER BY host")))
        while (q.next())
            out << q.value(0).toString();
    return out;
}

void Database::addTrustedHost(const QString& host)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO sslKnownHosts (host, since) VALUES (?, ?)"));
    q.addBindValue(host.toLower());
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    q.exec();
}

void Database::clearTrustedHosts() { exec(QStringLiteral("DELETE FROM sslKnownHosts")); }
