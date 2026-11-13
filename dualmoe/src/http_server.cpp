#include "engine.h"
#include "embedded_webui.h"

#include "httplib.h"
#include "nlohmann/json.hpp"

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

using json = nlohmann::json;

namespace {

json telemetry_to_json(const dualmoe::TelemetrySnapshot & t) {
    return {
        {"model_loaded", t.model_loaded},
        {"generating", t.generating},
        {"model_name", t.model_name},
        {"model_arch", t.model_arch},
        {"status_text", t.status_text},
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
        {"mmap_released", t.mmap_released},
        {"auto_split_active", t.auto_split_active},
        {"parallel_gpu_cpu_active", t.parallel_gpu_cpu_active},
        {"active_gpu_layers", t.active_gpu_layers},
        {"active_cpu_share", t.active_cpu_share},
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

// Supports both plain string content and OpenAI/Vercel AI SDK content block arrays
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
    std::string host       = "0.0.0.0";
    int         port       = 8090;
    std::string scan_dir   = ".";
    dualmoe::EngineConfig boot_cfg;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_val = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : "";
        };
        if (arg == "--host") host = next_val();
        else if (arg == "--port" || arg == "-p") port = std::atoi(next_val().c_str());
        else if (arg == "--model" || arg == "-m") boot_cfg.model_path = next_val();
        else if (arg == "--models-dir") scan_dir = next_val();
        else if (arg == "--ngl" || arg == "--gpu-layers") boot_cfg.n_gpu_layers = std::atoi(next_val().c_str());
        else if (arg == "--vram-cache-mb") boot_cfg.vram_cache_mb = (size_t) std::atoll(next_val().c_str());
        else if (arg == "--ram-cache-mb") boot_cfg.ram_cache_mb = (size_t) std::atoll(next_val().c_str());
        else if (arg == "--cpu-share") boot_cfg.cpu_share = std::strtof(next_val().c_str(), nullptr);
        else if (arg == "--ctx" || arg == "-c") boot_cfg.n_ctx = (uint32_t) std::atoi(next_val().c_str());
        else if (arg == "--threads" || arg == "-t") boot_cfg.n_threads = std::atoi(next_val().c_str());
        else if (arg == "--help" || arg == "-h") {
            std::cout << "DualMoE Server (GPU || CPU || SSD Parallel MoE Engine)\n"
                      << "  --host <addr>           Bind address (default 0.0.0.0)\n"
                      << "  --port <port>           HTTP/OpenAI port (default 8090)\n"
                      << "  --model <file.gguf>     Auto-load GGUF model at startup\n"
                      << "  --models-dir <folder>   Default folder for GGUF UI scanner\n"
                      << "  --ngl <N>               GPU layers for Attention/Router (default 99)\n"
                      << "  --vram-cache-mb <MiB>   Hot expert bank in GPU VRAM (default 2800)\n"
                      << "  --cpu-share <0..1>      Fraction of misses computed on CPU in parallel (default 0.92)\n"
                      << "  --ram-cache-mb <MiB>    DDR4 virtual arena budget for experts (default 6144)\n"
                      << "  --ctx <tokens>          Context window (default 24576)\n"
                      << "  --threads <N>           CPU decode threads (default 8)\n";
            return 0;
        }
    }

    dualmoe::DualMoeEngine engine;

    if (!boot_cfg.model_path.empty()) {
        if (scan_dir == ".") {
            const auto parent = std::filesystem::path(boot_cfg.model_path).parent_path().string();
            if (!parent.empty()) scan_dir = parent;
        }
        std::string err;
        std::cout << "[DualMoE] Автозагрузка модели: " << boot_cfg.model_path << " ...\n";
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
            });
        }
        const auto cfg = engine.current_config();
        json out = {
            {"ok", true},
            {"default_scan_dir", scan_dir},
            {"devices", devs},
            {"config", {
                {"model_path", cfg.model_path},
                {"n_gpu_layers", cfg.n_gpu_layers},
                {"vram_cache_mb", cfg.vram_cache_mb},
                {"cpu_share", cfg.cpu_share},
                {"ram_cache_mb", cfg.ram_cache_mb},
                {"n_ctx", cfg.n_ctx},
                {"n_threads", cfg.n_threads},
            }},
            {"telemetry", telemetry_to_json(engine.telemetry())},
        };
        res.set_content(out.dump(), "application/json; charset=utf-8");
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
        res.set_content(json{{"ok", true}, {"models", arr}}.dump(), "application/json; charset=utf-8");
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
            cfg.auto_split_experts = body.value("auto_split_experts", true);
            cfg.moe_stream         = body.value("moe_stream", true);
            cfg.release_mmap       = body.value("release_mmap", true);

            std::string err;
            if (!engine.load_model(cfg, err)) {
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", err}}.dump(), "application/json; charset=utf-8");
                return;
            }
            res.set_content(json{
                {"ok", true},
                {"model", summary_to_json(engine.current_model())},
                {"telemetry", telemetry_to_json(engine.telemetry())},
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
                const std::string line = "data: " + telemetry_to_json(engine.telemetry()).dump() + "\n\n";
                if (!sink.write(line.data(), line.size())) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(450));
                return true;
            });
    });

    // OpenAI-compatible endpoints (LearnHarness, Cursor, Continue, OpenWebUI)
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
        if (!engine.is_loaded()) {
            res.status = 503;
            res.set_content(json{{"error", {{"message", "Сначала загрузите .gguf модель в DualMoE Studio"}}}}.dump(),
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

                // Initial role delta per OpenAI SSE spec
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

    std::cout << "\n==========================================================\n"
              << "  DualMoE Studio & OpenAI API запущен: http://"
              << (host == "0.0.0.0" ? "127.0.0.1" : host) << ":" << port << "\n"
              << "  OpenAI / LearnHarness Base URL:      http://127.0.0.1:" << port << "/v1\n"
              << "==========================================================\n";
    std::cout.flush();

    if (!svr.listen(host, port)) {
        std::cerr << "[DualMoE] Ошибка привязки порта " << host << ":" << port << "\n";
        return 1;
    }
    return 0;
}
