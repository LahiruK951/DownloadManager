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
    void close();
    bool isOpen() const;
    QString lastError() const { return m_error; }

private:
#ifdef Q_OS_WIN
    void* m_h = nullptr; // HANDLE
#else
    QFile m_f;
#endif
    QString m_error;
};
