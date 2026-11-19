#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QList>
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
    int speedLimitKBps = 0;     // per-download cap, 0 = none
    int existingFileReaction = 0; // FileNamer::ExistingFile, fixed when the download is added
    bool pickFiles = false;     // torrent: ask which files to download once the list is known
    bool sequential = false;    // torrent: download in order
    QList<int> tags;            // user tag ids (downloadsTags)
    qint64 queueOrder = 0;      // FDM downloadOrder: position in the queue (default: the id)
    QString contentCategory;    // from the file list of a multi-file download; "" = use the title
    bool scheduled = false;     // downloadsScheduleRecords row present
    ScheduleWindow schedule;
};
