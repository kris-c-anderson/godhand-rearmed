#include "godhand/launcher_platform.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <shobjidl.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace godhand::launcher::platform
{
    std::filesystem::path pick(void *window, bool folder)
    {
#if defined(_WIN32)
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::filesystem::path result;
        IFileOpenDialog *dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        {
            FILEOPENDIALOGOPTIONS options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
            if (folder)
                dialog->SetTitle(L"Choose the texture pack folder (the one holding replacements)");
            else
            {
                const COMDLG_FILTERSPEC filters[] = {{L"PS2 disc image (*.iso)", L"*.iso"}, {L"All files", L"*.*"}};
                dialog->SetFileTypes(2, filters);
                dialog->SetTitle(L"Choose your God Hand (USA) disc image");
            }
            if (SUCCEEDED(dialog->Show(static_cast<HWND>(window))))
            {
                IShellItem *item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)))
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                    {
                        result = std::filesystem::path(path);
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }
        if (SUCCEEDED(init))
            CoUninitialize();
        return result;
#else
        (void)window;
        (void)folder;
        return {};
#endif
    }

    void open(const std::filesystem::path &path)
    {
#if defined(_WIN32)
        ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        (void)path;
#endif
    }

    bool shiftHeld()
    {
#if defined(_WIN32)
        return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
        return false;
#endif
    }
}
