#include "engine.h"
#include "embedded_webui.h"

#include "httplib.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string path_to_utf8(const fs::path & p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
}

fs::path utf8_to_path(const std::string & s) {
    return fs::u8path(s);
}

std::string default_exe_dir_utf8() {
#if defined(_WIN32)
    wchar_t buf[2048]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, 2047);
    if (len > 0) {
        const fs::path p(buf);
        if (p.has_parent_path()) return path_to_utf8(p.parent_path());
    }
#endif
    std::error_code ec;
    const auto cwd = fs::current_path(ec);
    return ec ? "." : path_to_utf8(cwd);
}

std::vector<std::string> list_roots_utf8() {
    std::vector<std::string> roots;
#if defined(_WIN32)
    wchar_t drives[512]{};
    const DWORD n = GetLogicalDriveStringsW(511, drives);
    const wchar_t * p = drives;
    while (n > 0 && *p) {
        roots.push_back(path_to_utf8(fs::path(p)));
        p += std::wcslen(p) + 1;
    }
#else
    roots.push_back("/");
    roots.push_back(default_exe_dir_utf8());
#endif
    return roots;
}

json telemetry_to_json(const dualmoe::TelemetrySnapshot & t) {
    return {
        {"model_loaded", t.model_loaded},
        {"loading", t.loading},
        {"generating", t.generating},
        {"model_name", t.model_name},
        {"model_arch", t.model_arch},
        {"status_text", t.status_text},
        {"load_progress_pct", t.load_progress_pct},
        {"load_phase_index", t.load_phase_index},
        {"load_phase_total", t.load_phase_total},
        {"prefill_tps", t.prefill_tps},
        {"decode_tps", t.decode_tps},
        {"ttft_ms", t.ttft_ms},
        {"prompt_tokens", t.prompt_tokens},
        {"generated_tokens", t.generated_tokens},
        {"kv_used_tokens", t.kv_used_tokens},
        {"kv_max_tokens", t.kv_max_tokens},
        {"gpu_expert_hits", t.gpu_expert_hits},
        {"gpu_expert_fills", t.gpu_expert_fills},
        {"cpu_parallel_experts", t.cpu_parallel_experts},
        {"ram_cache_hits", t.ram_cache_hits},
        {"ssd_expert_reads", t.ssd_expert_reads},
        {"ssd_bytes_read", t.ssd_bytes_read},
        {"vram_upload_bytes", t.vram_upload_bytes},
        {"ssd_bandwidth_mbs", t.ssd_bandwidth_mbs},
        {"vram_cache_budget_mb", t.vram_cache_budget_mb},
        {"ram_arena_used_mb", t.ram_arena_used_mb},
        {"ram_arena_budget_mb", t.ram_arena_budget_mb},
        {"dense_anon_mb", t.dense_anon_mb},
        {"os_ram_avail_mb", t.os_ram_avail_mb},
        {"os_commit_avail_mb", t.os_commit_avail_mb},
        {"gpu_vram_free_mb", t.gpu_vram_free_mb},
        {"gpu_vram_total_mb", t.gpu_vram_total_mb},
        {"arena_commit_failures", t.arena_commit_failures},
        {"mmap_released", t.mmap_released},
        {"auto_split_active", t.auto_split_active},
        {"parallel_gpu_cpu_active", t.parallel_gpu_cpu_active},
        {"active_gpu_layers", t.active_gpu_layers},
        {"active_cpu_share", t.active_cpu_share},
        {"active_ubatch", t.active_ubatch},
    };
}

json summary_to_json(const dualmoe::ModelSummary & s) {
    return {
        {"ok", s.ok},
        {"path", s.path},
        {"filename", s.filename},
        {"arch", s.arch},
        {"model_name", s.model_name},
        {"file_size_bytes", s.file_size_bytes},
        {"n_layer", s.n_layer},
        {"n_embd", s.n_embd},
        {"n_expert", s.n_expert},
        {"n_expert_used", s.n_expert_used},
        {"n_ctx_train", s.n_ctx_train},
        {"is_moe", s.is_moe},
        {"expert_bytes", s.expert_bytes},
        {"dense_bytes", s.dense_bytes},
        {"cycle_floor_mb", s.cycle_floor_mb},
    };
}

