// DualMoE — native desktop entry point (Windows 10/11).
//
//  * The UI (embedded Web UI, same HTML as the console build) is rendered by a
//    real Win32 window backed by the Edge WebView2 engine — no browser, no
//    address bar, no tabs, its own taskbar icon.
//  * The engine + embedded HTTP server run exactly like the console build, so
//    the OpenAI-compatible endpoint (http://127.0.0.1:8090/v1) keeps working for
//    LearnHarness / Hermes Agent while the window is open.
//  * Closing the window shuts the whole app down (no orphaned background
//    process holding the port or model RAM).
//
// Build targets:
//   DUALMOE_DESKTOP   -> DualMoE.exe        (native window, no console)
//   otherwise         -> DualMoE-server.exe (console + browser, unchanged)

#include "app_http.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>

#if defined(DUALMOE_DESKTOP)
// The WebView2 backend links against the WebView2 loader + MSVC CRT by default.
// DualMoE is built with a static runtime so the single .exe stays self-contained.
#if defined(_MSC_VER)
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "gdi32.lib")
#endif
#include "webview/webview.h"
#endif

namespace {

// UTF-8 <-> UTF-16 helpers (no locale dependency).
std::wstring widen(const std::string & s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), nullptr, 0);
    std::wstring out((size_t) n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), out.data(), n);
    return out;
}

// Best-effort: put a real application icon in the title bar / taskbar.
void apply_app_icon(HWND hwnd) {
    if (!hwnd) return;
    // NOTE: identifiers "small"/"big" are #defined away by rpcndr.h (via
    // windows.h) under MSVC, so the locals must use different names.
    HICON icon_large = (HICON) LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON icon_small = (HICON) LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                          GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (icon_large) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM) icon_large);
    if (icon_small) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM) icon_small);
}

void show_native_error(const std::wstring & title, const std::wstring & text) {
    MessageBoxW(nullptr, text.c_str(), title.c_str(), MB_ICONERROR | MB_OK);
}

