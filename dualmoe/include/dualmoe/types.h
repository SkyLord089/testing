#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dualmoe {

struct SamplingParams {
    float    temperature    = 0.7f;
    float    top_p          = 0.95f;
    float    min_p          = 0.05f;
    int32_t  top_k          = 64;
    float    repeat_penalty = 1.02f;
    int32_t  repeat_last_n  = 64;
    float    dry_multiplier = 0.8f;       // Phrase-level anti-repetition loop breaker (0 = off)
    uint32_t seed           = 0xFFFFFFFFu;
    int32_t  max_tokens     = 2048;
};

struct EngineConfig {
    std::string model_path;

    // GPU + CPU parallel hybrid & Zero-HDD VRAM <-> RAM engine controls
    int32_t  n_gpu_layers         = 99;     // Safe for 2..99: cold experts auto-routed to RAM arena
    size_t   vram_cache_mb        = 4600;   // Hot expert ClassLRU bank in GPU VRAM (MiB) — auto-clamped to ~90% free VRAM
    float    cpu_share            = 0.85f;  // Initial fraction of VRAM misses computed in parallel on CPU (0..1)
    bool     auto_split_experts   = true;   // Prevents >=2 GPU layer crashes by keeping cold 3D experts off VRAM
    bool     zero_hdd_mode        = true;   // Sequential 1-pass HDD preload into VRAM + RAM -> 0 disk reads in chat
    bool     auto_balance_compute = true;   // Live GPU || CPU dynamic load balancer + hot/cold swap hysteresis
    float    swap_hysteresis      = 1.22f;  // CRF threshold multiplier before swapping hot RAM <-> cold VRAM
    bool     enable_thinking      = true;   // Enable native reasoning channel (<|channel>thought / <think>) when supported

    // Host DDR4 + fallback disk controls
    bool     moe_stream           = true;   // Virtual-arena backing for MoE 3D experts (supports disjoint VRAM+RAM)
    size_t   ram_cache_mb         = 9728;   // Committed DDR4 budget for RAM-resident experts (auto-clamped to safe RAM)
#if defined(_WIN32)
    bool     dense_anon           = false;  // Keep false on Windows 16 GB RAM to preserve commit headroom
    bool     release_mmap         = false;  // Safe default on Windows: preserves Vulkan driver mappings
#else
    bool     dense_anon           = true;   // Rehome host dense weights into anonymous RAM
    bool     release_mmap         = true;   // Drop OS file mapping on POSIX
#endif
    int32_t  io_lanes             = 1;      // 1 = HDD-safe sequential reader (no head thrashing); 4 = NVMe SSD
    int32_t  prefetch_ahead       = 1;      // Prefetch previous-token expert route for layer il+1 on CPU path

    // Expert cache classes (see bench plan): a share of the hot VRAM bank is reserved for the
    // "static" experts (structure + system prompt) that every request re-uses, so a topic shift
    // evicts only the "dynamic" ones instead of thrashing the whole bank.
    float    static_bank_share    = 0.35f;  // 0 = single ClassLRU bank, 0.2..0.5 sensible
    size_t   static_bank_mb       = 0;      // 0 = derived: vram_cache_mb * static_bank_share
    bool     warm_state           = true;   // persist the hot expert set next to the model between runs
    int32_t  warm_state_max_kb    = 256;    // cap of the warm-state file

    // Context & batch sizing (tuned for RX 580 8 GB VRAM + 16 GB RAM + LearnHarness)
    uint32_t n_ctx                = 16384;
    uint32_t n_batch              = 512;
    uint32_t n_ubatch             = 32;     // Must stay <= 32 (llama_moe_cache::max_batch) so 100% microbatches use cache
    int32_t  n_threads            = 8;      // Decode CPU threads (physical cores on Ryzen 7 5700X)
    int32_t  n_threads_batch      = 14;     // Prefill CPU threads
    std::string kv_type_k         = "q8_0"; // f16 | q8_0 | q4_0
    std::string kv_type_v         = "f16";  // f16 default preserves Gemma 4 shared-KV precision & Polaris RX 580 compat
    int32_t  n_expert_used_override = 0;    // 0 = model default

    SamplingParams sampling;
};

struct DeviceInfo {
    std::string name;
    std::string description;
    std::string backend;
    bool        is_gpu        = false;
    uint64_t    free_mb       = 0;
    uint64_t    total_mb      = 0;
    uint64_t    max_buffer_mb = 0;
};

struct ModelSummary {
    bool        ok              = false;
    std::string error;
    std::string path;
    std::string filename;
    std::string arch;
    std::string model_name;
    uint64_t    file_size_bytes = 0;
    int32_t     n_layer         = 0;
    int32_t     n_embd          = 0;
    int32_t     n_expert        = 0;
    int32_t     n_expert_used   = 0;
    int32_t     n_ctx_train     = 0;
    int32_t     n_swa           = 0;      // Sliding window attention span (e.g. 1024 on Gemma 4)
    int32_t     n_kv_shared_layers = 0;   // Shared KV layers with 0 own KV allocation (e.g. Gemma 4)
    int32_t     n_swa_layers    = 0;      // Count of own-KV layers using sliding window
    int32_t     kv_dim_full     = 0;      // K or V elements per token on full-attention layers
    int32_t     kv_dim_swa      = 0;      // K or V elements per token on SWA layers
    bool        is_moe          = false;
    uint64_t    expert_bytes    = 0;
    uint64_t    dense_bytes     = 0;
    uint64_t    cycle_floor_mb  = 0;      // Minimum 1-token worst-case expert cycle in MiB
    std::vector<std::string> shard_paths;
};