json diagnostics_to_json(const dualmoe::LoadDiagnosticsSnapshot & d,
                         const std::vector<dualmoe::LoadLogEntry> & logs) {
    json steps_arr = json::array();
    for (const auto & s : d.steps) {
        steps_arr.push_back({
            {"index", s.index},
            {"id", s.id},
            {"title", s.title},
            {"detail", s.detail},
            {"risk_hint", s.risk_hint},
            {"state", s.state},
            {"elapsed_ms", s.elapsed_ms},
        });
    }

    json logs_arr = json::array();
    uint64_t max_seq = 0;
    for (const auto & e : logs) {
        if (e.seq > max_seq) max_seq = e.seq;
        logs_arr.push_back({
            {"seq", e.seq},
            {"timestamp", e.timestamp},
            {"level", e.level},
            {"text", e.text},
        });
    }

    return {
        {"ok", true},
        {"loading", d.loading},
        {"phase_index", d.phase_index},
        {"phase_total", d.phase_total},
        {"progress_pct", d.progress_pct},
        {"current_stage", d.current_stage},
        {"last_error", d.last_error},
        {"log_file_path", d.log_file_path},
        {"requested_ngl", d.requested_ngl},
        {"effective_ngl", d.effective_ngl},
        {"requested_vram_cache_mb", d.requested_vram_cache_mb},
        {"effective_vram_cache_mb", d.effective_vram_cache_mb},
        {"requested_ram_cache_mb", d.requested_ram_cache_mb},
        {"effective_ram_cache_mb", d.effective_ram_cache_mb},
        {"requested_ctx", d.requested_ctx},
        {"effective_ctx", d.effective_ctx},
        {"effective_ubatch", d.effective_ubatch},
        {"warnings", d.warnings},
        {"steps", steps_arr},
        {"sys_mem", {
            {"ram_total_mb", d.sys_mem.ram_total_mb},
            {"ram_avail_mb", d.sys_mem.ram_avail_mb},
            {"commit_total_mb", d.sys_mem.commit_total_mb},
            {"commit_avail_mb", d.sys_mem.commit_avail_mb},
            {"vram_total_mb", d.sys_mem.vram_total_mb},
            {"vram_free_mb", d.sys_mem.vram_free_mb},
            {"vram_max_buffer_mb", d.sys_mem.vram_max_buffer_mb},
            {"gpu_name", d.sys_mem.gpu_name},
            {"gpu_backend", d.sys_mem.gpu_backend},
        }},
        {"prev_crash", {
            {"has_previous_crash", d.prev_crash.has_previous_crash},
            {"crash_time", d.prev_crash.crash_time},
            {"crash_stage", d.prev_crash.crash_stage},
            {"crash_reason", d.prev_crash.crash_reason},
            {"crash_advice", d.prev_crash.crash_advice},
            {"log_file_path", d.prev_crash.log_file_path},
            {"last_log_tail", d.prev_crash.last_log_tail},
        }},
        {"logs", logs_arr},
        {"latest_seq", max_seq},
    };
}

std::string extract_message_text(const json & content_node) {
    if (content_node.is_string()) {
        return content_node.get<std::string>();
    }
    if (content_node.is_array()) {
        std::string out;
        for (const auto & part : content_node) {
            if (part.is_string()) {
                out += part.get<std::string>();
            } else if (part.is_object() && part.contains("text") && part["text"].is_string()) {
                out += part["text"].get<std::string>();
            }
        }
        return out;
    }
    return "";
}

void add_cors(httplib::Response & res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

} // namespace