void open_in_browser(const std::string & url) {
    const std::wstring wurl = widen(url);
    ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// WinMain needs the raw command line; AppOptions parsing works on argv-style.
int run_desktop(int argc, char ** argv) {
    dualmoe::AppOptions opt;
    const int parsed = dualmoe::parse_app_options(argc, argv, opt, /*desktop_defaults=*/true);
    if (parsed == 1) return 0;   // --help / --version already printed
    if (parsed < 0) return 1;

#if defined(DUALMOE_DESKTOP)
    opt.desktop_window = !opt.open_browser;
#endif
    dualmoe::AppServer app;
    std::string err;
    if (!app.start(opt, err)) {
        show_native_error(L"DualMoE — ошибка запуска", widen(err));
        return 1;
    }

#if defined(DUALMOE_DESKTOP)
    if (opt.open_browser) {
        open_in_browser(app.local_url());
        // Headless-in-desktop mode: no window, server only (used for testing).
        app.boot_model_sync();
        std::cout << "DualMoE слушает " << app.local_url() << " (окно отключено через --browser)\n";
        app.run();
        return 0;
    }

    std::unique_ptr<webview::webview> win;
    try {
        win = std::make_unique<webview::webview>(/*debug=*/false, nullptr);
        win->set_title("DualMoE v0.4.1 — Локальная MoE-студия (VRAM ⊕ RAM)");
        win->set_size(1440, 940, WEBVIEW_HINT_NONE);
    } catch (const webview::exception & e) {
        // WebView2 runtime missing (very old Windows 10 without Evergreen bootstrapper).
        const std::string msg =
            std::string("Не удалось создать окно приложения: ") + e.what() +
            "\n\nДля нативного окна нужен компонент Microsoft Edge WebView2 Runtime.\n"
            "Он уже встроен в Windows 11 и в обновлённую Windows 10, но если его нет —\n"
            "скачайте «Evergreen Bootstrapper» с aka.ms/webview2 и запустите установщик.\n\n"
            "Нажмите ОК, чтобы открыть интерфейс в обычном браузере.";
        MessageBoxW(nullptr, widen(msg).c_str(), L"DualMoE — WebView2 недоступен",
                    MB_ICONWARNING | MB_OK);
        open_in_browser(app.local_url());
        app.boot_model_sync();
        app.run();
        return 0;
    }

    // Real title-bar / taskbar icon instead of the generic Win32 default.
    HWND hwnd = nullptr;
    if (auto w = win->window(); w.ok()) hwnd = (HWND) w.value();
    apply_app_icon(hwnd);

    if (opt.boot_cfg.model_path.empty()) {
        std::cout << "Модель не задана в командной строке — выберите .gguf в окне DualMoE.\n";
    }

    std::cout << "\n==================================================================\n"
              << "  DualMoE v0.4.1 — нативное окно (Edge WebView2, без браузера)\n"
              << "  OpenAI / LearnHarness Base URL: " << app.openai_base_url() << "\n"
              << "  Закройте окно приложения, чтобы полностью остановить движок.\n"
              << "==================================================================\n";
    std::cout.flush();

    // Model loading runs in the background: the window shows the 8 live phases.
    if (!app.boot_model_async(err)) {
        std::cerr << "[DualMoE] Предупреждение: " << err << "\n";
    }

    // Serve HTTP/OpenAI traffic on a worker thread while this thread owns the
    // Win32 message pump (WebView2 requires the UI thread for pumping).
    std::thread server_thread([&app]() { app.run(); });

    win->navigate(app.local_url());
    win->run();     // blocks until the window is closed

    // Window closed -> tear everything down so no orphan process keeps the
    // model in RAM or the port busy.
    app.stop();
    if (server_thread.joinable()) server_thread.join();
    return 0;
#else
    // Console server build: identical behaviour to previous releases.
    std::cout << "\n==================================================================\n"
              << "  DualMoE v0.4.1 — консольный сервер + Web UI\n"
              << "  Интерфейс в браузере:           " << app.local_url() << "\n"
              << "  OpenAI / LearnHarness Base URL: " << app.openai_base_url() << "\n"
              << "==================================================================\n";
    std::cout.flush();

    if (opt.open_browser) {
        const std::string url = app.local_url();
        std::thread([url]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            open_in_browser(url);
        }).detach();
    }

    app.boot_model_sync();
    app.run();
    return 0;
#endif
}

} // namespace

// Unicode GUI entry point: the CRT resolves it to wWinMainCRTStartup for
// /SUBSYSTEM:WINDOWS + UNICODE, and the raw command line is already wide.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // Rebuild argv from the wide command line so the console and desktop builds
    // share exactly one option parser.
    int argc = 0;
    LPWSTR * argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args_storage;
    std::vector<char *> argv_ptrs;
    if (argv_w) {
        for (int i = 0; i < argc; ++i) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s((size_t) (n > 0 ? n - 1 : 0), '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, s.data(), n, nullptr, nullptr);
            args_storage.push_back(std::move(s));
        }
        LocalFree(argv_w);
    }
    if (args_storage.empty()) args_storage.emplace_back("DualMoE.exe");
    for (auto & a : args_storage) argv_ptrs.push_back(a.data());
    return run_desktop((int) argv_ptrs.size(), argv_ptrs.data());
}

#else // !_WIN32

#include <cstdlib>

int main(int argc, char ** argv) {
    dualmoe::AppOptions opt;
    const int parsed = dualmoe::parse_app_options(argc, argv, opt, /*desktop_defaults=*/false);
    if (parsed == 1) return 0;
    if (parsed < 0) return 1;

    dualmoe::AppServer app;
    std::string err;
    if (!app.start(opt, err)) {
        std::cerr << "[DualMoE] " << err << "\n";
        return 1;
    }
    std::cout << "\n  DualMoE v0.4.1 — сервер: " << app.local_url()
              << "\n  OpenAI Base URL: " << app.openai_base_url() << "\n";
    if (opt.open_browser) {
        const std::string cmd = "xdg-open '" + app.local_url() + "' >/dev/null 2>&1 &";
        (void) std::system(cmd.c_str());
    }
    app.boot_model_sync();
    app.run();
    return 0;
}

#endif
