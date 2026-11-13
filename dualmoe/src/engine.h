#pragma once

#include "dualmoe/types.h"
#include "gguf_inspect.h"
#include "virtual_arena.h"

#include "llama.h"

#include <atomic>
#include <mutex>
#include <string>
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

    bool load_model(const EngineConfig & cfg, std::string & err);
    void unload_model();
    bool is_loaded() const;

    EngineConfig current_config() const;
    ModelSummary current_model() const;
    TelemetrySnapshot telemetry() const;

    void request_cancel();

    // Thread-safe chat generation (OpenAI / LearnHarness / Studio UI)
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

private:
    static bool c_eval_callback(struct ggml_tensor * t, bool ask, void * user_data);
    static void c_moe_cache_callback(const struct llama_moe_cache_event * ev, void * user_data);
    static bool c_abort_callback(void * user_data);

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
    mutable std::mutex state_mu_;     // Protects config/summary/status strings

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

    // Atomic live counters
    std::atomic<bool>     loaded_{false};
    std::atomic<bool>     generating_{false};
    std::atomic<bool>     cancel_flag_{false};
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

    uint64_t active_vram_cache_mb_ = 0;
    bool     auto_split_active_    = false;
    bool     parallel_gpu_cpu_     = false;
    int32_t  active_gpu_layers_    = 0;
    float    active_cpu_share_     = 0.0f;
    std::string status_text_       = "Готов к загрузке модели";
};

} // namespace dualmoe
