#include "app_http.h"
#include "win_dialogs.h"
#include "bench.h"

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
#include <cstring>
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
        {"spec_tokens", t.spec_tokens},
        {"spec_drafted", t.spec_drafted},
        {"spec_accepted", t.spec_accepted},
        {"spec_efficiency", t.spec_efficiency},
        {"par_layers_dual", t.par_layers_dual},
        {"par_layers_serial", t.par_layers_serial},
        {"par_gpu_ms", t.par_gpu_ms},
        {"par_cpu_ms", t.par_cpu_ms},
        {"par_overlap_pct", t.par_overlap_pct},
        {"par_gpu_nodes", t.par_gpu_nodes},
        {"par_cpu_nodes", t.par_cpu_nodes},
        {"prompt_tokens", t.prompt_tokens},
        {"generated_tokens", t.generated_tokens},
        {"kv_used_tokens", t.kv_used_tokens},
        {"kv_max_tokens", t.kv_max_tokens},
        {"gpu_expert_hits", t.gpu_expert_hits},
        {"gpu_expert_fills", t.gpu_expert_fills},
        {"cpu_parallel_experts", t.cpu_parallel_experts},
        {"ram_cache_hits", t.ram_cache_hits},
        {"ram_to_vram_swaps", t.ram_to_vram_swaps},
        {"vram_to_ram_swaps", t.vram_to_ram_swaps},
        {"ssd_expert_reads", t.ssd_expert_reads},
        {"ssd_bytes_read", t.ssd_bytes_read},
        {"vram_upload_bytes", t.vram_upload_bytes},
        {"ssd_bandwidth_mbs", t.ssd_bandwidth_mbs},
        {"preload_bandwidth_mbs", t.preload_bandwidth_mbs},
        {"vram_cache_budget_mb", t.vram_cache_budget_mb},
        {"vram_prewarmed_mb", t.vram_prewarmed_mb},
        {"ram_arena_used_mb", t.ram_arena_used_mb},
        {"ram_arena_budget_mb", t.ram_arena_budget_mb},
        {"ram_preloaded_mb", t.ram_preloaded_mb},
        {"dense_anon_mb", t.dense_anon_mb},
        {"os_ram_avail_mb", t.os_ram_avail_mb},
        {"os_commit_avail_mb", t.os_commit_avail_mb},
        {"gpu_vram_free_mb", t.gpu_vram_free_mb},
        {"gpu_vram_total_mb", t.gpu_vram_total_mb},
        {"arena_commit_failures", t.arena_commit_failures},
        {"silicon_residency_pct", t.silicon_residency_pct},
        {"mmap_released", t.mmap_released},
        {"auto_split_active", t.auto_split_active},
        {"parallel_gpu_cpu_active", t.parallel_gpu_cpu_active},
        {"zero_hdd_active", t.zero_hdd_active},
        {"disjoint_swap_active", t.disjoint_swap_active},
        {"auto_balance_active", t.auto_balance_active},
        {"active_gpu_layers", t.active_gpu_layers},
        {"active_cpu_share", t.active_cpu_share},
        {"active_seam_share", t.active_seam_share},
        {"active_ubatch", t.active_ubatch},
        {"thinking_enabled", t.thinking_enabled},
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
        {"n_swa", s.n_swa},
        {"n_kv_shared_layers", s.n_kv_shared_layers},
        {"n_swa_layers", s.n_swa_layers},
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

