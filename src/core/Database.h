#pragma once
#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVariant>

#include "core/DownloadRecord.h"

// SQLite store laid out like FDM's db.sqlite (downloads, tags, Settings, ...).
class Database {
public:
    Database() = default;
    ~Database();

    bool open(const QString& dir, QString* error = nullptr);
    void backup(); // rotating copies in <dir>/backup/db (daily/newer/older), like FDM

    QList<DownloadRecord> loadDownloads();
    void save(const DownloadRecord& r);
    void saveProgress(qint64 id, qint64 received, const QByteArray& resumeData);
    void remove(qint64 id);
    qint64 maxId();

    QVariant setting(const QString& name, const QVariant& def = {});
    void setSetting(const QString& name, const QVariant& value);

private:
    bool exec(const QString& sql);
    QSqlDatabase m_db;
    QString m_dir;
    QHash<QString, QVariant> m_settings;
};
