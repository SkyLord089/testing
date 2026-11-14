#pragma once

#include "gguf_inspect.h"
#include "ggml.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dualmoe {

struct DenseRehomeEntry {
    ggml_tensor *              primary = nullptr;
    std::vector<ggml_tensor *> aliases;
    int32_t                    shard_idx   = 0;
    uint64_t                   file_offset = 0;
    uint64_t                   nbytes      = 0;
};

struct StreamedProjection {
    ggml_tensor * tensor      = nullptr;
    std::string   name;
    int32_t       shard_idx   = 0;
    uint64_t      file_offset = 0;
    uint64_t      nb2         = 0;   // Bytes per expert along dim 2
    uint64_t      total_bytes = 0;
    uint8_t *     arena_base  = nullptr;
    uint64_t      arena_cap   = 0;
    std::vector<uint16_t> page_refs; // Per-OS-page reference count (prevents boundary eviction bugs)
    std::unique_ptr<std::atomic<uint8_t>[]> state; // 0 = EMPTY, 1 = LOADING, 2 = READY, 3 = IN_VRAM
};

struct StreamedLayer {
    int32_t                         il = -1;
    std::vector<StreamedProjection> projs;
    uint64_t                        bytes_per_expert = 0;
};

// Virtual-Memory Expert Arena + Zero-HDD Sequential Preloader + Bidirectional VRAM <-> RAM Swap Backing
class VirtualExpertArena {
public:
    VirtualExpertArena();
    ~VirtualExpertArena();

    VirtualExpertArena(const VirtualExpertArena &) = delete;
    VirtualExpertArena & operator=(const VirtualExpertArena &) = delete;

    bool init(const std::vector<std::string> & shard_paths,
              int32_t n_layer,
              int32_t n_expert,
              size_t ram_budget_bytes,
              int32_t io_lanes,
              bool sequential_hdd_mode,
              std::string & err);

    void shutdown();

    void set_disjoint_vram_mode(bool enabled) { disjoint_vram_mode_ = enabled; }
    bool disjoint_vram_mode() const { return disjoint_vram_mode_; }

    void set_vram_rescue_callback(std::function<bool(int32_t il, int32_t exp)> fn) {
        vram_rescue_fn_ = std::move(fn);
    }

    // Bind a captured 3D expert tensor into its own uncommitted virtual address reservation
    bool bind_layer_projection(int32_t il,
                               ggml_tensor * tensor,
                               int32_t shard_idx,
                               uint64_t file_offset,
                               uint64_t nbytes,
                               std::string & err);

    // Rehome host dense weights into anonymous RAM so GGUF file mappings can be safely dropped
    bool rehome_dense_weights(const std::vector<DenseRehomeEntry> & entries, std::string & err);

    // Drop OS file mappings strictly matching our .gguf shards and reopen clean positioned readers
    bool release_file_mappings(const std::vector<const void *> & check_host_ptrs);

    // Sequential zero-seek read helper used by llama_moe_cache_prewarm (with 16 MiB contiguous read-ahead)
    bool fetch_expert_for_vram_prewarm(int32_t il,
                                       const char * tensor_name,
                                       int32_t exp,
                                       void * dst_buf,
                                       size_t nbytes);

    // Single-pass ascending disk-offset sweep loading warm/cold experts into committed DDR4 RAM (0 random seeks!)
    uint64_t preload_remaining_to_ram_sequential(
        uint64_t max_ram_bytes,
        const std::function<void(uint64_t done_bytes, uint64_t target_bytes, double mbs)> & progress_cb = nullptr);

    // Reset runtime disk counters right after sequential preload completes so chat tracks strictly runtime I/O
    void mark_preload_complete();

    // Bidirectional VRAM <-> RAM swap hooks invoked by llama_moe_cache::copy()
    void * prepare_vram_eviction_dst(int32_t evicted_il,
                                     const char * tensor_name,
                                     int32_t evicted_exp,
                                     size_t nbytes);

