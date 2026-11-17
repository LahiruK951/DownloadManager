#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QString>

#include "core/Schedule.h"

// Persistent part of a download; one row of the `downloads` table
// (column naming follows FDM's schema where an equivalent exists).
struct DownloadRecord {
    qint64 id = 0;
    QString moduleUid = QStringLiteral("downloadswww");
    QString url;
    QString referrer;
    QString cookies;
    QString userAgent;
    QString destDir;            // destinationPath
    QString preferredName;      // preferredFileTitle (user supplied)
    QString title;              // resolved file name
    QString filePath;           // full path of final file once known
    QDateTime created;          // creationTime
    QDateTime finished;         // finishedTime
    QByteArray resumeData;
    QString lastError;          // lastResult
    int state = 0;              // Download::State
    qint64 total = -1;
    qint64 received = 0;
    int resumable = -1;         // resumeSupport: -1 unknown, 0 no, 1 yes
    int priority = 1;           // 0 low, 1 normal, 2 high
    bool scheduled = false;     // downloadsScheduleRecords row present
    ScheduleWindow schedule;
};
