#pragma once
#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVariant>
#include <functional>

#include "core/DownloadRecord.h"
#include "core/Tag.h"

// SQLite store laid out like FDM's db.sqlite (downloads, tags, Settings, ...).
class Database {
public:
    Database() = default;
    ~Database();

    bool open(const QString& dir, QString* error = nullptr);
    void backup(); // rotating copies in <dir>/backup/db (daily/newer/older), like FDM

    QList<DownloadRecord> loadDownloads();
    bool save(const DownloadRecord& r);          // one transaction: download, schedule, tags
    bool saveProgress(qint64 id, qint64 received, const QByteArray& resumeData);
    bool remove(qint64 id);                      // one transaction

    // Failed writes (disk full, locked or damaged file, ...). Every failure is logged; the handler
    // lets the app tell the user, since resume data that isn't saved is lost on the next start.
    void setErrorHandler(std::function<void(const QString&)> h) { m_onError = std::move(h); }
    int writeErrors() const { return m_writeErrors; }
    QString lastError() const { return m_lastError; }
    qint64 maxId();

    QVariant setting(const QString& name, const QVariant& def = {});
    void setSetting(const QString& name, const QVariant& value);

    // Saved server logins (FDM: networkPasswords). Passwords are stored protected (Secret).
    bool login(const QString& server, QString* user, QString* password);
    void saveLogin(const QString& server, const QString& user, const QString& password);
    void removeLogin(const QString& server);
    int loginCount();
    void clearLogins();

    // User tags (FDM: tags + downloadsTagsDetails); assignments live in downloadsTags.
    // Servers whose untrusted TLS certificate the user accepted (FDM: sslKnownHosts).
    QStringList trustedHosts();
    void addTrustedHost(const QString& host);
    void clearTrustedHosts();

    QList<Tag> loadTags();
    int saveTag(const Tag& t); // inserts when t.id == 0; returns the id
    void removeTag(int id);

private:
    bool exec(const QString& sql);
    bool run(class QSqlQuery& q, const char* what); // exec() a prepared query; reports failures
    bool commitOrRollback(bool ok, const char* what);
    std::function<void(const QString&)> m_onError;
    int m_writeErrors = 0;
    QString m_lastError;
    QSqlDatabase m_db;
    QString m_dir;
    QHash<QString, QVariant> m_settings;
};