json bench_candidate_to_json(const dualmoe::BenchCandidate & c) {
    json knobs = json::array();
    for (const auto & k : c.knobs) knobs.push_back(k);
    return {
        {"index", c.index},
        {"label", c.label},
        {"knobs", knobs},
        {"ok", c.ok},
        {"measured", c.measured},
        {"in_place", c.in_place},
        {"error", c.error},
        {"prefill_tps", c.prefill_tps},
        {"decode_tps", c.decode_tps},
        {"ttft_ms", c.ttft_ms},
        {"vram_hit_pct", c.vram_hit_pct},
        {"hdd_reads", c.hdd_reads},
        {"ram_used_mb", c.ram_used_mb},
        {"vram_used_mb", c.vram_used_mb},
        {"score", c.score},
        {"elapsed_s", c.elapsed_s},
        {"best", c.best},
        {"config", {
            {"n_threads", c.cfg.n_threads},
            {"n_threads_batch", c.cfg.n_threads_batch},
            {"cpu_share", c.cfg.cpu_share},
            {"parallel_seam_share", c.cfg.parallel_seam_share},
            {"vram_cache_mb", c.cfg.vram_cache_mb},
            {"ram_cache_mb", c.cfg.ram_cache_mb},
            {"n_ubatch", c.cfg.n_ubatch},
            {"n_batch", c.cfg.n_batch},
            {"n_ctx", c.cfg.n_ctx},
            {"n_gpu_layers", c.cfg.n_gpu_layers},
            {"kv_type_k", c.cfg.kv_type_k},
            {"kv_type_v", c.cfg.kv_type_v},
            {"swap_hysteresis", c.cfg.swap_hysteresis},
            {"static_bank_share", c.cfg.static_bank_share},
            {"static_bank_mb", c.cfg.static_bank_mb},
            {"warm_state", c.cfg.warm_state},
            {"auto_balance_compute", c.cfg.auto_balance_compute},
            {"zero_hdd_mode", c.cfg.zero_hdd_mode},
            {"enable_thinking", c.cfg.enable_thinking},
        }},
    };
}

json bench_to_json(const dualmoe::BenchmarkSnapshot & b) {
    json cands = json::array();
    for (const auto & c : b.candidates) cands.push_back(bench_candidate_to_json(c));

    json logs = json::array();
    for (const auto & l : b.log) logs.push_back(l);

    json reasons = json::array();
    for (const auto & r : b.recommendation.reasons) reasons.push_back(r);

    json rec;
    if (b.recommendation.ok) {
        const auto & c = b.recommendation.cfg;
        rec = {
            {"ok", true},
            {"config", {
                {"n_threads", c.n_threads},
                {"n_threads_batch", c.n_threads_batch},
                {"cpu_share", c.cpu_share},
                {"vram_cache_mb", c.vram_cache_mb},
                {"ram_cache_mb", c.ram_cache_mb},
                {"n_ubatch", c.n_ubatch},
                {"n_batch", c.n_batch},
                {"n_ctx", c.n_ctx},
                {"n_gpu_layers", c.n_gpu_layers},
                {"kv_type_k", c.kv_type_k},
                {"kv_type_v", c.kv_type_v},
                {"swap_hysteresis", c.swap_hysteresis},
                {"auto_balance_compute", c.auto_balance_compute},
                {"zero_hdd_mode", c.zero_hdd_mode},
                {"enable_thinking", c.enable_thinking},
            }},
            {"reasons", reasons},
            {"predicted_decode_tps", b.recommendation.predicted_decode_tps},
            {"predicted_vram_mb", b.recommendation.predicted_vram_mb},
            {"predicted_ram_mb", b.recommendation.predicted_ram_mb},
            {"predicted_kv_mb", b.recommendation.predicted_kv_mb},
        };
    } else {
        rec = {{"ok", false}};
    }

    return {
        {"ok", true},
        {"stage", b.stage},
        {"mode", b.mode},
        {"progress_pct", b.progress_pct},
        {"message", b.message},
        {"error", b.error},
        {"candidates_total", b.candidates_total},
        {"candidates_done", b.candidates_done},
        {"current_index", b.current_index},
        {"current_label", b.current_label},
        {"model_path", b.model_path},
        {"model_name", b.model_name},
        {"elapsed_s", b.elapsed_s},
        {"best_index", b.best_index},
        {"running", b.stage == "planning" || b.stage == "running"},
        {"candidates", cands},
        {"log", logs},
        {"recommendation", rec},
    };
}

void add_cors(httplib::Response & res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

} // namespace

