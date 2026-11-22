#include "engines/www/FileWriter.h"

#include <QDir>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

static QString lastWinError()
{
    const DWORD code = GetLastError();
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString s = buf ? QString::fromWCharArray(buf).trimmed() : QStringLiteral("Windows error %1").arg(code);
    if (buf)
        LocalFree(buf);
    return s;
}

bool FileWriter::open(const QString& path, bool truncate, QString* error)
{
    close();
    const QString native = QDir::toNativeSeparators(path);
    HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           truncate ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        m_error = lastWinError();
        if (error)
            *error = m_error;
        return false;
    }
    DWORD br = 0;
    DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &br, nullptr); // best effort
    m_h = h;
    return true;
}

bool FileWriter::preallocate(qint64 size)
{
    if (!m_h)
        return false;
    FILE_END_OF_FILE_INFO eof{};
    eof.EndOfFile.QuadPart = size;
    if (!SetFileInformationByHandle(static_cast<HANDLE>(m_h), FileEndOfFileInfo, &eof, sizeof(eof))) {
        m_error = lastWinError();
        return false;
    }
    return true;
}

bool FileWriter::write(qint64 offset, const char* data, qint64 len)
{
    if (!m_h)
        return false;
    while (len > 0) {
        OVERLAPPED ov{};
        ov.Offset = DWORD(quint64(offset) & 0xFFFFFFFFull);
        ov.OffsetHigh = DWORD(quint64(offset) >> 32);
        const DWORD chunk = DWORD(qMin<qint64>(len, 1 << 30));
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(m_h), data, chunk, &written, &ov) || written == 0) {
            m_error = lastWinError();
            return false;
        }
        offset += written;
        data += written;
        len -= written;
        m_unflushed = true;
    }
    return true;
}

bool FileWriter::flush()
{
    if (!m_h || !m_unflushed)
        return true;
    m_unflushed = false;
    ++m_flushes;
    if (!FlushFileBuffers(static_cast<HANDLE>(m_h))) {
        m_error = lastWinError();
        return false;
    }
    return true;
}

bool FileWriter::flushAsync(std::function<void(bool)> done)
{
    if (m_pending.valid() && m_pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return false; // the disk is still busy with the previous one
    if (!m_h || !m_unflushed) {
        done(true);
        return true;
    }
    m_unflushed = false;
    HANDLE h = static_cast<HANDLE>(m_h); // stays open: close() waits for this
    m_pending = std::async(std::launch::async, [this, h, done = std::move(done)] {
        ++m_flushes;
        done(FlushFileBuffers(h) != 0);
    });
    return true;
}

void FileWriter::close()
{
    if (m_pending.valid())
        m_pending.wait();
    if (m_h) {
        flush();
        CloseHandle(static_cast<HANDLE>(m_h));
        m_h = nullptr;
    }
}

bool FileWriter::isOpen() const { return m_h != nullptr; }

#else // portable fallback

bool FileWriter::open(const QString& path, bool truncate, QString* error)
{
    close();
    m_f.setFileName(path);
    const auto mode = truncate ? (QIODevice::ReadWrite | QIODevice::Truncate) : QIODevice::ReadWrite;
    if (!m_f.open(mode)) {
        m_error = m_f.errorString();
        if (error)
            *error = m_error;
        return false;
    }
    return true;
}

bool FileWriter::preallocate(qint64 size)
{
    if (!m_f.resize(size)) {
        m_error = m_f.errorString();
        return false;
    }
    return true;
}

bool FileWriter::write(qint64 offset, const char* data, qint64 len)
{
    if (!m_f.seek(offset) || m_f.write(data, len) != len) {
        m_error = m_f.errorString();
        return false;
    }
    m_unflushed = true;
    return true;
}

bool FileWriter::flush()
{
    if (!m_f.isOpen() || !m_unflushed)
        return true;
    m_unflushed = false;
    ++m_flushes;
    return m_f.flush();
}

bool FileWriter::flushAsync(std::function<void(bool)> done)
{
    done(flush()); // portable fallback: in place
    return true;
}

void FileWriter::close()
{
    flush();
    m_f.close();
}
bool FileWriter::isOpen() const { return m_f.isOpen(); }

#endif
