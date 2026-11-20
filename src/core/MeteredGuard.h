#pragma once
#include <QList>
#include <QObject>

class DownloadManager;

// What to do when Windows says the connection is metered (phone hotspot, data limit).
// FDM asks before using mobile data; so does this by default. Downloads it paused for that
// reason are resumed when the connection is no longer metered.
class MeteredGuard : public QObject {
    Q_OBJECT
public:
    enum Policy { Ask = 0, Pause = 1, Ignore = 2 };
    explicit MeteredGuard(DownloadManager* mgr, QObject* parent = nullptr);

    void setPolicy(int p) { m_policy = p; }
    int policy() const { return m_policy; }
    void setMetered(bool metered); // from QNetworkInformation (or tests)
    bool metered() const { return m_metered; }
    void pauseNow();               // the user chose "Pause" in the question
    void keepDownloading();        // the user chose "Keep downloading" (until it's unmetered again)
    // Follows QNetworkInformation when the platform reports metered connections.
    static bool watchSystem(MeteredGuard* guard);

signals:
    void askUser(int activeDownloads); // policy Ask: show the question
    void paused(int count);
    void resumed(int count);

private:
    DownloadManager* m_mgr;
    int m_policy = Ask;
    bool m_metered = false;
    bool m_allowed = false;         // "keep downloading" for this metered period
    QList<qint64> m_pausedByUs;
};