namespace dualmoe {

std::string default_app_dir() {
    return default_exe_dir_utf8();
}

int parse_app_options(int argc, char ** argv, AppOptions & opt, bool desktop_defaults) {
    // The desktop build listens on loopback only (the native window is the UI),
    // while the headless build may expose the OpenAI API to the LAN.
    opt.host = desktop_defaults ? "127.0.0.1" : "0.0.0.0";
    opt.scan_dir = default_exe_dir_utf8();
    if (std::getenv("DUALMOE_NO_BROWSER") != nullptr) opt.open_browser = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_val = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : "";
        };
        if (arg == "--host") opt.host = next_val();
        else if (arg == "--port" || arg == "-p") { opt.port = std::atoi(next_val().c_str()); opt.port_explicit = true; }
        else if (arg == "--browser" || arg == "--open-browser") opt.open_browser = true;
        else if (arg == "--no-browser") opt.open_browser = false;
        else if (arg == "--model" || arg == "-m") opt.boot_cfg.model_path = next_val();
        else if (arg == "--models-dir") opt.scan_dir = next_val();
        else if (arg == "--ngl" || arg == "--gpu-layers") opt.boot_cfg.n_gpu_layers = std::atoi(next_val().c_str());
        else if (arg == "--vram-cache-mb") opt.boot_cfg.vram_cache_mb = (size_t) std::atoll(next_val().c_str());
        else if (arg == "--ram-cache-mb") opt.boot_cfg.ram_cache_mb = (size_t) std::atoll(next_val().c_str());
        else if (arg == "--cpu-share") opt.boot_cfg.cpu_share = std::strtof(next_val().c_str(), nullptr);
        else if (arg == "--zero-hdd") opt.boot_cfg.zero_hdd_mode = true;
        else if (arg == "--no-zero-hdd") opt.boot_cfg.zero_hdd_mode = false;
        else if (arg == "--auto-balance") opt.boot_cfg.auto_balance_compute = true;
        else if (arg == "--no-auto-balance") opt.boot_cfg.auto_balance_compute = false;
        else if (arg == "--swap-hysteresis") opt.boot_cfg.swap_hysteresis = std::strtof(next_val().c_str(), nullptr);
        else if (arg == "--io-lanes") opt.boot_cfg.io_lanes = std::atoi(next_val().c_str());
        else if (arg == "--ctx" || arg == "-c") opt.boot_cfg.n_ctx = (uint32_t) std::atoi(next_val().c_str());
        else if (arg == "--threads" || arg == "-t") opt.boot_cfg.n_threads = std::atoi(next_val().c_str());
        else if (arg == "--thinking") opt.boot_cfg.enable_thinking = true;
        else if (arg == "--no-thinking") opt.boot_cfg.enable_thinking = false;
        else if (arg == "--version") {
            std::cout << "DualMoE v0.6.1\n";
            return 1;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "DualMoE v0.6.1 — нативное приложение (окно Windows, без браузера)\n"
                      << "  --model <file.gguf>       Auto-load GGUF model at startup\n"
                      << "  --models-dir <folder>     Default folder for GGUF UI scanner\n"
                      << "  --host <addr>             Bind address (desktop default 127.0.0.1)\n"
                      << "  --port <port>             HTTP/OpenAI port (default 8090)\n"
                      << "  --browser / --no-browser  Headless build: open Web UI in the default browser\n"
                      << "  --ngl <N>                 GPU layers for Attention/Router (default 99, auto-step-down)\n"
                      << "  --vram-cache-mb <MiB>     Hot expert bank in GPU VRAM (default 4600, auto-clamped)\n"
                      << "  --cpu-share <0..1>        Initial CPU miss share (default 0.85, auto-balanced)\n"
                      << "  --ram-cache-mb <MiB>      DDR4 virtual arena budget for experts (default 9728)\n"
                      << "  --zero-hdd / --no-zero-hdd Sequential 1-pass HDD preload into VRAM+RAM (default ON)\n"
                      << "  --auto-balance            Live GPU || CPU load balancer + CRF swap hysteresis (default ON)\n"
                      << "  --ctx <tokens>            Context window (default 16384)\n"
                      << "  --threads <N>             CPU decode threads (default 8)\n";
            return 1;
        }
    }
    return 0;
}

struct AppServer::Impl {
    dualmoe::DualMoeEngine  engine;
    dualmoe::BenchmarkRunner bench{engine};
    httplib::Server       svr;
    AppOptions            opt;
    std::atomic<int>      bound_port{-1};
    std::atomic<bool>     shutting_down{false};

    bool start(const AppOptions & o, std::string & err);

    std::string openai_base_url_for_json() const {
        const std::string & h = opt.host;
        return "http://" + std::string(h == "0.0.0.0" || h.empty() ? "127.0.0.1" : h) + ":" +
               std::to_string(bound_port.load(std::memory_order_acquire)) + "/v1";
    }
};

