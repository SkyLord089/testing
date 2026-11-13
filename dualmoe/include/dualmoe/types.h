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
    size_t   vram_cache_mb      = 2800;   // Hot expert ClassLRU bank in GPU VRAM (MiB)
    float    cpu_share          = 0.92f;  // Fraction of VRAM cache misses computed in parallel on CPU (0..1)
    bool     auto_split_experts = true;   // Prevents >=2 GPU layer crashes by keeping cold 3D experts off VRAM

    // Host DDR4 + SSD streaming controls
    bool     moe_stream         = true;   // Virtual-arena on-demand expert streaming + RAM budget cap
    size_t   ram_cache_mb       = 6144;   // Committed DDR4 budget for warm/cold streamed experts (MiB)
    bool     dense_anon         = true;   // Rehome host dense weights into anonymous RAM
    bool     release_mmap       = true;   // Drop OS file mapping (fixes Windows NTFS read serialization)
    int32_t  io_lanes           = 4;      // Background positioned SSD reader threads
    int32_t  prefetch_ahead     = 1;      // Prefetch previous-token expert route for layer il+1 on CPU path

    // Context & batch sizing (tuned for 8 GB VRAM + 16 GB RAM + LearnHarness)
    uint32_t n_ctx              = 24576;
    uint32_t n_batch            = 512;
    uint32_t n_ubatch           = 256;    // Compact scratch buffers to prevent RX 580 Vulkan OOM
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
    bool        is_gpu    = false;
    uint64_t    free_mb   = 0;
    uint64_t    total_mb  = 0;
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

struct TelemetrySnapshot {
    bool        model_loaded          = false;
    bool        generating            = false;
    std::string model_name;
    std::string model_arch;
    std::string status_text           = "Готов к загрузке модели";

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
    bool        mmap_released         = false;
    bool        auto_split_active     = false;
    bool        parallel_gpu_cpu_active = false;
    int32_t     active_gpu_layers     = 0;
    float       active_cpu_share      = 0.0f;
};

struct ChatMessage {
    std::string role;
    std::string content;
};

using TokenCallback = std::function<bool(const std::string & piece)>;

} // namespace dualmoe