struct SystemMemoryInfo {
    uint64_t    ram_total_mb       = 0;
    uint64_t    ram_avail_mb       = 0;
    uint64_t    commit_total_mb    = 0;   // Windows Virtual Commit Limit (RAM + pagefile.sys)
    uint64_t    commit_avail_mb    = 0;   // Available Virtual Commit before OS refuses VirtualAlloc
    uint64_t    vram_total_mb      = 0;
    uint64_t    vram_free_mb       = 0;
    uint64_t    vram_max_buffer_mb = 0;   // Vulkan maxStorageBufferRange / suballocation limit
    std::string gpu_name;
    std::string gpu_backend;
};

struct LoadStepRecord {
    int32_t     index      = 0;           // 1..8
    std::string id;
    std::string title;
    std::string detail;
    std::string risk_hint;                // Explains potential crash causes at this stage
    std::string state      = "pending";   // pending | running | done | warn | error
    double      elapsed_ms = 0.0;
};

struct LoadLogEntry {
    uint64_t    seq       = 0;
    std::string timestamp;
    std::string level;                    // STEP | INFO | WARN | ERROR | VULKAN
    std::string text;
};

struct CrashForensicsReport {
    bool        has_previous_crash = false;
    std::string crash_time;
    std::string crash_stage;
    std::string crash_reason;
    std::string crash_advice;
    std::string log_file_path;
    std::vector<std::string> last_log_tail;
};

struct LoadDiagnosticsSnapshot {
    bool        loading                  = false;
    int32_t     phase_index              = 0;
    int32_t     phase_total              = 8;
    int32_t     progress_pct             = 0;
    std::string current_stage            = "Ожидание выбора модели";
    std::string last_error;
    std::string log_file_path;

    // Requested vs Safe Auto-Adjusted Parameters
    int32_t     requested_ngl            = 0;
    int32_t     effective_ngl            = 0;
    uint64_t    requested_vram_cache_mb  = 0;
    uint64_t    effective_vram_cache_mb  = 0;
    uint64_t    requested_ram_cache_mb   = 0;
    uint64_t    effective_ram_cache_mb   = 0;
    uint32_t    requested_ctx            = 0;
    uint32_t    effective_ctx            = 0;
    uint32_t    effective_ubatch         = 32;

    std::vector<std::string>    warnings;
    std::vector<LoadStepRecord> steps;
    SystemMemoryInfo            sys_mem;
    CrashForensicsReport        prev_crash;
};

struct TelemetrySnapshot {
    bool        model_loaded            = false;
    bool        loading                 = false;
    bool        generating              = false;
    std::string model_name;
    std::string model_arch;
    std::string status_text             = "Готов к загрузке модели";
    int32_t     load_progress_pct       = 0;
    int32_t     load_phase_index        = 0;
    int32_t     load_phase_total        = 8;

    // Live throughput
    double      prefill_tps             = 0.0;
    double      decode_tps              = 0.0;
    double      ttft_ms                 = 0.0;
    uint64_t    prompt_tokens           = 0;
    uint64_t    generated_tokens        = 0;
    uint32_t    kv_used_tokens          = 0;
    uint32_t    kv_max_tokens           = 0;

    // Dual-Compute & VRAM <-> RAM Swap Counters
    uint64_t    gpu_expert_hits         = 0;  // Computed on GPU from VRAM ClassLRU bank (0 PCIe, 0 HDD)
    uint64_t    gpu_expert_fills        = 0;  // Hot promotions uploaded RAM -> VRAM
    uint64_t    cpu_parallel_experts    = 0;  // Computed on Ryzen AVX2 concurrently from DDR4 RAM
    uint64_t    ram_cache_hits          = 0;  // Served directly from committed DDR4 arena (0 HDD)
    uint64_t    ram_to_vram_swaps       = 0;  // Hot RAM -> VRAM promotions
    uint64_t    vram_to_ram_swaps       = 0;  // Cold VRAM -> RAM evictions
    uint64_t    ssd_expert_reads        = 0;  // Runtime disk misses AFTER preload (0 in Zero-HDD mode!)
    uint64_t    ssd_bytes_read          = 0;
    uint64_t    vram_upload_bytes       = 0;
    double      ssd_bandwidth_mbs       = 0.0;
    double      preload_bandwidth_mbs   = 0.0;

    // Silicon Residency (VRAM + DDR4 RAM)
    uint64_t    vram_cache_budget_mb    = 0;
    uint64_t    vram_prewarmed_mb       = 0;
    uint64_t    ram_arena_used_mb       = 0;
    uint64_t    ram_arena_budget_mb     = 0;
    uint64_t    ram_preloaded_mb        = 0;
    uint64_t    dense_anon_mb           = 0;
    uint64_t    os_ram_avail_mb         = 0;
    uint64_t    os_commit_avail_mb      = 0;
    uint64_t    gpu_vram_free_mb        = 0;
    uint64_t    gpu_vram_total_mb       = 0;
    uint64_t    arena_commit_failures   = 0;
    double      silicon_residency_pct   = 0.0; // % of model held in VRAM + DDR4 RAM (100% = Zero HDD!)

    bool        mmap_released           = false;
    bool        auto_split_active       = false;
    bool        parallel_gpu_cpu_active = false;
    bool        zero_hdd_active         = false;
    bool        disjoint_swap_active    = false;
    bool        auto_balance_active     = false;
    bool        thinking_enabled        = true;
    int32_t     active_gpu_layers       = 0;
    float       active_cpu_share        = 0.0f;
    uint32_t    active_ubatch           = 32;
};

struct ChatMessage {
    std::string role;
    std::string content;
};

using TokenCallback       = std::function<bool(const std::string & piece)>;
using TokenStreamCallback = TokenCallback;

} // namespace dualmoe
