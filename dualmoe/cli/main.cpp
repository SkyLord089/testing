#include "engine.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char ** argv) {
    dualmoe::EngineConfig cfg;
    std::string prompt     = "Привет! Кратко объясни, как работает параллельное вычисление экспертов.";
    int32_t     max_tokens = 128;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_val = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : "";
        };
        if (arg == "--model" || arg == "-m") cfg.model_path = next_val();
        else if (arg == "--prompt" || arg == "-p") prompt = next_val();
        else if (arg == "--ngl" || arg == "--gpu-layers") cfg.n_gpu_layers = std::atoi(next_val().c_str());
        else if (arg == "--vram-cache-mb") cfg.vram_cache_mb = (size_t) std::atoll(next_val().c_str());
        else if (arg == "--ram-cache-mb") cfg.ram_cache_mb = (size_t) std::atoll(next_val().c_str());
        else if (arg == "--cpu-share") cfg.cpu_share = std::strtof(next_val().c_str(), nullptr);
        else if (arg == "--no-stream") cfg.moe_stream = false;
        else if (arg == "--max-tokens" || arg == "-n") max_tokens = std::atoi(next_val().c_str());
        else if (arg == "--temp") cfg.sampling.temperature = std::strtof(next_val().c_str(), nullptr);
        else if (arg == "--threads" || arg == "-t") cfg.n_threads = std::atoi(next_val().c_str());
    }

    if (cfg.model_path.empty()) {
        std::cerr << "Использование: dualmoe-cli -m <model.gguf> [-p <prompt>] [--ngl 99] [--vram-cache-mb 2800] [--cpu-share 0.92]\n";
        return 1;
    }

    dualmoe::DualMoeEngine engine;
    std::string err;
    if (!engine.load_model(cfg, err)) {
        std::cerr << "Ошибка загрузки: " << err << "\n";
        return 1;
    }

    std::string reply;
    if (!engine.generate_raw(prompt, max_tokens, cfg.sampling.temperature, cfg.sampling.seed,
                             [](const std::string & piece) {
                                 std::cout << piece << std::flush;
                                 return true;
                             },
                             reply, err)) {
        std::cerr << "\nОшибка генерации: " << err << "\n";
        return 1;
    }

    const auto t = engine.telemetry();
    std::cout << "\n\n--- Телеметрия DualMoE ---\n"
              << "Prefill: " << t.prefill_tps << " tok/s | Decode: " << t.decode_tps << " tok/s | TTFT: " << t.ttft_ms << " ms\n"
              << "GPU VRAM Hits: " << t.gpu_expert_hits << " | CPU Parallel Experts: " << t.cpu_parallel_experts
              << " | SSD Reads: " << t.ssd_expert_reads << " (" << t.ssd_bandwidth_mbs << " MB/s)\n"
              << "DDR4 Arena: " << t.ram_arena_used_mb << " / " << t.ram_arena_budget_mb
              << " MiB | mmap released: " << (t.mmap_released ? "yes" : "no") << "\n";
    return 0;
}
