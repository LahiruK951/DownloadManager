#pragma once
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkReply;
class QProcess;

// ffmpeg and yt-dlp as FluxDM ships them ("<app>/tools", written by scripts\fetch-tools.ps1) or
// updated them ("<profile>/tools", always writable). Each folder has a versions.json, so finding
// the newest copy never has to start a program. Updates come from the projects' GitHub releases
// (yt-dlp, and gyan.dev's ffmpeg builds) and are only installed when the file's SHA-256 matches
// the digest GitHub publishes for it. Copies the user chose (setting, PATH) are left alone.
class ExternalTools : public QObject {
    Q_OBJECT
public:
    enum Tool { YtDlp = 0, Ffmpeg = 1 };
    static constexpr int kCount = 2;
    struct Copy {
        QString path, version;
        bool bundled = false; // shipped with FluxDM (else updated into the profile)
    };

    ExternalTools(const QString& profileToolsDir, const QString& bundledToolsDir, QObject* parent = nullptr);

    static QString key(Tool t);     // "yt-dlp", "ffmpeg" (also the versions.json keys)
    static QString exeName(Tool t); // "yt-dlp.exe", "ffmpeg.exe"
    // The newest copy FluxDM manages; empty path = none.
    Copy managed(Tool t) const;
    QString profileDir() const { return m_profileDir; }

    QString latest(Tool t) const { return m_rel[t].version; }
    // A newer release than the managed copy (or no managed copy at all) is known.
    bool updateAvailable(Tool t) const;
    bool busy(Tool t) const { return m_busy[t]; }
    QString status(Tool t) const { return m_status[t]; }

    // GitHub "releases/latest" API addresses; tests point them at a local server.
    void setReleaseApi(Tool t, const QUrl& url) { m_api[t] = url; }
    // 7-Zip or Windows' tar.exe, for the ffmpeg archive.
    void setExtractor(const QString& path) { m_extractor = path; }

    void checkLatest(); // emits checked() when both answers are in (or failed)
    void update(Tool t); // emits updated()

    static int compareVersions(const QString& a, const QString& b); // "2026.10.26", "9.1", "7.1.1"

signals:
    void changed(); // status, progress, versions
    void checked();
    void updated(int tool, bool ok, const QString& message);

private:
    struct Release {
        QString version;
        QUrl url;
        QByteArray sha256; // hex, lower case
        QString assetName;
    };
    void fetchRelease(Tool t, bool thenInstall);
    void download(Tool t);
    void install(Tool t, const QString& file);
    void finish(Tool t, bool ok, const QString& message);
    void setStatus(Tool t, const QString& s);
    static QJsonObject readVersions(const QString& dir);
    bool writeVersion(Tool t, const QString& version);

    QString m_profileDir, m_bundledDir, m_extractor;
    QUrl m_api[kCount];
    Release m_rel[kCount];
    bool m_busy[kCount] = {false, false};
    QString m_status[kCount];
    int m_pendingChecks = 0;
    QNetworkAccessManager m_nam;
};
