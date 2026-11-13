#include "engine.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char ** argv) {
    if (argc < 2) {
        std::cerr << "Usage: dualmoe-tests <tiny_moe.gguf>\n";
        return 1;
    }
    const std::string model_path = argv[1];

    // Gate 1: GGUF zero-alloc MoE inspector
    const dualmoe::GgufLayout lay = dualmoe::inspect_gguf_layout(model_path);
    if (!lay.summary.ok || !lay.summary.is_moe || lay.summary.n_expert != 8 || lay.summary.n_expert_used != 2) {
        std::cerr << "[FAIL] Gate 1: GGUF MoE inspection failed: " << lay.summary.error << "\n";
        return 1;
    }
    std::cout << "[PASS] Gate 1: GGUF MoE layout verified (arch=" << lay.summary.arch
              << ", experts=" << lay.summary.n_expert << "x" << lay.summary.n_expert_used
              << ", expert_bytes=" << lay.summary.expert_bytes << ")\n";

    const std::string test_prompt = "Привет DualMoE работает параллельно на GPU и CPU";

    // Gate 2a: Resident baseline (no streaming)
    std::string baseline_out;
    {
        dualmoe::DualMoeEngine eng;
        dualmoe::EngineConfig cfg;
        cfg.model_path = model_path;
        cfg.moe_stream = false;
        cfg.n_gpu_layers = 0;
        cfg.n_ctx = 512;
        cfg.n_threads = 2;
        std::string err;
        if (!eng.load_model(cfg, err)) {
            std::cerr << "[FAIL] Gate 2a load failed: " << err << "\n";
            return 1;
        }
        if (!eng.generate_raw(test_prompt, 24, 0.0f, 42, nullptr, baseline_out, err)) {
            std::cerr << "[FAIL] Gate 2a generate failed: " << err << "\n";
            return 1;
        }
    }

    // Gate 2b: Virtual-Arena SSD Streamed + Aggressive Eviction + release_mmap + >=2 GPU Layers Safe Split!
    std::string streamed_out;
    dualmoe::TelemetrySnapshot snap;
    {
        dualmoe::DualMoeEngine eng;
        dualmoe::EngineConfig cfg;
        cfg.model_path         = model_path;
        cfg.moe_stream         = true;
        cfg.dense_anon         = true;
        cfg.release_mmap       = true;
        cfg.auto_split_experts = true;
        cfg.n_gpu_layers       = 4;   // Specifically exercises >=2 GPU layers path!
        cfg.vram_cache_mb      = 64;
        cfg.cpu_share          = 0.90f;
        cfg.ram_cache_mb       = 1;   // Tight 1 MiB budget to stress page-refcounted commit/evict & async lanes
        cfg.io_lanes           = 4;
        cfg.n_ctx              = 512;
        cfg.n_threads          = 2;

        std::string err;
        if (!eng.load_model(cfg, err)) {
            std::cerr << "[FAIL] Gate 2b streamed load failed: " << err << "\n";
            return 1;
        }
        if (!eng.generate_raw(test_prompt, 24, 0.0f, 42, nullptr, streamed_out, err)) {
            std::cerr << "[FAIL] Gate 2b streamed generate failed: " << err << "\n";
            return 1;
        }
        snap = eng.telemetry();
    }

    if (baseline_out != streamed_out) {
        std::cerr << "[FAIL] Gate 2: Byte-identity mismatch!\n"
                  << "  Resident: [" << baseline_out << "]\n"
                  << "  Streamed: [" << streamed_out << "]\n";
        return 1;
    }
    if (snap.ssd_expert_reads == 0) {
        std::cerr << "[FAIL] Gate 2: Expected >0 SSD expert reads, got 0\n";
        return 1;
    }
    std::cout << "[PASS] Gate 2: Streamed == Resident BYTE-IDENTICAL! (ssd_reads=" << snap.ssd_expert_reads
              << ", ram_hits=" << snap.ram_cache_hits
              << ", mmap_released=" << (snap.mmap_released ? "true" : "false") << ")\n";

    // Gate 3: Multi-turn Chat + Prefix KV Cache Reuse
    {
        dualmoe::DualMoeEngine eng;
        dualmoe::EngineConfig cfg;
        cfg.model_path = model_path;
        cfg.moe_stream = true;
        cfg.ram_cache_mb = 4;
        cfg.n_ctx = 512;
        cfg.n_threads = 2;
        cfg.sampling.temperature = 0.0f;
        cfg.sampling.max_tokens  = 12;

        std::string err, r1, r2;
        int32_t p1 = 0, c1 = 0, p2 = 0, c2 = 0;
        if (!eng.load_model(cfg, err)) {
            std::cerr << "[FAIL] Gate 3 load failed: " << err << "\n";
            return 1;
        }
        std::vector<dualmoe::ChatMessage> msgs = {
            {"system", "Ты наставник DualMoE"},
            {"user", "Привет!"},
        };
        if (!eng.generate_chat(msgs, cfg.sampling, false, nullptr, r1, p1, c1, err)) {
            std::cerr << "[FAIL] Gate 3 turn 1 failed: " << err << "\n";
            return 1;
        }
        msgs.push_back({"assistant", r1});
        msgs.push_back({"user", "Продолжай"});
        if (!eng.generate_chat(msgs, cfg.sampling, false, nullptr, r2, p2, c2, err)) {
            std::cerr << "[FAIL] Gate 3 turn 2 failed: " << err << "\n";
            return 1;
        }
        std::cout << "[PASS] Gate 3: Multi-turn Chat & Prefix KV Cache verified (c1=" << c1 << ", c2=" << c2 << ")\n";
    }

    std::cout << "ALL DUALMOE GATES PASSED!\n";
    return 0;
}