bool AppServer::Impl::start(const AppOptions & o, std::string & err) {
    opt = o;

    if (!opt.boot_cfg.model_path.empty()) {
        const auto parent_p = utf8_to_path(opt.boot_cfg.model_path).parent_path();
        if (!parent_p.empty()) opt.scan_dir = path_to_utf8(parent_p);
    }

    httplib::Server & svr = this->svr;

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
            {"version", "0.6.1"},
            {"default_scan_dir", opt.scan_dir},
            {"roots", list_roots_utf8()},
            {"devices", devs},
            {"config", {
                {"model_path", cfg.model_path},
                {"n_gpu_layers", cfg.n_gpu_layers},
                {"vram_cache_mb", cfg.vram_cache_mb},
                {"cpu_share", cfg.cpu_share},
                {"parallel_seam_share", cfg.parallel_seam_share},
                {"ram_cache_mb", cfg.ram_cache_mb},
                {"zero_hdd_mode", cfg.zero_hdd_mode},
                {"auto_balance_compute", cfg.auto_balance_compute},
                {"swap_hysteresis", cfg.swap_hysteresis},
                {"io_lanes", cfg.io_lanes},
                {"static_bank_share", cfg.static_bank_share},
                {"static_bank_mb", cfg.static_bank_mb},
                {"warm_state", cfg.warm_state},
                {"n_ctx", cfg.n_ctx},
                {"n_threads", cfg.n_threads},
                {"n_ubatch", cfg.n_ubatch},
                {"kv_type_k", cfg.kv_type_k},
                {"kv_type_v", cfg.kv_type_v},
                {"enable_thinking", cfg.enable_thinking},
            }},
            {"model", summary_to_json(engine.current_model())},
            {"telemetry", telemetry_to_json(engine.telemetry())},
        };
        res.set_content(out.dump(), "application/json; charset=utf-8");
    });

    // Optional native Windows dialogs for the desktop shell: a real "Open file"
    // picker instead of browser-only dialogs (the WebView2 window has none).
    svr.Post("/api/app/pick-file", [&](const httplib::Request & req, httplib::Response & res) {
        add_cors(res);
        std::string start_dir = opt.scan_dir;
        try {
            if (!req.body.empty()) {
                const json body = json::parse(req.body);
                start_dir = body.value("dir", start_dir);
            }
        } catch (...) {}

        std::string picked;
        bool canceled = true;
        const bool ok = dualmoe::pick_gguf_file(start_dir, picked, canceled);
        if (!ok) {
            res.set_content(json{{"ok", false}, {"canceled", canceled},
                                 {"error", canceled ? "" : "Нативный выбор файла доступен только в Windows-приложении"}}.dump(),
                            "application/json; charset=utf-8");
            return;
        }
        res.set_content(json{{"ok", true}, {"path", picked}}.dump(), "application/json; charset=utf-8");
    });

    // Desktop shell info (native window build): lets the UI show "приложение"
    // instead of a browser URL and confirm the embedded window is alive.
    svr.Get("/api/app/info", [&](const httplib::Request &, httplib::Response & res) {
        add_cors(res);
        json out = {
            {"ok", true},
            {"mode", opt.desktop_window ? "desktop" : "console"},
            {"native_dialogs", dualmoe::native_file_dialog_available()},
            {"version", "0.6.1"},
            {"port", bound_port.load(std::memory_order_acquire)},
            {"openai_base_url", openai_base_url_for_json()},
            {"scan_dir", opt.scan_dir},
        };
        res.set_content(out.dump(), "application/json; charset=utf-8");
    });

    // /api/seam?cpu_share=0.9&tokens=32
    // A "GPU ‖ CPU" probe for the live model: applies the CPU share to the running context (no reload),
    // generates a few tokens and reports, from the scheduler's own split timeline, whether the GPU and
    // the CPU really computed experts of the SAME token at the same time (par_layers_dual) or ran one
    // after the other (par_layers_serial). Numbers come from the engine, not from a heuristic.
    svr.Get("/api/seam", [&](const httplib::Request & req, httplib::Response & res) {
        add_cors(res);
        if (!engine.is_loaded()) {
            res.status = 409;
            res.set_content(json{{"ok", false}, {"error", "Модель не загружена"}}.dump(),
                            "application/json; charset=utf-8");
            return;
        }
        float share = 0.85f;
        int32_t tokens = 32;
        try {
            if (req.has_param("cpu_share")) share = std::strtof(req.get_param_value("cpu_share").c_str(), nullptr);
            if (req.has_param("tokens"))    tokens = std::atoi(req.get_param_value("tokens").c_str());
        } catch (...) {}
        share = std::clamp(share, 0.0f, 1.0f);
        tokens = std::clamp(tokens, 8, 256);

        dualmoe::EngineConfig live = engine.current_config();
        live.cpu_share = share;
        std::string err;
        if (!engine.apply_live_config(live, err)) {
            res.status = 409;
            res.set_content(json{{"ok", false}, {"error", err}}.dump(), "application/json; charset=utf-8");
            return;
        }

        // A discard answer warms the route, the second one is the measurement.
        std::string sink, gerr;
        engine.generate_raw("Разогрев маршрута экспертов.", 16, 0.0f, 1u, nullptr, sink, gerr);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = engine.generate_raw(
            "Объясни по пунктам, как движок распределяет экспертов между GPU, CPU и диском.",
            tokens, 0.0f, 1u, nullptr, sink, gerr);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const auto t = engine.telemetry();

        res.set_content(json{
            {"ok", ok},
            {"error", ok ? "" : gerr},
            {"cpu_share", share},
            {"tokens", tokens},
            {"decode_tps", ok ? (double) tokens / std::max(1e-6, sec) : 0.0},
            {"parallel", {
                {"dual_layers", t.par_layers_dual},
                {"serial_layers", t.par_layers_serial},
                {"gpu_ms_per_token", t.par_gpu_ms},
                {"cpu_ms_per_token", t.par_cpu_ms},
                {"overlap_pct", t.par_overlap_pct},
                {"gpu_splits", t.par_gpu_nodes},
                {"cpu_splits", t.par_cpu_nodes},
            }},
            {"experts", {
                {"gpu_from_vram", t.gpu_expert_hits},
                {"cpu_parallel", t.cpu_parallel_experts},
                {"ssd_reads", t.ssd_expert_reads},
            }},
        }.dump(), "application/json; charset=utf-8");
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
            : opt.scan_dir;

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
            : opt.scan_dir;
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
            dualmoe::EngineConfig cfg = opt.boot_cfg;
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
            cfg.auto_split_experts   = body.value("auto_split_experts", true);
            cfg.zero_hdd_mode        = body.value("zero_hdd_mode", cfg.zero_hdd_mode);
            cfg.auto_balance_compute = body.value("auto_balance_compute", cfg.auto_balance_compute);
            cfg.swap_hysteresis      = body.value("swap_hysteresis", cfg.swap_hysteresis);
            cfg.io_lanes             = body.value("io_lanes", cfg.io_lanes);
            cfg.moe_stream           = body.value("moe_stream", true);
            cfg.dense_anon           = body.value("dense_anon", cfg.dense_anon);
            cfg.release_mmap         = body.value("release_mmap", cfg.release_mmap);
            cfg.enable_thinking      = body.value("enable_thinking", cfg.enable_thinking);
            cfg.static_bank_share    = std::clamp(body.value("static_bank_share", cfg.static_bank_share), 0.0f, 0.8f);
            cfg.static_bank_mb       = body.value("static_bank_mb", cfg.static_bank_mb);
            cfg.warm_state           = body.value("warm_state", cfg.warm_state);
            cfg.spec_tokens          = std::clamp(body.value("spec_tokens", cfg.spec_tokens), 0, 8);
            cfg.spec_disk_cache      = body.value("spec_disk_cache", cfg.spec_disk_cache);
            cfg.parallel_seam_share  = std::clamp(body.value("parallel_seam_share", cfg.parallel_seam_share), 0.0f, 0.9f);

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

    svr.Post("/api/settings/thinking", [&](const httplib::Request & req, httplib::Response & res) {
        add_cors(res);
        try {
            const json body = json::parse(req.body);
            const bool en = body.value("enable_thinking", true);
            engine.set_thinking_enabled(en);
            res.set_content(json{{"ok", true}, {"enable_thinking", en}}.dump(), "application/json; charset=utf-8");
        } catch (const std::exception & e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json; charset=utf-8");
        }
    });

    // ---------------------------------------------------------------------
    // Benchmark / autotuner (own tab in the UI)
    // ---------------------------------------------------------------------
    svr.Get("/api/bench/status", [&](const httplib::Request &, httplib::Response & res) {
        add_cors(res);
        res.set_content(bench_to_json(bench.snapshot()).dump(), "application/json; charset=utf-8");
    });

    svr.Post("/api/bench/start", [&](const httplib::Request & req, httplib::Response & res) {
        add_cors(res);
        try {
            const json body = req.body.empty() ? json::object() : json::parse(req.body);
            dualmoe::BenchOptions opt;
            opt.mode      = (body.value("mode", std::string("full")) == "plan")
                                ? dualmoe::BenchMode::Plan : dualmoe::BenchMode::Full;
            opt.model_path = body.value("model_path", std::string(""));
            opt.tokens     = body.value("tokens", 64);
            opt.prompt     = body.value("prompt", std::string(""));
            opt.sweep_threads   = body.value("sweep_threads", true);
            opt.sweep_cpu_share = body.value("sweep_cpu_share", true);
            opt.sweep_seam      = body.value("sweep_seam", opt.sweep_cpu_share);
            opt.sweep_vram_bank = body.value("sweep_vram_bank", true);
            opt.sweep_ubatch    = body.value("sweep_ubatch", true);
            opt.max_candidates  = std::clamp(body.value("max_candidates", 8), 1, 32);
            opt.fast            = body.value("fast", true);
            opt.warmup_tokens   = std::clamp(body.value("warmup_tokens", 8), 0, 64);
            std::string err;
            if (!bench.start(opt, err)) {
                res.status = 409;
                res.set_content(json{{"ok", false}, {"error", err}}.dump(), "application/json; charset=utf-8");
                return;
            }
            res.set_content(bench_to_json(bench.snapshot()).dump(), "application/json; charset=utf-8");
        } catch (const std::exception & e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json; charset=utf-8");
        }
    });

    svr.Post("/api/bench/cancel", [&](const httplib::Request &, httplib::Response & res) {
        add_cors(res);
        bench.cancel();
        res.set_content(json{{"ok", true}}.dump(), "application/json; charset=utf-8");
    });

    // Applies the winning (or a chosen) candidate: reloads the model with it and
    // returns the effective diagnostics so the UI can show the tuned setup.
    svr.Post("/api/bench/apply", [&](const httplib::Request & req, httplib::Response & res) {
        add_cors(res);
        try {
            const json body = req.body.empty() ? json::object() : json::parse(req.body);
            const auto snap = bench.snapshot();
            if (snap.candidates.empty()) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "Сначала запустите бенчмарк"}}.dump(),
                                "application/json; charset=utf-8");
                return;
            }
            int32_t idx = body.value("index", snap.best_index);
            if (idx < 0 || idx >= (int32_t) snap.candidates.size()) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "Некорректный индекс кандидата"}}.dump(),
                                "application/json; charset=utf-8");
                return;
            }
            dualmoe::EngineConfig cfg = snap.candidates[(size_t) idx].cfg;
            if (cfg.model_path.empty()) cfg.model_path = snap.model_path;
            const bool async_mode = body.value("async", false);
            std::string err;
            if (async_mode) {
                if (!engine.start_load_async(cfg, err)) {
                    res.status = 400;
                    res.set_content(json{{"ok", false}, {"error", err}}.dump(), "application/json; charset=utf-8");
                    return;
                }
                res.set_content(json{{"ok", true}, {"async", true}, {"index", idx}}.dump(),
                                "application/json; charset=utf-8");
                return;
            }
            engine.unload_model();
            if (!engine.load_model(cfg, err)) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", err}}.dump(), "application/json; charset=utf-8");
                return;
            }
            res.set_content(json{
                {"ok", true},
                {"index", idx},
                {"model", summary_to_json(engine.current_model())},
                {"telemetry", telemetry_to_json(engine.telemetry())},
                {"diagnostics", diagnostics_to_json(engine.diagnostics(), engine.recent_logs(0))},
            }.dump(), "application/json; charset=utf-8");
        } catch (const std::exception & e) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", e.what()}}.dump(), "application/json; charset=utf-8");
        }
    });

    svr.Get("/api/telemetry/stream", [&](const httplib::Request &, httplib::Response & res) {
        add_cors(res);
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_chunked_content_provider(
            "text/event-stream",
            [&](size_t, httplib::DataSink & sink) {
                if (shutting_down.load(std::memory_order_acquire)) return false;
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

        if (body.contains("enable_thinking") && body["enable_thinking"].is_boolean()) {
            engine.set_thinking_enabled(body["enable_thinking"].get<bool>());
        }

        dualmoe::SamplingParams sp = engine.current_config().sampling;
        if (body.contains("temperature") && body["temperature"].is_number()) {
            sp.temperature = body["temperature"].get<float>();
        }
        if (body.contains("top_p") && body["top_p"].is_number()) {
            sp.top_p = body["top_p"].get<float>();
        }
        if (body.contains("top_k") && body["top_k"].is_number_integer()) {
            sp.top_k = body["top_k"].get<int32_t>();
        }
        if (body.contains("repeat_penalty") && body["repeat_penalty"].is_number()) {
            sp.repeat_penalty = body["repeat_penalty"].get<float>();
        }
        if (body.contains("dry_multiplier") && body["dry_multiplier"].is_number()) {
            sp.dry_multiplier = body["dry_multiplier"].get<float>();
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
            [messages = std::move(messages), sp, model_id, created_ts, this](size_t, httplib::DataSink & sink) {
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
                if (shutting_down.load(std::memory_order_acquire) ||
                    !sink.write(head_str.data(), head_str.size())) {
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
    const int max_tries = opt.port_explicit ? 1 : 9;
    for (int t = 0; t < max_tries; ++t) {
        if (svr.bind_to_port(opt.host, opt.port + t)) {
            bound_port = opt.port + t;
            break;
        }
    }
    if (bound_port < 0) {
        err = "Не удалось занять HTTP-порт " + opt.host + ":" + std::to_string(opt.port) +
              " (порт занят другим приложением или заблокирован антивирусом)";
        return false;
    }
    this->bound_port.store(bound_port, std::memory_order_release);
    return true;
}

AppServer::AppServer() : impl_(std::make_unique<Impl>()) {}

AppServer::~AppServer() { stop(); }

bool AppServer::start(const AppOptions & opt, std::string & err) {
    return impl_->start(opt, err);
}

bool AppServer::model_loading() const {
    return impl_->engine.is_loading();
}

int AppServer::port() const { return impl_->bound_port.load(std::memory_order_acquire); }

std::string AppServer::local_url() const {
    const std::string & host = impl_->opt.host;
    return "http://" + std::string(host == "0.0.0.0" || host.empty() ? "127.0.0.1" : host) +
           ":" + std::to_string(port());
}

std::string AppServer::openai_base_url() const { return local_url() + "/v1"; }

void AppServer::boot_model_sync() {
    const auto & boot_cfg = impl_->opt.boot_cfg;
    if (boot_cfg.model_path.empty()) return;
    std::string err;
    std::cout << "[DualMoE] Автозагрузка стартовой модели: " << boot_cfg.model_path << " ...\n";
    if (!impl_->engine.load_model(boot_cfg, err)) {
        std::cerr << "[DualMoE] Предупреждение: не удалось загрузить стартовую модель: " << err << "\n";
    } else {
        std::cout << "[DualMoE] Модель готова: " << impl_->engine.current_model().model_name << "\n";
    }
}

bool AppServer::boot_model_async(std::string & err) {
    const auto & boot_cfg = impl_->opt.boot_cfg;
    if (boot_cfg.model_path.empty()) return true;
    std::cout << "[DualMoE] Фоновая автозагрузка модели: " << boot_cfg.model_path << " ...\n";
    return impl_->engine.start_load_async(boot_cfg, err);
}

void AppServer::run() {
    impl_->svr.listen_after_bind();
}

void AppServer::stop() {
    if (!impl_) return;
    // Closing the native window must terminate the process immediately: flag
    // every long-lived SSE stream first, then unblock the accept loop.
    impl_->shutting_down.store(true, std::memory_order_release);
    impl_->svr.stop();
}

} // namespace dualmoe
