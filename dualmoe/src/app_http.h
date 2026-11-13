// DualMoE desktop app core: embedded HTTP/OpenAI server + engine boot logic.
// Shared by the native window front-end (DualMoE.exe) and the headless
// console build (DualMoE-server.exe) so both behave identically.
#pragma once

#include "dualmoe/types.h"

#include <memory>
#include <string>

namespace dualmoe {

struct AppOptions {
    std::string  host             = "127.0.0.1";
    int          port             = 8090;
    bool         port_explicit    = false;
    bool         open_browser     = false;   // console build: open Web UI in browser
    bool         desktop_window   = false;   // true when the UI lives in the native window
    std::string  scan_dir;                   // folder shown by the model browser
    EngineConfig boot_cfg;                   // --model / --ngl / --ctx / ...
};

// Folder next to the executable (models, load log): used as default scan dir.
std::string default_app_dir();

// Parses the shared command line. Returns:
//   0  -> options parsed, continue startup
//   1  -> informational flag handled (--help / --version), caller should exit(0)
//  -1  -> invalid usage, caller should exit(1)
int parse_app_options(int argc, char ** argv, AppOptions & opt, bool desktop_defaults);

// Owns the engine + HTTP server for one process lifetime.
class AppServer {
public:
    AppServer();
    ~AppServer();

    AppServer(const AppServer &) = delete;
    AppServer & operator=(const AppServer &) = delete;

    // Binds the HTTP port (127.0.0.1 by default) and installs all routes.
    bool start(const AppOptions & opt, std::string & err);

    // Loads the startup model synchronously (console build).
    void boot_model_sync();

    // Loads the startup model on a background thread so the native window can
    // appear immediately while the UI streams the 8 load phases.
    bool boot_model_async(std::string & err);

    // Blocks in the accept/serve loop until stop() is called.
    void run();
    void stop();

    int         port() const;
    std::string local_url() const;      // http://127.0.0.1:<port>
    std::string openai_base_url() const; // http://127.0.0.1:<port>/v1
    bool        model_loading() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dualmoe
