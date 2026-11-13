#pragma once

#include "dualmoe/types.h"
#include "gguf_inspect.h"
#include "virtual_arena.h"

#include "llama.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace dualmoe {

class DualMoeEngine {
public:
    DualMoeEngine();
    ~DualMoeEngine();

    DualMoeEngine(const DualMoeEngine &) = delete;
    DualMoeEngine & operator=(const DualMoeEngine &) = delete;

    std::vector<DeviceInfo> list_devices() const;
    SystemMemoryInfo query_system_memory() const;

    // Synchronous load (used by CLI & deterministic unit tests)
    bool load_model(const EngineConfig & cfg, std::string & err);

    // Asynchronous background load (used by Web UI for real-time step-by-step streaming)
    bool start_load_async(const EngineConfig & cfg, std::string & err);
    bool is_loading() const { return loading_.load(std::memory_order_acquire); }

    void unload_model();
    bool is_loaded() const;

    EngineConfig current_config() const;
    ModelSummary current_model() const;
    TelemetrySnapshot telemetry() const;
    LoadDiagnosticsSnapshot diagnostics() const;
    std::vector<LoadLogEntry> recent_logs(uint64_t since_seq = 0) const;
    void clear_crash_banner();

    void request_cancel();

    // Thread-safe chat generation (OpenAI / LearnHarness / Hermes Studio UI)
    bool generate_chat(const std::vector<ChatMessage> & messages,
                       const SamplingParams & sampling_override,
                       bool use_override,
                       const TokenCallback & on_token,
                       std::string & full_reply,
                       int32_t & prompt_tokens_out,
                       int32_t & completion_tokens_out,
                       std::string & err);

    // Raw prompt generation (used by deterministic byte-identity tests & CLI)
    bool generate_raw(const std::string & prompt,
                      int32_t max_tokens,
                      float temperature,
                      uint32_t seed,
                      const TokenCallback & on_token,
                      std::string & full_reply,
                      std::string & err);

    // Internal hook for llama_log_set & crash handlers
    void append_backend_log(int level, const char * text);

private:
    static bool   c_eval_callback    (struct ggml_tensor * t, bool ask, void * user_data);
    static void   c_moe_cache_callback(const struct llama_moe_cache_event * ev, void * user_data);
    static bool   c_moe_prewarm_fetch(int32_t il, int32_t ip, int32_t expert_id, const char * tensor_name, void * dst_buf, size_t nbytes, void * user_data);
    static void * c_moe_evict_cb     (int32_t evicted_il, int32_t ip, int32_t evicted_expert, const char * tensor_name, size_t nbytes, void * user_data);
    static void   c_moe_promoted_cb  (int32_t il, const int32_t * promoted_experts, size_t n_promoted, size_t n_evicted_to_ram, void * user_data);
    static bool   c_abort_callback   (void * user_data);

    void unload_model_locked();
    bool load_model_impl(const EngineConfig & cfg, std::string & err);

    void init_load_steps(const EngineConfig & cfg);
    void begin_step(int32_t idx, const std::string & detail);
    void finish_step(int32_t idx, const std::string & state, const std::string & detail);
    void add_warning(const std::string & msg);
    void write_flight_line(const std::string & line, bool flush_now = true);
    void mark_flight_header(const std::string & status_tag, const std::string & stage_desc);
    void inspect_previous_session_logs();

    bool on_eval_node(struct ggml_tensor * t, bool ask);
    void on_moe_cache_event(const struct llama_moe_cache_event * ev);

    std::string format_chat_prompt(const std::vector<ChatMessage> & messages) const;
    bool run_tokens(const std::vector<llama_token> & tokens,
                    const SamplingParams & sp,
                    const TokenCallback & on_token,
                    std::string & full_reply,
                    int32_t & prompt_tokens_out,
                    int32_t & completion_tokens_out,
                    std::string & err);

    mutable std::mutex api_mu_;       // Serializes load/unload/generate turns safely
    mutable std::mutex state_mu_;     // Protects config/summary/diagnostics/status strings
    mutable std::mutex log_mu_;       // Protects log ring buffer & flight-recorder file

    std::thread async_load_thread_;

    EngineConfig cfg_;
    GgufLayout   layout_;
    VirtualExpertArena arena_;

    llama_model *   model_ = nullptr;
    llama_context * ctx_   = nullptr;
    const llama_vocab * vocab_ = nullptr;

    // Prefix KV cache tracking across turns
    std::vector<llama_token> kv_tokens_;

    // Capture-phase state
    bool capture_mode_ = false;
    std::unordered_set<ggml_tensor *> captured_weights_;
    std::vector<std::vector<ggml_tensor *>> captured_layer_experts_;
    std::vector<bool> layer_on_gpu_cache_;
    std::vector<std::vector<int32_t>> prev_route_;

    // Diagnostics & crash forensics state
    std::string log_file_path_;
    std::string crash_file_path_;
    LoadDiagnosticsSnapshot diag_;
    std::chrono::steady_clock::time_point step_t0_;
    std::deque<LoadLogEntry> log_ring_;
    uint64_t next_log_seq_ = 1;

    // Atomic live counters
    std::atomic<bool>     loaded_{false};
    std::atomic<bool>     loading_{false};
    std::atomic<bool>     generating_{false};
    std::atomic<bool>     cancel_flag_{false};
    std::atomic<int32_t>  load_progress_pct_{0};
    std::atomic<int32_t>  load_phase_index_{0};

    std::atomic<double>   prefill_tps_{0.0};
    std::atomic<double>   decode_tps_{0.0};
    std::atomic<double>   ttft_ms_{0.0};
    std::atomic<uint64_t> prompt_tokens_{0};
    std::atomic<uint64_t> generated_tokens_{0};
    std::atomic<uint32_t> kv_used_{0};

    std::atomic<uint64_t> gpu_expert_hits_{0};
    std::atomic<uint64_t> gpu_expert_fills_{0};
    std::atomic<uint64_t> cpu_parallel_experts_{0};
    std::atomic<uint64_t> vram_upload_bytes_{0};

    uint64_t active_vram_cache_mb_  = 0;
    uint64_t vram_prewarmed_bytes_  = 0;
    uint64_t ram_preloaded_bytes_   = 0;
    bool     auto_split_active_     = false;
    bool     parallel_gpu_cpu_      = false;
    bool     zero_hdd_active_       = false;
    bool     disjoint_swap_active_  = false;
    bool     auto_balance_active_   = false;
    int32_t  active_gpu_layers_     = 0;
    std::atomic<float> active_cpu_share_{0.0f};
    float    ema_gpu_hit_ratio_     = 0.75f;
    uint64_t balance_events_        = 0;
    uint32_t active_ubatch_         = 32;
    std::string status_text_        = "Готов к загрузке модели";
};

} // namespace dualmoe
