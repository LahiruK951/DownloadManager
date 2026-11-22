#pragma once
#include <QFile>
#include <QString>
#include <functional>
#include <atomic>
#include <future>

// Positional writer for segmented downloads. On Windows the file is created
// sparse and sized up-front so each connection can write at its own offset.
class FileWriter {
public:
    FileWriter() = default;
    ~FileWriter() { close(); }
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;

    bool open(const QString& path, bool truncate, QString* error = nullptr);
    bool preallocate(qint64 size);
    bool write(qint64 offset, const char* data, qint64 len);
    // Pushes written data from the OS cache to the disk, so resume data saved afterwards never
    // claims bytes a power cut could still lose. No-op when nothing was written since the last flush.
    bool flush();
    // The same on a helper thread, so a slow disk doesn't hold up the caller (the download thread);
    // `done` runs on that thread afterwards. Skipped (returns false) while the previous one runs.
    bool flushAsync(std::function<void(bool ok)> done);
    void close(); // waits for a pending flushAsync, then flushes
    qint64 flushes() const { return m_flushes.load(); } // for tests
    bool isOpen() const;
    QString lastError() const { return m_error; }

private:
#ifdef Q_OS_WIN
    void* m_h = nullptr; // HANDLE
#else
    QFile m_f;
#endif
    QString m_error;
    bool m_unflushed = false;
    std::atomic<qint64> m_flushes{0};
    std::future<void> m_pending;
};