int main(int argc, char ** argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    std::string host          = "0.0.0.0";
    int         port          = 8090;
    bool        port_explicit = false;
    bool        no_browser    = (std::getenv("DUALMOE_NO_BROWSER") != nullptr);
    std::string scan_dir      = default_exe_dir_utf8();
    dualmoe::EngineConfig boot_cfg;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_val = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : "";
        };
        if (arg == "--host") host = next_val();
        else if (arg == "--port" || arg == "-p") { port = std::atoi(next_val().c_str()); port_explicit = true; }
        else if (arg == "--no-browser") no_browser = true;
        else if (arg == "--model" || arg == "-m") boot_cfg.model_path = next_val();
        else if (arg == "--models-dir") scan_dir = next_val();
        else if (arg == "--ngl" || arg == "--gpu-layers") boot_cfg.n_gpu_layers = std::atoi(next_val().c_str());
        else if (arg == "--vram-cache-mb") boot_cfg.vram_cache_mb = (size_t) std::atoll(next_val().c_str());
        else if (arg == "--ram-cache-mb") boot_cfg.ram_cache_mb = (size_t) std::atoll(next_val().c_str());
        else if (arg == "--cpu-share") boot_cfg.cpu_share = std::strtof(next_val().c_str(), nullptr);
        else if (arg == "--ctx" || arg == "-c") boot_cfg.n_ctx = (uint32_t) std::atoi(next_val().c_str());
        else if (arg == "--threads" || arg == "-t") boot_cfg.n_threads = std::atoi(next_val().c_str());
        else if (arg == "--help" || arg == "-h") {
            std::cout << "DualMoE v0.2.0 (LearnHarness x Hermes Edition — GPU || CPU || SSD MoE Runtime)\n"
                      << "  --host <addr>           Bind address (default 0.0.0.0)\n"
                      << "  --port <port>           HTTP/OpenAI port (default 8090)\n"
                      << "  --no-browser            Do not auto-open Web UI on Windows startup\n"
                      << "  --model <file.gguf>     Auto-load GGUF model at startup\n"
                      << "  --models-dir <folder>   Default folder for GGUF UI scanner\n"
                      << "  --ngl <N>               GPU layers for Attention/Router (default 99, auto-step-down)\n"
                      << "  --vram-cache-mb <MiB>   Hot expert bank in GPU VRAM (default 2400, auto-clamped)\n"
                      << "  --cpu-share <0..1>      Fraction of misses computed on CPU in parallel (default 0.92)\n"
                      << "  --ram-cache-mb <MiB>    DDR4 virtual arena budget for experts (default 5632)\n"
                      << "  --ctx <tokens>          Context window (default 24576)\n"
                      << "  --threads <N>           CPU decode threads (default 8)\n";
            return 0;
        }
    }

    dualmoe::DualMoeEngine engine;

    if (!boot_cfg.model_path.empty()) {
        const auto parent_p = utf8_to_path(boot_cfg.model_path).parent_path();
        if (!parent_p.empty()) scan_dir = path_to_utf8(parent_p);
        std::string err;
        std::cout << "[DualMoE] Автозагрузка стартовой модели: " << boot_cfg.model_path << " ...\n";
        if (!engine.load_model(boot_cfg, err)) {
            std::cerr << "[DualMoE] Предупреждение: не удалось загрузить стартовую модель: " << err << "\n";
        } else {
            std::cout << "[DualMoE] Модель готова: " << engine.current_model().model_name << "\n";
        }
    }

    httplib::Server svr;

    svr.Options(R"(.*)", [](const httplib::Request &, httplib::Response & res) {
        add_cors(res);
        res.status = 204;
    });

    svr.Get("/", [](const httplib::Request &, httplib::Response & res) {
        add_cors(res);
        res.set_content(reinterpret_cast<const char *>(dualmoe::kEmbeddedIndexHtml),
                        dualmoe::kEmbeddedIndexHtmlSize,
                        "text/html; charset=utf-8");
    });

    svr.Get("/api/status", [&](const httplib::Request &, httplib::Response & res) {
        add_cors(res);
        json devs = json::array();
        for (const auto & d : engine.list_devices()) {
            devs.push_back({
                {"name", d.name},
                {"description", d.description},
                {"backend", d.backend},
                {"is_gpu", d.is_gpu},
                {"free_mb", d.free_mb},
                {"total_mb", d.total_mb},
                {"max_buffer_mb", d.max_buffer_mb},
            });
        }
        const auto cfg = engine.current_config();
        json out = {
            {"ok", true},
            {"version", "0.2.0"},
            {"default_scan_dir", scan_dir},
            {"roots", list_roots_utf8()},
            {"devices", devs},
            {"config", {
                {"model_path", cfg.model_path},
                {"n_gpu_layers", cfg.n_gpu_layers},
                {"vram_cache_mb", cfg.vram_cache_mb},
                {"cpu_share", cfg.cpu_share},
                {"ram_cache_mb", cfg.ram_cache_mb},
                {"n_ctx", cfg.n_ctx},
                {"n_threads", cfg.n_threads},
                {"n_ubatch", cfg.n_ubatch},
                {"kv_type_k", cfg.kv_type_k},
                {"kv_type_v", cfg.kv_type_v},
            }},
            {"model", summary_to_json(engine.current_model())},
            {"telemetry", telemetry_to_json(engine.telemetry())},
        };
        res.set_content(out.dump(), "application/json; charset=utf-8");
    });

    svr.Get("/api/diagnostics", [&](const httplib::Request & req, httplib::Response & res) {
        add_cors(res);
        uint64_t since_seq = 0;
        if (req.has_param("since_seq")) {
            since_seq = (uint64_t) std::strtoull(req.get_param_value("since_seq").c_str(), nullptr, 10);
        }
        const auto diag = engine.diagnostics();
        const auto logs = engine.recent_logs(since_seq);
        res.set_content(diagnostics_to_json(diag, logs).dump(), "application/json; charset=utf-8");
    });

    svr.Post("/api/diagnostics/dismiss", [&](const httplib::Request &, httplib::Response & res) {
        add_cors(res);
        engine.clear_crash_banner();
        res.set_content(json{{"ok", true}}.dump(), "application/json; charset=utf-8");
    });

    svr.Get("/api/fs/browse", [&](const httplib::Request & req, httplib::Response & res) {
        add_cors(res);
        const std::string raw_dir = (req.has_param("dir") && !req.get_param_value("dir").empty())
            ? req.get_param_value("dir")
            : scan_dir;

        std::error_code ec;
        fs::path target = utf8_to_path(raw_dir);
        if (!fs::exists(target, ec) || !fs::is_directory(target, ec)) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", "Папка не найдена: " + raw_dir}, {"roots", list_roots_utf8()}}.dump(),
                            "application/json; charset=utf-8");
            return;
        }
        const fs::path canon = fs::weakly_canonical(target, ec);
        const fs::path cur   = ec ? target : canon;
        const std::string cur_utf8 = path_to_utf8(cur);
        const std::string parent_utf8 = (cur.has_parent_path() && cur.parent_path() != cur)
            ? path_to_utf8(cur.parent_path())
            : "";

        json dirs = json::array();
        json files = json::array();
        for (fs::directory_iterator it(cur, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code iec;
            const std::string fname = path_to_utf8(it->path().filename());
            if (fname.empty()) continue;
            if (it->is_directory(iec)) {
                dirs.push_back({
                    {"name", fname},
                    {"path", path_to_utf8(it->path())},
                });
            } else if (it->is_regular_file(iec)) {
                const std::string ext = path_to_utf8(it->path().extension());
                if (ext == ".gguf" || ext == ".GGUF") {
                    const uint64_t fsz = it->file_size(iec);
                    files.push_back({
                        {"name", fname},
                        {"path", path_to_utf8(it->path())},
                        {"size_bytes", iec ? 0ull : fsz},
                    });
                }
            }
        }

        std::sort(dirs.begin(), dirs.end(), [](const json & a, const json & b) {
            return a.value("name", "") < b.value("name", "");
        });
        std::sort(files.begin(), files.end(), [](const json & a, const json & b) {
            return a.value("name", "") < b.value("name", "");
        });

        res.set_content(json{
            {"ok", true},
            {"current_dir", cur_utf8},
            {"parent_dir", parent_utf8},
            {"roots", list_roots_utf8()},
            {"dirs", dirs},
            {"files", files},
        }.dump(), "application/json; charset=utf-8");
    });

    svr.Get("/api/models/scan", [&](const httplib::Request & req, httplib::Response & res) {
        add_cors(res);
        const std::string dir = req.has_param("dir") && !req.get_param_value("dir").empty()
            ? req.get_param_value("dir")
            : scan_dir;
        json arr = json::array();
        for (const auto & m : dualmoe::scan_gguf_directory(dir)) {
            arr.push_back(summary_to_json(m));
        }
        res.set_content(json{{"ok", true}, {"dir", dir}, {"models", arr}}.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/api/model/load", [&](const httplib::Request & req, httplib::Response & res) {
        add_cors(res);
        try {
            const json body = json::parse(req.body);
            dualmoe::EngineConfig cfg = boot_cfg;
            cfg.model_path         = body.value("model_path", "");
            cfg.n_gpu_layers       = body.value("n_gpu_layers", cfg.n_gpu_layers);
            cfg.vram_cache_mb      = body.value("vram_cache_mb", cfg.vram_cache_mb);
            cfg.cpu_share          = body.value("cpu_share", cfg.cpu_share);
            cfg.ram_cache_mb       = body.value("ram_cache_mb", cfg.ram_cache_mb);
            cfg.n_ctx              = body.value("n_ctx", cfg.n_ctx);
            cfg.n_threads          = body.value("n_threads", cfg.n_threads);
            cfg.n_ubatch           = body.value("n_ubatch", cfg.n_ubatch);
            cfg.kv_type_k          = body.value("kv_type_k", cfg.kv_type_k);
            cfg.kv_type_v          = body.value("kv_type_v", cfg.kv_type_v);
            cfg.auto_split_experts = body.value("auto_split_experts", true);
            cfg.moe_stream         = body.value("moe_stream", true);
            cfg.dense_anon         = body.value("dense_anon", cfg.dense_anon);
            cfg.release_mmap       = body.value("release_mmap", cfg.release_mmap);

            const bool async_mode = body.value("async", false);
            std::string err;
            if (async_mode) {
                if (!engine.start_load_async(cfg, err)) {
                    res.status = 400;
                    res.set_content(json{{"ok", false}, {"error", err}}.dump(), "application/json; charset=utf-8");
                    return;
                }
                res.set_content(json{
                    {"ok", true},
                    {"async", true},
                    {"diagnostics", diagnostics_to_json(engine.diagnostics(), engine.recent_logs(0))},
                }.dump(), "application/json; charset=utf-8");
                return;
            }

            if (!engine.load_model(cfg, err)) {
                res.status = 400;
                res.set_content(json{
                    {"ok", false},
                    {"error", err},
                    {"diagnostics", diagnostics_to_json(engine.diagnostics(), engine.recent_logs(0))},
                }.dump(), "application/json; charset=utf-8");
                return;
            }
            res.set_content(json{
                {"ok", true},
                {"model", summary_to_json(engine.current_model())},
                {"telemetry", telemetry_to_json(engine.telemetry())},
                {"diagnostics", diagnostics_to_json(engine.diagnostics(), engine.recent_logs(0))},
            }.dump(), "application/json; charset=utf-8");
        } catch (const std::exception & e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json; charset=utf-8");
        }
    });

    svr.Post("/api/model/unload", [&](const httplib::Request &, httplib::Response & res) {
        add_cors(res);
        engine.unload_model();
        res.set_content(json{{"ok", true}}.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/api/cancel", [&](const httplib::Request &, httplib::Response & res) {
        add_cors(res);
        engine.request_cancel();
        res.set_content(json{{"ok", true}}.dump(), "application/json; charset=utf-8");
    });

    svr.Get("/api/telemetry/stream", [&](const httplib::Request &, httplib::Response & res) {
        add_cors(res);
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_chunked_content_provider(
            "text/event-stream",
            [&](size_t, httplib::DataSink & sink) {
                const auto snap = engine.telemetry();
                const std::string line = "data: " + telemetry_to_json(snap).dump() + "\n\n";
                if (!sink.write(line.data(), line.size())) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(snap.loading ? 220 : 420));
                return true;
            });
    });

    // OpenAI-compatible endpoints (LearnHarness, Hermes Agent, Cursor, Continue)
    svr.Get("/v1/models", [&](const httplib::Request &, httplib::Response & res) {
        add_cors(res);
        const auto m = engine.current_model();
        json data = json::array();
        data.push_back({
            {"id", "dualmoe-default"},
            {"object", "model"},
            {"created", (int64_t) std::time(nullptr)},
            {"owned_by", "dualmoe"},
        });
        if (m.ok && !m.filename.empty()) {
            data.push_back({
                {"id", m.filename},
                {"object", "model"},
                {"created", (int64_t) std::time(nullptr)},
                {"owned_by", "dualmoe"},
            });
        }
        res.set_content(json{{"object", "list"}, {"data", data}}.dump(), "application/json; charset=utf-8");
    });

    svr.Post("/v1/chat/completions", [&](const httplib::Request & req, httplib::Response & res) {
        add_cors(res);
        if (engine.is_loading()) {
            res.status = 503;
            res.set_content(json{{"error", {{"message", "Модель сейчас проходит этапы загрузки (см. панель диагностики)"}}}}.dump(),
                            "application/json; charset=utf-8");
            return;
        }
        if (!engine.is_loaded()) {
            res.status = 503;
            res.set_content(json{{"error", {{"message", "Сначала загрузите .gguf модель в интерфейсе DualMoE"}}}}.dump(),
                            "application/json; charset=utf-8");
            return;
        }

        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception & e) {
            res.status = 400;
            res.set_content(json{{"error", {{"message", std::string("Некорректный JSON: ") + e.what()}}}}.dump(),
                            "application/json; charset=utf-8");
            return;
        }

        std::vector<dualmoe::ChatMessage> messages;
        if (body.contains("messages") && body["messages"].is_array()) {
            for (const auto & item : body["messages"]) {
                if (!item.is_object()) continue;
                dualmoe::ChatMessage cm;
                cm.role    = item.value("role", "user");
                cm.content = item.contains("content") ? extract_message_text(item["content"]) : "";
                messages.push_back(std::move(cm));
            }
        }
        if (messages.empty()) {
            res.status = 400;
            res.set_content(json{{"error", {{"message", "Поле messages пусто"}}}}.dump(),
                            "application/json; charset=utf-8");
            return;
        }

        dualmoe::SamplingParams sp = engine.current_config().sampling;
        if (body.contains("temperature") && body["temperature"].is_number()) {
            sp.temperature = body["temperature"].get<float>();
        }
        if (body.contains("top_p") && body["top_p"].is_number()) {
            sp.top_p = body["top_p"].get<float>();
        }
        if (body.contains("max_tokens") && body["max_tokens"].is_number_integer()) {
            sp.max_tokens = body["max_tokens"].get<int32_t>();
        } else if (body.contains("max_completion_tokens") && body["max_completion_tokens"].is_number_integer()) {
            sp.max_tokens = body["max_completion_tokens"].get<int32_t>();
        }

        const bool stream = body.value("stream", false);
        const std::string model_id = body.value("model", "dualmoe-default");
        const int64_t created_ts   = (int64_t) std::time(nullptr);

        if (!stream) {
            std::string reply, err;
            int32_t p_tok = 0, c_tok = 0;
            if (!engine.generate_chat(messages, sp, true, nullptr, reply, p_tok, c_tok, err)) {
                res.status = 500;
                res.set_content(json{{"error", {{"message", err}}}}.dump(), "application/json; charset=utf-8");
                return;
            }
            json out = {
                {"id", "chatcmpl-dualmoe"},
                {"object", "chat.completion"},
                {"created", created_ts},
                {"model", model_id},
                {"choices", json::array({{
                    {"index", 0},
                    {"message", {{"role", "assistant"}, {"content", reply}}},
                    {"finish_reason", "stop"},
                }})},
                {"usage", {
                    {"prompt_tokens", p_tok},
                    {"completion_tokens", c_tok},
                    {"total_tokens", p_tok + c_tok},
                }},
            };
            res.set_content(out.dump(), "application/json; charset=utf-8");
            return;
        }

        res.set_header("Cache-Control", "no-cache");
        res.set_chunked_content_provider(
            "text/event-stream",
            [messages = std::move(messages), sp, model_id, created_ts, &engine](size_t, httplib::DataSink & sink) {
                std::string reply, err;
                int32_t p_tok = 0, c_tok = 0;

                const json head_ev = {
                    {"id", "chatcmpl-dualmoe"},
                    {"object", "chat.completion.chunk"},
                    {"created", created_ts},
                    {"model", model_id},
                    {"choices", json::array({{
                        {"index", 0},
                        {"delta", {{"role", "assistant"}}},
                        {"finish_reason", nullptr},
                    }})},
                };
                const std::string head_str = "data: " + head_ev.dump() + "\n\n";
                if (!sink.write(head_str.data(), head_str.size())) {
                    sink.done();
                    return false;
                }

                const bool ok = engine.generate_chat(
                    messages, sp, true,
                    [&](const std::string & piece) -> bool {
                        const json chunk_ev = {
                            {"id", "chatcmpl-dualmoe"},
                            {"object", "chat.completion.chunk"},
                            {"created", created_ts},
                            {"model", model_id},
                            {"choices", json::array({{
                                {"index", 0},
                                {"delta", {{"content", piece}}},
                                {"finish_reason", nullptr},
                            }})},
                        };
                        const std::string line = "data: " + chunk_ev.dump() + "\n\n";
                        return sink.write(line.data(), line.size());
                    },
                    reply, p_tok, c_tok, err);

                const json stop_ev = {
                    {"id", "chatcmpl-dualmoe"},
                    {"object", "chat.completion.chunk"},
                    {"created", created_ts},
                    {"model", model_id},
                    {"choices", json::array({{
                        {"index", 0},
                        {"delta", ok ? json::object() : json{{"content", "\n[Ошибка: " + err + "]"}}},
                        {"finish_reason", "stop"},
                    }})},
                };
                const std::string tail = "data: " + stop_ev.dump() + "\n\ndata: [DONE]\n\n";
                sink.write(tail.data(), tail.size());
                sink.done();
                return false;
            });
    });

    int bound_port = -1;
    const int max_tries = port_explicit ? 1 : 9;
    for (int t = 0; t < max_tries; ++t) {
        if (svr.bind_to_port(host, port + t)) {
            bound_port = port + t;
            break;
        }
    }
    if (bound_port < 0) {
        std::cerr << "[DualMoE] Ошибка привязки порта " << host << ":" << port << "\n";
        return 1;
    }

    const std::string local_url = "http://" + std::string(host == "0.0.0.0" ? "127.0.0.1" : host) + ":" + std::to_string(bound_port);
    std::cout << "\n==================================================================\n"
              << "  DualMoE v0.2.0 — LearnHarness x Hermes Agent Runtime\n"
              << "  Интерфейс в браузере:           " << local_url << "\n"
              << "  OpenAI / LearnHarness Base URL: " << local_url << "/v1\n"
              << "  Журнал загрузки (Flight Log):   dualmoe-load.log\n"
              << "==================================================================\n";
    std::cout.flush();

#if defined(_WIN32)
    if (!no_browser) {
        std::thread([local_url]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            const std::wstring wurl(local_url.begin(), local_url.end());
            ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }).detach();
    }
#else
    (void) no_browser;
#endif

    svr.listen_after_bind();
    return 0;
}