    void * prepare_vram_rescue_dst(int32_t il,
                                   const char * tensor_name,
                                   int32_t exp,
                                   size_t nbytes);

    void on_experts_promoted_to_vram(int32_t il,
                                     const int32_t * promoted_experts,
                                     size_t n_promoted,
                                     size_t n_evicted_to_ram);

    void flush_deferred_promotions();

    // Synchronous load (for GPU VRAM cache fills that are about to be DMA-uploaded)
    void ensure_experts_sync(int32_t il, const int32_t * experts, size_t count);

    // Asynchronous prefetch (for parallel CPU misses & route-ahead)
    void prefetch_experts_async(int32_t il, const int32_t * experts, size_t count);

    // Per-expert CPU matmul barrier hooked via ggml_cpu_set_expert_ready_hook
    void wait_tensor_expert_ready(const ggml_tensor * src0, int32_t expert_id);

    // Static C callback wrapper for ggml_cpu_set_expert_ready_hook
    static void c_expert_ready_hook(const struct ggml_tensor * src0, int32_t expert_id, void * user_data);

    // Live telemetry getters
    uint64_t committed_bytes() const { return committed_bytes_.load(std::memory_order_relaxed); }
    uint64_t budget_bytes() const { return budget_bytes_; }
    uint64_t dense_anon_bytes() const { return dense_bytes_; }
    uint64_t ram_hits() const { return ram_hits_.load(std::memory_order_relaxed); }
    uint64_t ssd_reads() const { return ssd_reads_.load(std::memory_order_relaxed); }
    uint64_t ssd_bytes_read() const { return ssd_bytes_read_.load(std::memory_order_relaxed); }
    uint64_t ram_to_vram_swaps() const { return ram_to_vram_swaps_.load(std::memory_order_relaxed); }
    uint64_t vram_to_ram_swaps() const { return vram_to_ram_swaps_.load(std::memory_order_relaxed); }
    uint64_t commit_failures() const { return commit_failures_.load(std::memory_order_relaxed); }
    uint64_t total_expert_bytes() const { return total_expert_bytes_; }
    // Experts of each layer (bytes per expert is uniform within a layer, not across layers).
    uint64_t layer_bytes_per_expert(int32_t il) const {
        if (il < 0 || (size_t) il >= layers_.size()) return 0;
        return layers_[(size_t) il].bytes_per_expert;
    }
    // How many (layer, expert) slots count as one expert of the whole model, and how many of them are
    // resident in DDR4 right now. Used for the residency report ("how much of the model is in RAM").
    uint64_t total_expert_slots() const { return (uint64_t) layers_.size() * (uint64_t) n_expert_; }
    uint64_t resident_expert_slots() const;
    // Tightest per-layer bytes-per-expert, i.e. the smallest indivisible step of the two budgets.
    uint64_t min_bytes_per_expert() const {
        uint64_t best = 0;
        for (const StreamedLayer & L : layers_) {
            if (L.projs.empty() || L.bytes_per_expert == 0) continue;
            if (best == 0 || L.bytes_per_expert < best) best = L.bytes_per_expert;
        }
        return best;
    }
    uint64_t silicon_resident_bytes(uint64_t vram_bytes) const;
    double   ssd_bandwidth_mbs() const;
    double   preload_bandwidth_mbs() const { return preload_mbs_.load(std::memory_order_relaxed); }
    bool     mmap_released() const { return mmap_released_; }
    bool     has_layer(int32_t il) const {
        return il >= 0 && (size_t) il < layers_.size() && !layers_[(size_t) il].projs.empty();
    }

private:
    struct ShardHandle;
    struct IoJob {
        int32_t  il       = 0;
        int32_t  proj     = 0;
        int32_t  exp      = 0;
        int32_t  shard    = 0;
        uint64_t file_off = 0;
    };

