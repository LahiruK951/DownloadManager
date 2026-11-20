#pragma once
#include <QFile>
#include <QString>

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
    void close(); // flushes first
    qint64 flushes() const { return m_flushes; } // for tests
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
    qint64 m_flushes = 0;
};
