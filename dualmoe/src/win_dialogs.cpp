// Native Windows "Open file" dialog (IFileOpenDialog) used by the desktop app so
// the user picks a .gguf model with the standard system picker instead of a
// browser-only dialog.
#include "win_dialogs.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace {

std::string wide_to_utf8(const std::wstring & w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t) n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int) w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string & s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t) n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), out.data(), n);
    return out;
}

} // namespace

namespace dualmoe {

bool native_file_dialog_available() { return true; }

bool pick_gguf_file(const std::string & start_dir, std::string & out_path, bool & canceled) {
    out_path.clear();
    canceled = true;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninit = SUCCEEDED(hr);
    if (!uninit && hr != RPC_E_CHANGED_MODE) return false;

    IFileOpenDialog * dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))) || !dlg) {
        if (uninit) CoUninitialize();
        return false;
    }

    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    }
    const COMDLG_FILTERSPEC filters[] = {
        {L"GGUF модели", L"*.gguf"},
        {L"Все файлы", L"*.*"},
    };
    dlg->SetFileTypes(2, filters);
    dlg->SetTitle(L"DualMoE — выберите .gguf модель");

    if (!start_dir.empty()) {
        const std::wstring initial = fs::path(utf8_to_wide(start_dir)).wstring();
        IShellItem * folder = nullptr;
        if (!initial.empty() &&
            SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder))) &&
            folder) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }

    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem * item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                out_path = wide_to_utf8(std::wstring(path));
                CoTaskMemFree(path);
                canceled = false;
            }
            item->Release();
        }
    }

    dlg->Release();
    if (uninit) CoUninitialize();
    return !out_path.empty();
}

} // namespace dualmoe

#else // !_WIN32

namespace dualmoe {

bool native_file_dialog_available() { return false; }

bool pick_gguf_file(const std::string &, std::string & out_path, bool & canceled) {
    out_path.clear();
    canceled = true;
    return false;
}

} // namespace dualmoe

#endif