    struct CacheSlotMeta {
        float    crf       = 0.0f;
        uint64_t last_step = 0;
        uint64_t pin_epoch = 0;
        bool     resident  = false;
        bool     in_vram   = false;
    };

    struct PrewarmReadAhead {
        int32_t              shard_idx = -1;
        uint64_t             base_off  = 0;
        uint64_t             valid_len = 0;
        std::vector<uint8_t> buf;
    };

    bool open_shards(std::string & err);
    void close_shards();
    bool pread_exact(int32_t shard_idx, void * dst, size_t len, uint64_t offset);
    int32_t find_proj_by_name(int32_t il, const char * tensor_name) const;
    void * prepare_vram_readback_dst(int32_t il, const char * tensor_name, int32_t exp, size_t nbytes, bool keep_in_vram);
    void   retire_completed_promotions_locked();

    bool evict_one_lowest_locked(uint64_t current_pin_epoch);
    bool commit_pages_with_recovery_locked(uint8_t * ptr, size_t bytes, uint64_t current_pin_epoch);
    bool commit_expert_locked(int32_t il, int32_t exp, uint64_t current_pin_epoch);
    void evict_expert_locked(int32_t il, int32_t exp, uint8_t post_state = 0);
    void evict_to_budget_locked(uint64_t needed_bytes, uint64_t current_pin_epoch);
    void worker_loop();

    size_t page_size_ = 4096;
    std::vector<std::string> shard_paths_;
    std::vector<std::unique_ptr<ShardHandle>> shards_;

    int32_t  n_layer_              = 0;
    int32_t  n_expert_             = 0;
    uint64_t budget_bytes_         = 0;
    uint64_t total_expert_bytes_   = 0;
    bool     sequential_hdd_mode_  = true;
    bool     disjoint_vram_mode_   = false;

    std::vector<StreamedLayer> layers_;
    std::unordered_map<const ggml_tensor *, std::pair<int32_t, int32_t>> tensor_to_proj_; // tensor -> (il, proj_idx)

    // Reusable 16 MiB sequential read-ahead buffer for zero-seek HDD prewarming
    PrewarmReadAhead prewarm_ra_;
    uint64_t         prewarm_disk_bytes_ = 0;
    uint64_t         prewarm_disk_ns_    = 0;

    std::function<bool(int32_t il, int32_t exp)> vram_rescue_fn_;

    // Anonymous dense storage
    uint8_t * dense_arena_ = nullptr;
    uint64_t  dense_bytes_ = 0;

    // Placeholder reservations for released Windows mappings
    std::vector<std::pair<void *, size_t>> win_placeholders_;
    bool mmap_released_ = false;

    // Cache metadata & async I/O queue
    mutable std::mutex      mu_;
    std::condition_variable io_cv_;
    std::condition_variable ready_cv_;
    std::deque<IoJob>       io_queue_;
    std::vector<std::thread> workers_;
    bool                    stop_workers_ = false;

    std::vector<CacheSlotMeta> slot_meta_; // [n_layer * n_expert]
    std::vector<std::pair<int32_t, int32_t>> pending_promoted_; // Deferred (il, exp) decommits after async DMA
    uint64_t                   step_clock_ = 1;
    uint64_t                   pin_epoch_  = 1;

    std::atomic<uint64_t> committed_bytes_{0};
    std::atomic<uint64_t> ram_hits_{0};
    std::atomic<uint64_t> ssd_reads_{0};
    std::atomic<uint64_t> ssd_bytes_read_{0};
    std::atomic<uint64_t> ssd_active_ns_{0};
    std::atomic<uint64_t> ram_to_vram_swaps_{0};
    std::atomic<uint64_t> vram_to_ram_swaps_{0};
    std::atomic<uint64_t> commit_failures_{0};
    std::atomic<double>   preload_mbs_{0.0};
};

} // namespace dualmoe
