#include "core/AttachmentCheck.h"

#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#endif

namespace AttachmentCheck {

#ifdef Q_OS_WIN
static QString hrText(HRESULT hr)
{
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   DWORD(hr), 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    const QString s = buf ? QString::fromWCharArray(buf).trimmed() : QStringLiteral("error 0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0'));
    if (buf)
        LocalFree(buf);
    return s;
}
#endif

Result check(const QString& filePath, const QString& sourceUrl, const QString& referrer, QString* message)
{
#ifdef Q_OS_WIN
    if (!QFileInfo(filePath).isFile()) {
        *message = QStringLiteral("not a file");
        return Unavailable;
    }
    // Attachment services want a single-threaded apartment on this thread.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IAttachmentExecute* ae = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_AttachmentServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ae));
    Result result = Unavailable;
    if (SUCCEEDED(hr)) {
        // Fixed client GUID: lets Windows remember per-application prompts for FluxDM.
        static const GUID kClient = {0x8f7a3c21, 0x5d4e, 0x4b6a, {0x9c, 0x1e, 0x2a, 0x6f, 0x3d, 0x8b, 0x7e, 0x45}};
        ae->SetClientGuid(kClient);
        ae->SetClientTitle(L"FluxDM");
        ae->SetLocalPath(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(filePath).utf16()));
        if (!sourceUrl.isEmpty())
            ae->SetSource(reinterpret_cast<const wchar_t*>(sourceUrl.utf16()));
        if (!referrer.isEmpty())
            ae->SetReferrer(reinterpret_cast<const wchar_t*>(referrer.utf16()));
        hr = ae->Save(); // runs the antivirus and writes Zone.Identifier
        if (SUCCEEDED(hr) && QFileInfo::exists(filePath)) {
            result = Clean;
        } else {
            result = Blocked; // the antivirus refused it (and may have removed the file)
            *message = hrText(hr);
        }
        ae->Release();
    } else {
        *message = QStringLiteral("Windows attachment services are not available (0x%1)").arg(quint32(hr), 8, 16, QLatin1Char('0'));
    }
    if (SUCCEEDED(init))
        CoUninitialize();
    return result;
#else
    Q_UNUSED(filePath);
    Q_UNUSED(sourceUrl);
    Q_UNUSED(referrer);
    *message = QStringLiteral("not available on this platform");
    return Unavailable;
#endif
}

} // namespace AttachmentCheck
