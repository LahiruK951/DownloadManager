#pragma once
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/Download.h"
#include "core/DownloadManager.h"

// Fills an empty (dev-profile) database with simulated downloads in every state.
namespace DevDemo {
inline void seed(DownloadManager& mgr, const QString& dir)
{
    if (!mgr.downloads().isEmpty())
        return;
    mgr.setTrafficRules({TrafficRule{true, ScheduleWindow{0x1F, 9 * 60, 18 * 60}, 0},
                         TrafficRule{false, ScheduleWindow{0x60, 0, 0}, 1}});
    auto add = [&](const QString& url, bool start = true) { return mgr.add(url, dir, {}, start); };
    auto pausedAt = [&](const QString& url, double frac, qint64 size) {
        Download* d = add(url, false);
        d->rec.total = size;
        d->rec.received = qint64(size * frac);
        d->rec.resumable = 1;
        d->rec.resumeData = QJsonDocument(QJsonObject{{QStringLiteral("received"), d->rec.received}}).toJson(QJsonDocument::Compact);
        d->notify();
        return d;
    };

    {   // a finished download whose file was deleted afterwards
        Download* gone = add(QStringLiteral("demo://old.example.com/meeting-recording-2025-11.mp4?size=734003200"), false);
        gone->rec.state = Download::Completed;
        gone->rec.total = gone->rec.received = 734003200;
        gone->rec.filePath = QDir(dir).filePath(QStringLiteral("meeting-recording-2025-11.mp4"));
        gone->rec.finished = QDateTime::currentDateTime().addDays(-3);
        gone->setFileMissing(true);
        gone->notify();
    }
    add(QStringLiteral("demo://docs.example.com/annual-report-2026.pdf?size=3355443&speed=1e12"));
    add(QStringLiteral("demo://media.example.com/Midnight%20Sessions%20-%2001%20-%20Intro.flac?size=48234567&speed=1e12"));
    add(QStringLiteral("demo://dl.example.com/VSCodeUserSetup-x64-1.96.exe?size=99876543&speed=1e12"));
    add(QStringLiteral("demo://photos.example.com/wallpaper-aurora-5k.png?size=14567890&speed=1e12"));
    Download* err = add(QStringLiteral("demo://broken.example.com/old-mirror/driver-pack.rar?fail=1&size=734003200"));
    Q_UNUSED(err);
    pausedAt(QStringLiteral("demo://backup.example.com/photos-2025-full-backup.zip?size=12884901888"), 0.18, 12884901888LL);
    ScheduleWindow night{0x7F, 1 * 60, 7 * 60};
    if (night.contains(QDateTime::currentDateTime()))
        night = ScheduleWindow{0x7F, 13 * 60, 14 * 60};
    mgr.add(QStringLiteral("demo://cdn.example.com/game-assets-hd-pack.7z?size=21474836480&speed=9000000"), dir, {}, true,
            {}, {}, {}, &night);
    add(QStringLiteral("demo://files.example.com/Project%20Final%20Cut%20(4K).mp4?size=2576980378&speed=5800000&start=0.71&conns=8"));
    add(QStringLiteral("demo://releases.example.com/ubuntu-24.04.1-desktop-amd64.iso?size=6203355136&speed=18500000&start=0.42&conns=16"));
}
} // namespace DevDemo
