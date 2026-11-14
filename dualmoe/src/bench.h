// DualMoE Benchmark — autotuner for the hybrid GPU ∥ CPU / VRAM ⊕ RAM engine.
//
// Two modes:
//   * Plan  — instant (no reload): inspects the GGUF + free VRAM/RAM and derives
//             the best-known-safe EngineConfig (KV budget, hot expert bank,
//             ubatch, threads) with a human readable rationale.
//   * Full  — real measurement: coordinate-descent over the reload-only knobs
//             (threads, cpu_share, vram bank, ubatch), reloading the model for
//             every candidate and benchmarking a fixed greedy generation.
//
// The winner is left loaded, so the user continues chatting with the tuned
// parameters instead of the defaults.
#pragma once

#include "engine.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dualmoe {

enum class BenchMode { Plan, Full };

struct BenchOptions {
    std::string model_path;              // empty -> currently loaded model
    BenchMode   mode              = BenchMode::Full;
    int32_t     tokens            = 96;  // generated tokens per candidate
    std::string prompt;                  // empty -> built-in benchmark prompt
    bool        sweep_threads     = true;
    bool        sweep_cpu_share   = true;
    bool        sweep_vram_bank   = true;
    bool        sweep_ubatch      = true;
    int32_t     max_candidates    = 16;  // hard cap: every candidate = one model reload
    uint32_t    seed              = 424242u;
};

struct BenchCandidate {
    int32_t     index        = 0;
    std::string label;                  // "потоки 8 • CPU 0.85 • VRAM 4600 МБ"
    std::vector<std::string> knobs;     // ["потоки=8", ...]
    EngineConfig cfg;
    bool        ok           = false;
    bool        measured     = false;   // false in Plan mode (estimate only)
    std::string error;
    double      prefill_tps  = 0.0;
    double      decode_tps   = 0.0;
    double      ttft_ms      = 0.0;
    double      vram_hit_pct = 0.0;
    uint64_t    hdd_reads    = 0;
    uint64_t    ram_used_mb  = 0;
    uint64_t    vram_used_mb = 0;
    double      score        = 0.0;
    double      elapsed_s    = 0.0;
    bool        best         = false;
};

struct BenchRecommendation {
    bool                     ok = false;
    EngineConfig             cfg;
    std::vector<std::string> reasons;
    double                   predicted_decode_tps = 0.0;
    uint64_t                 predicted_vram_mb    = 0;
    uint64_t                 predicted_ram_mb     = 0;
    double                   predicted_kv_mb      = 0.0;
};

struct BenchmarkSnapshot {
    std::string stage          = "idle";  // idle | planning | running | done | canceled | failed
    std::string mode           = "full";
    int32_t     progress_pct   = 0;
    std::string message        = "Готов к запуску";
    int32_t     candidates_total = 0;
    int32_t     candidates_done  = 0;
    int32_t     current_index    = -1;
    std::string current_label;
    std::string model_path;
    std::string model_name;
    std::string error;
    double      elapsed_s      = 0.0;
    int32_t     best_index     = -1;
    std::vector<BenchCandidate>      candidates;
    std::vector<std::string>         log;
    BenchRecommendation              recommendation;
};

class BenchmarkRunner {
public:
    explicit BenchmarkRunner(DualMoeEngine & engine);
    ~BenchmarkRunner();

    BenchmarkRunner(const BenchmarkRunner &) = delete;
    BenchmarkRunner & operator=(const BenchmarkRunner &) = delete;

    bool start(const BenchOptions & opt, std::string & err);
    void cancel();
    bool running() const { return running_.load(std::memory_order_acquire); }

    BenchmarkSnapshot snapshot() const;
    bool best_config(EngineConfig & out) const;

private:
    void run_plan(const BenchOptions & opt);
    void run_full(const BenchOptions & opt);

    BenchRecommendation plan_config(const BenchOptions & opt, std::string & err) const;
    bool   measure_candidate(BenchCandidate & cand, const BenchOptions & opt,
                             const std::string & prompt, std::string & err);

    void set_stage(const std::string & stage, const std::string & message, int32_t pct);
    void push_log(const std::string & line);
    void publish(const std::vector<BenchCandidate> & cands, int32_t current);

    DualMoeEngine & engine_;
    std::thread     worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};

    mutable std::mutex mu_;
    BenchmarkSnapshot  snap_;
};

} // namespace dualmoe
