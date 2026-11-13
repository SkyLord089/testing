#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dualmoe {

struct SamplingParams {
    float    temperature    = 0.7f;
    float    top_p          = 0.92f;
    float    min_p          = 0.05f;
    int32_t  top_k          = 40;
    float    repeat_penalty = 1.05f;
    int32_t  repeat_last_n  = 64;
    uint32_t seed           = 0xFFFFFFFFu;
    int32_t  max_tokens     = 2048;
};

struct EngineConfig {
    std::string model_path;

    // GPU + CPU parallel hybrid controls
    int32_t  n_gpu_layers       = 99;     // Safe for 2..99: cold experts auto-routed to RAM/SSD arena
    size_t   vram_cache_mb      = 2400;   // Hot expert ClassLRU bank in GPU VRAM (MiB) — auto-clamped to free VRAM
    float    cpu_share          = 0.92f;  // Fraction of VRAM cache misses computed in parallel on CPU (0..1)
    bool     auto_split_experts = true;   // Prevents >=2 GPU layer crashes by keeping cold 3D experts off VRAM

    // Host DDR4 + SSD streaming controls
    bool     moe_stream         = true;   // Virtual-arena on-demand expert streaming + RAM budget cap
    size_t   ram_cache_mb       = 5632;   // Committed DDR4 budget for warm/cold streamed experts (MiB)
#if defined(_WIN32)
    bool     dense_anon         = false;  // Keep false on Windows 16 GB RAM to preserve commit headroom
    bool     release_mmap       = false;  // Safe default on Windows: preserves Vulkan driver mappings
#else
    bool     dense_anon         = true;   // Rehome host dense weights into anonymous RAM
    bool     release_mmap       = true;   // Drop OS file mapping on POSIX
#endif
    int32_t  io_lanes           = 4;      // Background positioned SSD reader threads
    int32_t  prefetch_ahead     = 1;      // Prefetch previous-token expert route for layer il+1 on CPU path

    // Context & batch sizing (tuned for RX 580 8 GB VRAM + 16 GB RAM + LearnHarness)
    uint32_t n_ctx              = 24576;
    uint32_t n_batch            = 512;
    uint32_t n_ubatch           = 32;     // Must stay <= 32 (llama_moe_cache::max_batch) so 100% microbatches use cache
    int32_t  n_threads          = 8;      // Decode CPU threads (physical cores on Ryzen 7 5700X)
    int32_t  n_threads_batch    = 14;     // Prefill CPU threads
    std::string kv_type_k       = "q8_0"; // f16 | q8_0 | q4_0
    std::string kv_type_v       = "q8_0"; // Auto-falls back to f16 if backend lacks quantized-V FlashAttn
    int32_t  n_expert_used_override = 0;  // 0 = model default

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
    bool        model_loaded          = false;
    bool        loading               = false;
    bool        generating            = false;
    std::string model_name;
    std::string model_arch;
    std::string status_text           = "Готов к загрузке модели";
    int32_t     load_progress_pct     = 0;
    int32_t     load_phase_index      = 0;
    int32_t     load_phase_total      = 8;

    // Live throughput
    double      prefill_tps           = 0.0;
    double      decode_tps            = 0.0;
    double      ttft_ms               = 0.0;
    uint64_t    prompt_tokens         = 0;
    uint64_t    generated_tokens      = 0;
    uint32_t    kv_used_tokens        = 0;
    uint32_t    kv_max_tokens         = 0;

    // 3-Tier Parallel Execution Counters (GPU ∥ CPU ∥ SSD)
    uint64_t    gpu_expert_hits       = 0;  // Computed on GPU from VRAM ClassLRU bank (0 PCIe, 0 SSD)
    uint64_t    gpu_expert_fills      = 0;  // Promoted & uploaded to GPU VRAM bank
    uint64_t    cpu_parallel_experts  = 0;  // Computed on CPU concurrently with GPU (or CPU-streamed)
    uint64_t    ram_cache_hits        = 0;  // Served directly from committed DDR4 arena
    uint64_t    ssd_expert_reads      = 0;  // Cold misses read from SSD via multi-lane I/O
    uint64_t    ssd_bytes_read        = 0;
    uint64_t    vram_upload_bytes     = 0;
    double      ssd_bandwidth_mbs     = 0.0;

    // Memory Residency
    uint64_t    vram_cache_budget_mb  = 0;
    uint64_t    ram_arena_used_mb     = 0;
    uint64_t    ram_arena_budget_mb   = 0;
    uint64_t    dense_anon_mb         = 0;
    uint64_t    os_ram_avail_mb       = 0;
    uint64_t    os_commit_avail_mb    = 0;
    uint64_t    gpu_vram_free_mb      = 0;
    uint64_t    gpu_vram_total_mb     = 0;
    uint64_t    arena_commit_failures = 0;

    bool        mmap_released         = false;
    bool        auto_split_active     = false;
    bool        parallel_gpu_cpu_active = false;
    int32_t     active_gpu_layers     = 0;
    float       active_cpu_share      = 0.0f;
    uint32_t    active_ubatch         = 32;
};

struct ChatMessage {
    std::string role;
    std::string content;
};

using TokenCallback = std::function<bool(const std::string & piece)>;

} // namespace dualmoe
