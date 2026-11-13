#pragma once

#include "gguf_inspect.h"
#include "ggml.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
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
    int32_t       shard_idx   = 0;
    uint64_t      file_offset = 0;
    uint64_t      nb2         = 0;   // Bytes per expert along dim 2
    uint64_t      total_bytes = 0;
    uint8_t *     arena_base  = nullptr;
    uint64_t      arena_cap   = 0;
    std::vector<uint16_t> page_refs; // Per-OS-page reference count (prevents boundary eviction bugs)
    std::unique_ptr<std::atomic<uint8_t>[]> state; // 0 = EMPTY, 1 = LOADING, 2 = READY
};

struct StreamedLayer {
    int32_t                         il = -1;
    std::vector<StreamedProjection> projs;
    uint64_t                        bytes_per_expert = 0;
};

// Virtual-Memory Expert Arena + Decayed-Frequency DDR4 Cache + Multi-Lane SSD Streamer
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
              std::string & err);

    void shutdown();

    // Bind a captured 3D expert tensor into its own uncommitted virtual address reservation
    bool bind_layer_projection(int32_t il,
                               ggml_tensor * tensor,
                               int32_t shard_idx,
                               uint64_t file_offset,
                               uint64_t nbytes,
                               std::string & err);

    // Rehome host dense weights into anonymous RAM so GGUF file mappings can be safely dropped
    bool rehome_dense_weights(const std::vector<DenseRehomeEntry> & entries, std::string & err);

    // Drop OS file mappings of the GGUF shards and reopen clean positioned readers
    bool release_file_mappings(const std::vector<const void *> & check_host_ptrs);

    // Synchronous load (for GPU VRAM cache fills that are about to be DMA-uploaded)
    void ensure_experts_sync(int32_t il, const int32_t * experts, size_t count);

    // Asynchronous multi-lane SSD prefetch (for parallel CPU misses & route-ahead)
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
    double   ssd_bandwidth_mbs() const;
    bool     mmap_released() const { return mmap_released_; }
    bool     has_layer(int32_t il) const {
        return il >= 0 && (size_t) il < layers_.size() && !layers_[(size_t) il].projs.empty();
    }

private:
    struct ShardHandle;
    struct IoJob {
        int32_t il    = 0;
        int32_t proj  = 0;
        int32_t exp   = 0;
    };

    struct CacheSlotMeta {
        float    crf       = 0.0f;
        uint64_t last_step = 0;
        uint64_t pin_epoch = 0;
        bool     resident  = false;
    };

    bool open_shards(std::string & err);
    void close_shards();
    bool pread_exact(int32_t shard_idx, void * dst, size_t len, uint64_t offset);

    void commit_expert_locked(int32_t il, int32_t exp);
    void evict_expert_locked(int32_t il, int32_t exp);
    void evict_to_budget_locked(uint64_t needed_bytes, uint64_t current_pin_epoch);
    void worker_loop();

    size_t page_size_ = 4096;
    std::vector<std::string> shard_paths_;
    std::vector<std::unique_ptr<ShardHandle>> shards_;

    int32_t  n_layer_      = 0;
    int32_t  n_expert_     = 0;
    uint64_t budget_bytes_ = 0;

    std::vector<StreamedLayer> layers_;
    std::unordered_map<const ggml_tensor *, std::pair<int32_t, int32_t>> tensor_to_proj_; // tensor -> (il, proj_idx)

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
    uint64_t                   step_clock_ = 1;
    uint64_t                   pin_epoch_  = 1;

    std::atomic<uint64_t> committed_bytes_{0};
    std::atomic<uint64_t> ram_hits_{0};
    std::atomic<uint64_t> ssd_reads_{0};
    std::atomic<uint64_t> ssd_bytes_read_{0};
    std::atomic<uint64_t> ssd_active_ns_{0};
};

} // namespace dualmoe
