#pragma once
#include "core/DownloadEngine.h"

class CurlWorker;

// Module "downloadswww": HTTP, HTTPS, FTP, FTPS.
class WwwEngine : public DownloadEngine {
    Q_OBJECT
public:
    explicit WwwEngine(QObject* parent = nullptr);
    ~WwwEngine() override;

    QString moduleUid() const override { return QStringLiteral("downloadswww"); }
    bool canHandle(const QUrl& url) const override;
    void start(const TaskParams& params) override;
    void stop(qint64 id) override;
    void setSpeedLimit(qint64 bytesPerSec) override;
    void setMaxConnections(int perDownload) override;
    QHash<qint64, QByteArray> shutdown() override;

private:
    CurlWorker* m_worker;
};
