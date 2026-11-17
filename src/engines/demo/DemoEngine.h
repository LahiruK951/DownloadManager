#pragma once
#include <QHash>
#include <QTimer>

#include "core/DownloadEngine.h"

// Developer-only module: simulates downloads for demo:// URLs so the UI can be
// exercised and screenshotted without any network. Query parameters:
//   size=<bytes>  speed=<bytes/s>  start=<0..1>  conns=<n>  fail=1
// Registered only when FLUXDM_DEV_DEMO is set.
class DemoEngine : public DownloadEngine {
    Q_OBJECT
public:
    explicit DemoEngine(QObject* parent = nullptr);

    QString moduleUid() const override { return QStringLiteral("downloadsdemo"); }
    bool canHandle(const QUrl& url) const override { return url.scheme() == QLatin1String("demo"); }
    void start(const TaskParams& params) override;
    void stop(qint64 id) override;
    void setSpeedLimit(qint64 bytesPerSec) override { m_limit = bytesPerSec; }
    void setMaxConnections(int) override {}
    QHash<qint64, QByteArray> shutdown() override;

private:
    struct Task {
        QString name, path;
        qint64 total = 0;
        double received = 0;
        double speed = 0;
        int conns = 8;
        bool fail = false;
        int ticks = 0;
    };
    void tick();
    static QByteArray resume(const Task& t);

    QHash<qint64, Task> m_tasks;
    QTimer m_timer;
    qint64 m_limit = 0;
};
