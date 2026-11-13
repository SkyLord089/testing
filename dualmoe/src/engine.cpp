#include "engine.h"

#include "ggml-backend.h"
#include "ggml-cpu.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unordered_map>

namespace dualmoe {

namespace {

ggml_type parse_kv_type(const std::string & s) {
    if (s == "q8_0" || s == "Q8_0") return GGML_TYPE_Q8_0;
    if (s == "q4_0" || s == "Q4_0") return GGML_TYPE_Q4_0;
    return GGML_TYPE_F16;
}

bool parse_topk_node_layer(const char * name, int32_t & il_out) {
    if (!name) return false;
    static constexpr const char * kPrefix = "ffn_moe_topk-";
    static constexpr size_t kLen = 13;
    if (std::strncmp(name, kPrefix, kLen) != 0) return false;
    char * endp = nullptr;
    const long v = std::strtol(name + kLen, &endp, 10);
    if (!endp || *endp != '\0' || v < 0) return false;
    il_out = (int32_t) v;
    return true;
}

void install_cpu_expert_hook(ggml_cpu_expert_ready_hook_t fn, void * ud) {
    // Try dynamic backend registry first (works for both static and DLL-modular builds)
    ggml_backend_reg_t reg = ggml_backend_reg_by_name("CPU");
    if (reg) {
        using setter_t = void (*)(ggml_cpu_expert_ready_hook_t, void *);
        auto * dyn = reinterpret_cast<setter_t>(
            ggml_backend_reg_get_proc_address(reg, "ggml_cpu_set_expert_ready_hook"));
        if (dyn) {
            dyn(fn, ud);
            return;
        }
    }
#if !defined(GGML_BACKEND_DL)
    ggml_cpu_set_expert_ready_hook(fn, ud);
#endif
}

// How many trailing bytes of `s` form an incomplete UTF-8 multi-byte sequence
size_t incomplete_utf8_tail(const std::string & s) {
    if (s.empty()) return 0;
    const size_t n = s.size();
    size_t back = 0;
    while (back < n && back < 4 && ((uint8_t) s[n - 1 - back] & 0xC0u) == 0x80u) {
        ++back;
    }
    if (back >= n || back >= 4) return 0;
    const uint8_t lead = (uint8_t) s[n - 1 - back];
    size_t expected = 1;
    if ((lead & 0x80u) == 0x00u) expected = 1;
    else if ((lead & 0xE0u) == 0xC0u) expected = 2;
    else if ((lead & 0xF0u) == 0xE0u) expected = 3;
    else if ((lead & 0xF8u) == 0xF0u) expected = 4;
    else return 0;

    const size_t have = back + 1;
    return have < expected ? have : 0;
}

void quiet_llama_log(enum ggml_log_level level, const char * text, void *) {
    if (!text) return;
    if (level == GGML_LOG_LEVEL_WARN || level == GGML_LOG_LEVEL_ERROR) {
        std::fputs(text, stderr);
    }
}

} // namespace

DualMoeEngine::DualMoeEngine() {
    if (!std::getenv("DUALMOE_VERBOSE")) {
        llama_log_set(quiet_llama_log, nullptr);
    }
    ggml_backend_load_all();
    llama_backend_init();
}

DualMoeEngine::~DualMoeEngine() {
    unload_model();
    llama_backend_free();
}

std::vector<DeviceInfo> DualMoeEngine::list_devices() const {
    std::vector<DeviceInfo> out;
    const size_t n = ggml_backend_dev_count();
    for (size_t i = 0; i < n; ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        if (!dev) continue;
        DeviceInfo info;
        info.name        = ggml_backend_dev_name(dev) ? ggml_backend_dev_name(dev) : "Device";
        info.description = ggml_backend_dev_description(dev) ? ggml_backend_dev_description(dev) : "";
        ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(dev);
        info.backend     = (reg && ggml_backend_reg_name(reg)) ? ggml_backend_reg_name(reg) : "";
        const auto dt    = ggml_backend_dev_type(dev);
        info.is_gpu      = (dt == GGML_BACKEND_DEVICE_TYPE_GPU || dt == GGML_BACKEND_DEVICE_TYPE_IGPU);
        size_t free_b = 0, total_b = 0;
        ggml_backend_dev_memory(dev, &free_b, &total_b);
        info.free_mb  = (uint64_t) (free_b >> 20);
        info.total_mb = (uint64_t) (total_b >> 20);
        out.push_back(std::move(info));
    }
    return out;
}

bool DualMoeEngine::is_loaded() const {
    return loaded_.load(std::memory_order_acquire);
}

EngineConfig DualMoeEngine::current_config() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return cfg_;
}

ModelSummary DualMoeEngine::current_model() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return layout_.summary;
}

void DualMoeEngine::request_cancel() {
    cancel_flag_.store(true, std::memory_order_release);
}

void DualMoeEngine::unload_model() {
    std::lock_guard<std::mutex> lk(api_mu_);
    install_cpu_expert_hook(nullptr, nullptr);
    if (ctx_) {
        llama_set_moe_cache_host_cb(ctx_, nullptr, nullptr);
        llama_free(ctx_);
        ctx_ = nullptr;
    }
    // Note: VirtualExpertArena shuts down BEFORE llama_model_free on Windows so placeholder
    // reservations at the unmapped file view address are released cleanly before ~llama_mmap.
    arena_.shutdown();
    if (model_) {
        llama_model_free(model_);
        model_ = nullptr;
    }
    vocab_ = nullptr;
    kv_tokens_.clear();
    captured_weights_.clear();
    captured_layer_experts_.clear();
    layer_on_gpu_cache_.clear();
    prev_route_.clear();

    loaded_.store(false, std::memory_order_release);
    generating_.store(false, std::memory_order_release);
    kv_used_.store(0, std::memory_order_relaxed);
    gpu_expert_hits_.store(0, std::memory_order_relaxed);
    gpu_expert_fills_.store(0, std::memory_order_relaxed);
    cpu_parallel_experts_.store(0, std::memory_order_relaxed);
    vram_upload_bytes_.store(0, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> slk(state_mu_);
        layout_ = GgufLayout{};
        active_vram_cache_mb_ = 0;
        auto_split_active_    = false;
        parallel_gpu_cpu_     = false;
        active_gpu_layers_    = 0;
        active_cpu_share_     = 0.0f;
        status_text_          = "Готов к загрузке модели";
    }
}

bool DualMoeEngine::load_model(const EngineConfig & input_cfg, std::string & err) {
    unload_model();
    std::lock_guard<std::mutex> lk(api_mu_);

    {
        std::lock_guard<std::mutex> slk(state_mu_);
        status_text_ = "Анализ заголовка GGUF и структуры экспертов...";
    }

    GgufLayout lay = inspect_gguf_layout(input_cfg.model_path);
    if (!lay.summary.ok) {
        err = lay.summary.error;
        std::lock_guard<std::mutex> slk(state_mu_);
        status_text_ = "Ошибка: " + err;
        return false;
    }

    // Check if this machine actually has a GPU backend registered
    bool has_gpu_device = false;
    for (const DeviceInfo & d : list_devices()) {
        if (d.is_gpu) {
            has_gpu_device = true;
            break;
        }
    }

    EngineConfig cfg = input_cfg;
    const int32_t effective_ngl = has_gpu_device ? std::max<int32_t>(0, cfg.n_gpu_layers) : 0;
    const bool    stream_moe    = cfg.moe_stream && lay.summary.is_moe;
    const bool    split_experts = lay.summary.is_moe && effective_ngl > 0 &&
                                  (cfg.auto_split_experts || stream_moe || cfg.vram_cache_mb > 0);

    llama_model_params mparams = llama_model_default_params();
    ggml_backend_dev_t cpu_only_devs[1] = {nullptr};
    if (effective_ngl == 0) {
        mparams.devices = cpu_only_devs;
    }
    mparams.load_mode       = LLAMA_LOAD_MODE_MMAP;
    mparams.use_extra_bufts = false; // Crucial: keep native GGUF layout so virtual arena rebinds cleanly
    mparams.lazy_mode       = stream_moe ? LLAMA_LAZY_MODE_OFF : LLAMA_LAZY_MODE_AUTO;
    mparams.n_gpu_layers    = effective_ngl;

    // Prevent >=2 GPU layer crashes on 8 GB cards: keep all 3D routed expert banks in host memory
    // while Attention, Norms, Router, Shared Experts, and Output Head live in fast GPU VRAM!
    std::vector<llama_model_tensor_buft_override> overrides;
    static const char * kRoutedExpsRegex = "\\.ffn_(gate|up|down|gate_up)_exps\\.weight";
    if (split_experts) {
        overrides.push_back({kRoutedExpsRegex, ggml_backend_cpu_buffer_type()});
        overrides.push_back({nullptr, nullptr});
        mparams.tensor_buft_overrides = overrides.data();
    }

    llama_model_kv_override kv_ov[2]{};
    std::string used_key;
    if (cfg.n_expert_used_override > 0 && lay.summary.is_moe && !lay.summary.arch.empty()) {
        used_key = lay.summary.arch + ".expert_used_count";
        kv_ov[0].tag = LLAMA_KV_OVERRIDE_TYPE_INT;
        std::snprintf(kv_ov[0].key, sizeof(kv_ov[0].key), "%s", used_key.c_str());
        kv_ov[0].val_i64 = std::min<int32_t>(cfg.n_expert_used_override, lay.summary.n_expert);
        kv_ov[1].key[0] = '\0';
        mparams.kv_overrides = kv_ov;
    }

    {
        std::lock_guard<std::mutex> slk(state_mu_);
        status_text_ = "Отображение модели и размещение слоёв...";
    }

    model_ = llama_model_load_from_file(cfg.model_path.c_str(), mparams);
    if (!model_) {
        err = "llama.cpp не смог загрузить модель: " + cfg.model_path;
        std::lock_guard<std::mutex> slk(state_mu_);
        status_text_ = "Ошибка загрузки модели";
        return false;
    }
    vocab_ = llama_model_get_vocab(model_);
    const int32_t n_layer = llama_model_n_layer(model_);

    auto make_base_cparams = [&]() {
        llama_context_params cp = llama_context_default_params();
        cp.n_ctx           = cfg.n_ctx > 0 ? cfg.n_ctx : 8192;
        cp.n_batch         = std::max<uint32_t>(32, cfg.n_batch);
        cp.n_ubatch        = std::clamp<uint32_t>(cfg.n_ubatch > 0 ? cfg.n_ubatch : 256, 32, cp.n_batch);
        cp.n_threads       = std::max<int32_t>(1, cfg.n_threads);
        cp.n_threads_batch = std::max<int32_t>(1, cfg.n_threads_batch > 0 ? cfg.n_threads_batch : cfg.n_threads);
        cp.n_outputs_max   = 32; // Saves 1-2 GiB of wasted logit scratch buffer!
        cp.type_k          = parse_kv_type(cfg.kv_type_k);
        cp.type_v          = parse_kv_type(cfg.kv_type_v);
        cp.offload_kqv     = (effective_ngl > 0);
        // Disable opportunistic host-matmul offload when streaming so wide batches never copy uncommitted pages
        cp.op_offload      = !stream_moe && (effective_ngl > 0);
        cp.cb_eval           = &DualMoeEngine::c_eval_callback;
        cp.cb_eval_user_data = this;
        cp.abort_callback      = &DualMoeEngine::c_abort_callback;
        cp.abort_callback_data = this;
        return cp;
    };

    // Stage 1: If streaming MoE, run 1-token capture pass BEFORE arming moe_cache so we discover
    // all live host ggml_tensor* pointers, bind them to VirtualExpertArena, and release OS mmap!
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        cfg_    = cfg;
        layout_ = lay;
    }
    captured_weights_.clear();
    captured_layer_experts_.assign((size_t) std::max<int32_t>(1, n_layer), {});
    prev_route_.assign((size_t) std::max<int32_t>(1, n_layer), {});
    layer_on_gpu_cache_.assign((size_t) std::max<int32_t>(1, n_layer), false);

    if (stream_moe) {
        {
            std::lock_guard<std::mutex> slk(state_mu_);
            status_text_ = "Захват графа экспертов и настройка виртуальной арены DDR4...";
        }

        llama_context_params cap_cp = make_base_cparams();
        cap_cp.n_ctx          = std::min<uint32_t>(cap_cp.n_ctx, 512);
        cap_cp.n_batch        = 32;
        cap_cp.n_ubatch       = 32;
        cap_cp.moe_cache_size = 0;

        llama_context * cap_ctx = llama_init_from_model(model_, cap_cp);
        if (!cap_ctx && ggml_is_quantized(cap_cp.type_v)) {
            cap_cp.type_v = GGML_TYPE_F16;
            cfg.kv_type_v = "f16";
            cap_ctx = llama_init_from_model(model_, cap_cp);
        }
        if (!cap_ctx) {
            err = "Не удалось создать контекст захвата графа";
            llama_model_free(model_);
            model_ = nullptr;
            return false;
        }

        capture_mode_ = true;
        llama_token warm_tok = llama_vocab_bos(vocab_);
        if (warm_tok < 0) warm_tok = 0;
        llama_batch warm = llama_batch_get_one(&warm_tok, 1);
        const int rc = llama_decode(cap_ctx, warm);
        capture_mode_ = false;
        llama_free(cap_ctx);

        if (rc != 0) {
            err = "Пробный прогон захвата графа завершился с ошибкой";
            llama_model_free(model_);
            model_ = nullptr;
            return false;
        }

        const size_t ram_budget = cfg.ram_cache_mb > 0 ? (cfg.ram_cache_mb << 20) : (4096ull << 20);
        if (!arena_.init(lay.summary.shard_paths, n_layer, lay.summary.n_expert, ram_budget, cfg.io_lanes, err)) {
            llama_model_free(model_);
            model_ = nullptr;
            return false;
        }

        std::unordered_set<std::string> bound_expert_names;
        for (int32_t il = 0; il < n_layer; ++il) {
            for (ggml_tensor * et : captured_layer_experts_[(size_t) il]) {
                const auto it = lay.tensors.find(et->name);
                if (it == lay.tensors.end()) continue;
                if (!arena_.bind_layer_projection(il, et, it->second.shard_idx,
                                                  it->second.file_offset, it->second.nbytes, err)) {
                    arena_.shutdown();
                    llama_model_free(model_);
                    model_ = nullptr;
                    return false;
                }
                bound_expert_names.insert(et->name);
            }
        }

        if (cfg.dense_anon) {
            std::vector<DenseRehomeEntry> dense_list;
            std::unordered_map<uint64_t, size_t> seen_key;
            for (ggml_tensor * wt : captured_weights_) {
                if (!wt) continue;
                // Strictly skip GPU/device buffers — touching device tensor->data crashes Vulkan!
                if (wt->buffer && !ggml_backend_buffer_is_host(wt->buffer)) continue;
                const std::string tname = wt->name;
                if (bound_expert_names.count(tname)) continue;
                const auto it = lay.tensors.find(tname);
                if (it == lay.tensors.end()) continue;

                const uint64_t key = ((uint64_t) (uint32_t) it->second.shard_idx << 48) ^ it->second.file_offset;
                const auto sit = seen_key.find(key);
                if (sit != seen_key.end()) {
                    dense_list[sit->second].aliases.push_back(wt);
                    continue;
                }
                DenseRehomeEntry de;
                de.primary     = wt;
                de.shard_idx   = it->second.shard_idx;
                de.file_offset = it->second.file_offset;
                de.nbytes      = it->second.nbytes;
                seen_key.emplace(key, dense_list.size());
                dense_list.push_back(std::move(de));
            }
            if (!arena_.rehome_dense_weights(dense_list, err)) {
                arena_.shutdown();
                llama_model_free(model_);
                model_ = nullptr;
                return false;
            }
        }

        if (cfg.release_mmap && cfg.dense_anon) {
            std::vector<const void *> host_ptrs;
            for (ggml_tensor * wt : captured_weights_) {
                if (!wt) continue;
                if (wt->buffer && !ggml_backend_buffer_is_host(wt->buffer)) continue;
                if (wt->data) host_ptrs.push_back(wt->data);
            }
            arena_.release_file_mappings(host_ptrs);
        }

        install_cpu_expert_hook(&VirtualExpertArena::c_expert_ready_hook, &arena_);
    }

    // Stage 2: Create the production context with parallel GPU ClassLRU cache + CPU share!
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        status_text_ = "Запуск параллельного планировщика GPU + CPU...";
    }

    llama_context_params cparams = make_base_cparams();
    const bool want_gpu_moe_cache = lay.summary.is_moe && effective_ngl > 0 && cfg.vram_cache_mb > 0;
    if (want_gpu_moe_cache) {
        cparams.moe_cache_size      = cfg.vram_cache_mb << 20;
        cparams.moe_cache_cpu_share = std::clamp(cfg.cpu_share, 0.0f, 1.0f);
        // Mark GPU-cached layers BEFORE llama_init_from_model so sched_reserve preserves async GPU||CPU splits
        const int32_t gpu_block_count = effective_ngl > n_layer ? n_layer : std::max<int32_t>(0, effective_ngl - 1);
        const int32_t first_gpu_il    = std::max<int32_t>(0, n_layer - gpu_block_count);
        for (int32_t il = first_gpu_il; il < n_layer; ++il) {
            layer_on_gpu_cache_[(size_t) il] = true;
        }
    }

    ctx_ = llama_init_from_model(model_, cparams);
    if (!ctx_ && ggml_is_quantized(cparams.type_v)) {
        cparams.type_v = GGML_TYPE_F16;
        cfg.kv_type_v  = "f16";
        ctx_ = llama_init_from_model(model_, cparams);
    }
    if (!ctx_ && ggml_is_quantized(cparams.type_k)) {
        cparams.type_k = GGML_TYPE_F16;
        cfg.kv_type_k  = "f16";
        ctx_ = llama_init_from_model(model_, cparams);
    }
    // Automatic VRAM safety valve: step down moe_cache_size by 25% if VRAM is tighter than requested
    if (!ctx_ && cparams.moe_cache_size > 0) {
        const size_t floor_b = std::max<size_t>((size_t) lay.summary.cycle_floor_mb << 20, 64ull << 20);
        size_t cur_sz = cparams.moe_cache_size;
        while (!ctx_ && (cur_sz * 3) / 4 >= floor_b) {
            cur_sz = ((cur_sz * 3) / 4) & ~((1ull << 20) - 1);
            cparams.moe_cache_size = cur_sz;
            ctx_ = llama_init_from_model(model_, cparams);
        }
        if (!ctx_) {
            cparams.moe_cache_size = 0;
            layer_on_gpu_cache_.assign((size_t) std::max<int32_t>(1, n_layer), false);
            ctx_ = llama_init_from_model(model_, cparams);
        }
    }

    if (!ctx_) {
        err = "Не удалось выделить вычислительный контекст (проверьте размер контекста и свободный VRAM/RAM)";
        install_cpu_expert_hook(nullptr, nullptr);
        arena_.shutdown();
        llama_model_free(model_);
        model_ = nullptr;
        return false;
    }

    if (cparams.moe_cache_size > 0 && effective_ngl > 0) {
        llama_set_moe_cache_host_cb(ctx_, &DualMoeEngine::c_moe_cache_callback, this);
    }

    {
        std::lock_guard<std::mutex> slk(state_mu_);
        cfg_                  = cfg;
        layout_               = std::move(lay);
        active_vram_cache_mb_ = (uint64_t) (cparams.moe_cache_size >> 20);
        auto_split_active_    = split_experts;
        parallel_gpu_cpu_     = (cparams.moe_cache_size > 0 && cparams.moe_cache_cpu_share > 0.0f);
        active_gpu_layers_    = effective_ngl;
        active_cpu_share_     = cparams.moe_cache_size > 0 ? cparams.moe_cache_cpu_share : 1.0f;
        status_text_          = parallel_gpu_cpu_
            ? "Активен: Параллельный режим GPU (VRAM) ∥ CPU (DDR4) ∥ SSD"
            : (stream_moe ? "Активен: Стриминг экспертов DDR4 ∥ SSD" : "Активен: Модель загружена");
    }

    loaded_.store(true, std::memory_order_release);
    return true;
}

bool DualMoeEngine::c_eval_callback(struct ggml_tensor * t, bool ask, void * user_data) {
    if (!user_data) return true;
    return static_cast<DualMoeEngine *>(user_data)->on_eval_node(t, ask);
}

void DualMoeEngine::c_moe_cache_callback(const struct llama_moe_cache_event * ev, void * user_data) {
    if (!user_data || !ev) return;
    static_cast<DualMoeEngine *>(user_data)->on_moe_cache_event(ev);
}

bool DualMoeEngine::c_abort_callback(void * user_data) {
    if (!user_data) return false;
    return static_cast<DualMoeEngine *>(user_data)->cancel_flag_.load(std::memory_order_acquire);
}

void DualMoeEngine::on_moe_cache_event(const struct llama_moe_cache_event * ev) {
    gpu_expert_hits_.fetch_add(ev->n_hits, std::memory_order_relaxed);
    gpu_expert_fills_.fetch_add(ev->n_fills, std::memory_order_relaxed);
    cpu_parallel_experts_.fetch_add(ev->n_cpu, std::memory_order_relaxed);
    vram_upload_bytes_.fetch_add(ev->upload_bytes, std::memory_order_relaxed);

    if (cfg_.moe_stream && arena_.has_layer(ev->il)) {
        // 1. Ensure experts being uploaded into GPU VRAM bank are committed & resident right now
        if (ev->n_fills > 0 && ev->fill_experts) {
            arena_.ensure_experts_sync(ev->il, ev->fill_experts, ev->n_fills);
        }
        // 2. Non-blocking async SSD prefetch for CPU-assigned misses:
        //    GPU starts its cached expert split IMMEDIATELY while SSD lanes + CPU threads run concurrently!
        if (ev->n_cpu > 0 && ev->cpu_experts) {
            arena_.prefetch_experts_async(ev->il, ev->cpu_experts, ev->n_cpu);
        }
    }
}

bool DualMoeEngine::on_eval_node(struct ggml_tensor * t, bool ask) {
    if (!t) return !ask;

    if (capture_mode_) {
        if (ask) {
            for (int s = 0; s < GGML_MAX_SRC; ++s) {
                ggml_tensor * src = t->src[s];
                if (!src) continue;
                ggml_tensor * leaf = src;
                while (leaf->view_src) leaf = leaf->view_src;
                if (leaf->op == GGML_OP_NONE) {
                    captured_weights_.insert(leaf);
                }
                const auto it = layout_.tensors.find(src->name);
                if (it != layout_.tensors.end() && it->second.is_expert && it->second.layer_idx >= 0) {
                    const bool host_ok = !src->buffer || ggml_backend_buffer_is_host(src->buffer);
                    if (host_ok && (size_t) it->second.layer_idx < captured_layer_experts_.size()) {
                        auto & vec = captured_layer_experts_[(size_t) it->second.layer_idx];
                        if (std::find(vec.begin(), vec.end(), src) == vec.end()) {
                            vec.push_back(src);
                        }
                    }
                }
            }
            return false; // Capture only observes, never splits the graph
        }
        return true;
    }

    // Production phase:
    // If this layer is served by llama_moe_cache on GPU, DO NOT isolate any node (`return false`)!
    // Combined with our ggml-backend.cpp `if (need)` fix, this preserves 100% async GPU || CPU execution!
    int32_t il = -1;
    if (!parse_topk_node_layer(t->name, il)) {
        return ask ? false : true;
    }
    if (il >= 0 && (size_t) il < layer_on_gpu_cache_.size() && layer_on_gpu_cache_[(size_t) il]) {
        return ask ? false : true;
    }

    if (ask) {
        return true; // Isolate ffn_moe_topk-<il> for CPU-only / non-GPU-cached layers
    }

    // Non-ask callback: ffn_moe_topk-<il> has materialized selected expert IDs!
    const int64_t n_used   = t->ne[0];
    const int64_t n_tokens = t->ne[1];
    if (n_used <= 0 || n_tokens <= 0) return true;

    std::vector<int32_t> selected;
    selected.reserve((size_t) (n_used * n_tokens));

    const bool on_dev = t->buffer && !ggml_backend_buffer_is_host(t->buffer);
    if (!on_dev && t->data) {
        const uint8_t * base = static_cast<const uint8_t *>(t->data);
        for (int64_t it = 0; it < n_tokens; ++it) {
            for (int64_t k = 0; k < n_used; ++k) {
                int32_t id = 0;
                std::memcpy(&id, base + it * t->nb[1] + k * t->nb[0], sizeof(int32_t));
                if (id >= 0) selected.push_back(id);
            }
        }
    } else if (ggml_is_contiguous(t)) {
        selected.resize((size_t) (n_used * n_tokens));
        ggml_backend_tensor_get(t, selected.data(), 0, selected.size() * sizeof(int32_t));
    }

    cpu_parallel_experts_.fetch_add(selected.size(), std::memory_order_relaxed);

    if (cfg_.moe_stream && arena_.has_layer(il) && !selected.empty()) {
        arena_.prefetch_experts_async(il, selected.data(), selected.size());
        if ((size_t) il < prev_route_.size()) {
            prev_route_[(size_t) il] = selected;
        }
        // Route-ahead: warm next CPU layer's previous-token experts on idle SSD lanes
        if (cfg_.prefetch_ahead > 0 && n_tokens == 1) {
            const int32_t next_il = il + 1;
            if ((size_t) next_il < prev_route_.size() && arena_.has_layer(next_il) &&
                !prev_route_[(size_t) next_il].empty()) {
                arena_.prefetch_experts_async(next_il, prev_route_[(size_t) next_il].data(),
                                              prev_route_[(size_t) next_il].size());
            }
        }
    }
    return true;
}

std::string DualMoeEngine::format_chat_prompt(const std::vector<ChatMessage> & messages) const {
    if (messages.empty()) return "";
    std::vector<llama_chat_message> raw;
    raw.reserve(messages.size());
    size_t total_chars = 0;
    for (const auto & m : messages) {
        raw.push_back({m.role.c_str(), m.content.c_str()});
        total_chars += m.role.size() + m.content.size() + 32;
    }

    const char * tmpl = model_ ? llama_model_chat_template(model_, nullptr) : nullptr;
    std::vector<char> buf(std::max<size_t>(1024, total_chars * 2 + 256));
    int32_t res = llama_chat_apply_template(tmpl, raw.data(), raw.size(), true, buf.data(), (int32_t) buf.size());
    if (res > (int32_t) buf.size()) {
        buf.resize((size_t) res + 64);
        res = llama_chat_apply_template(tmpl, raw.data(), raw.size(), true, buf.data(), (int32_t) buf.size());
    }
    if (res > 0) {
        return std::string(buf.data(), (size_t) res);
    }

    // Clean ChatML fallback if model carries an unrecognized Jinja template
    std::ostringstream oss;
    for (const auto & m : messages) {
        oss << "<|im_start|>" << m.role << "\n" << m.content << "<|im_end|>\n";
    }
    oss << "<|im_start|>assistant\n";
    return oss.str();
}

bool DualMoeEngine::generate_chat(const std::vector<ChatMessage> & messages,
                                  const SamplingParams & sampling_override,
                                  bool use_override,
                                  const TokenCallback & on_token,
                                  std::string & full_reply,
                                  int32_t & prompt_tokens_out,
                                  int32_t & completion_tokens_out,
                                  std::string & err) {
    std::lock_guard<std::mutex> lk(api_mu_);
    if (!loaded_.load(std::memory_order_acquire) || !ctx_ || !vocab_) {
        err = "Модель ещё не загружена в движок";
        return false;
    }

    const std::string prompt = format_chat_prompt(messages);
    int32_t n_tok = -llama_tokenize(vocab_, prompt.c_str(), (int32_t) prompt.size(), nullptr, 0, true, true);
    if (n_tok <= 0) {
        err = "Не удалось токенизировать запрос";
        return false;
    }
    std::vector<llama_token> tokens((size_t) n_tok);
    const int32_t got = llama_tokenize(vocab_, prompt.c_str(), (int32_t) prompt.size(),
                                       tokens.data(), (int32_t) tokens.size(), true, true);
    if (got < 0) {
        err = "Ошибка буфера токенизатора";
        return false;
    }
    tokens.resize((size_t) got);

    const SamplingParams sp = use_override ? sampling_override : cfg_.sampling;
    return run_tokens(tokens, sp, on_token, full_reply, prompt_tokens_out, completion_tokens_out, err);
}

bool DualMoeEngine::generate_raw(const std::string & prompt,
                                 int32_t max_tokens,
                                 float temperature,
                                 uint32_t seed,
                                 const TokenCallback & on_token,
                                 std::string & full_reply,
                                 std::string & err) {
    std::lock_guard<std::mutex> lk(api_mu_);
    if (!loaded_.load(std::memory_order_acquire) || !ctx_ || !vocab_) {
        err = "Модель не загружена";
        return false;
    }

    // Clear KV state for deterministic raw runs
    llama_memory_seq_rm(llama_get_memory(ctx_), -1, -1, -1);
    kv_tokens_.clear();

    int32_t n_tok = -llama_tokenize(vocab_, prompt.c_str(), (int32_t) prompt.size(), nullptr, 0, true, false);
    if (n_tok <= 0) n_tok = 1;
    std::vector<llama_token> tokens((size_t) n_tok);
    const int32_t got = llama_tokenize(vocab_, prompt.c_str(), (int32_t) prompt.size(),
                                       tokens.data(), (int32_t) tokens.size(), true, false);
    if (got > 0) {
        tokens.resize((size_t) got);
    } else {
        llama_token bos = llama_vocab_bos(vocab_);
        tokens = {bos >= 0 ? bos : 0};
    }

    SamplingParams sp = cfg_.sampling;
    sp.max_tokens  = max_tokens;
    sp.temperature = temperature;
    sp.seed        = seed;

    int32_t p_out = 0, c_out = 0;
    return run_tokens(tokens, sp, on_token, full_reply, p_out, c_out, err);
}

bool DualMoeEngine::run_tokens(const std::vector<llama_token> & tokens,
                               const SamplingParams & sp,
                               const TokenCallback & on_token,
                               std::string & full_reply,
                               int32_t & prompt_tokens_out,
                               int32_t & completion_tokens_out,
                               std::string & err) {
    full_reply.clear();
    prompt_tokens_out     = (int32_t) tokens.size();
    completion_tokens_out = 0;
    cancel_flag_.store(false, std::memory_order_release);
    generating_.store(true, std::memory_order_release);

    const uint32_t n_ctx = llama_n_ctx(ctx_);
    const int32_t  max_new = std::max<int32_t>(1, sp.max_tokens);
    if (tokens.size() + 8 >= n_ctx) {
        generating_.store(false, std::memory_order_release);
        err = "Длина промпта превышает размер контекста (увеличьте Context Budget)";
        return false;
    }

    // Prefix KV cache reuse (crucial for 14k-token LearnHarness sessions!)
    size_t common_prefix = 0;
    const size_t max_prefix = std::min(kv_tokens_.size(), tokens.size() > 0 ? tokens.size() - 1 : 0);
    while (common_prefix < max_prefix && kv_tokens_[common_prefix] == tokens[common_prefix]) {
        ++common_prefix;
    }
    if (common_prefix < kv_tokens_.size()) {
        if (!llama_memory_seq_rm(llama_get_memory(ctx_), 0, (llama_pos) common_prefix, -1)) {
            // Hybrid recurrent models (DeltaNet/Mamba) disallow partial tail trim: clean reset fallback
            llama_memory_seq_rm(llama_get_memory(ctx_), -1, -1, -1);
            common_prefix = 0;
        }
        kv_tokens_.resize(common_prefix);
    }

    // Prefill new prompt suffix in chunks of n_batch
    const auto t_prefill_start = std::chrono::steady_clock::now();
    const size_t batch_cap = std::max<size_t>(32, llama_n_batch(ctx_));
    llama_batch batch = llama_batch_init((int32_t) batch_cap, 0, 1);

    size_t pos = common_prefix;
    while (pos < tokens.size()) {
        const size_t chunk = std::min(batch_cap, tokens.size() - pos);
        batch.n_tokens = (int32_t) chunk;
        for (size_t i = 0; i < chunk; ++i) {
            batch.token[i]     = tokens[pos + i];
            batch.pos[i]       = (llama_pos) (pos + i);
            batch.n_seq_id[i]  = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i]    = (pos + i + 1 == tokens.size()) ? 1 : 0;
        }
        const int rc = llama_decode(ctx_, batch);
        if (rc != 0) {
            llama_batch_free(batch);
            generating_.store(false, std::memory_order_release);
            if (cancel_flag_.load(std::memory_order_acquire)) {
                err = "Остановлено пользователем";
                return false;
            }
            err = "Ошибка декодирования промпта (код " + std::to_string(rc) + ")";
            return false;
        }
        for (size_t i = 0; i < chunk; ++i) {
            kv_tokens_.push_back(tokens[pos + i]);
        }
        pos += chunk;
        kv_used_.store((uint32_t) kv_tokens_.size(), std::memory_order_relaxed);
    }

    const auto t_prefill_end = std::chrono::steady_clock::now();
    const double prefill_ms = std::chrono::duration<double, std::milli>(t_prefill_end - t_prefill_start).count();
    const size_t evaluated_prompt = tokens.size() - common_prefix;
    ttft_ms_.store(prefill_ms, std::memory_order_relaxed);
    if (prefill_ms > 0.5 && evaluated_prompt > 0) {
        prefill_tps_.store((double) evaluated_prompt * 1000.0 / prefill_ms, std::memory_order_relaxed);
    }
    prompt_tokens_.fetch_add(evaluated_prompt, std::memory_order_relaxed);

    // Build sampler chain
    llama_sampler * smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (sp.repeat_penalty > 1.001f && sp.repeat_last_n > 0) {
        llama_sampler_chain_add(smpl, llama_sampler_init_penalties(
            llama_vocab_n_tokens(vocab_), sp.repeat_last_n, sp.repeat_penalty, 0.0f, 0.0f));
    }
    if (sp.temperature <= 0.0f) {
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    } else {
        if (sp.top_k > 0) llama_sampler_chain_add(smpl, llama_sampler_init_top_k(sp.top_k));
        if (sp.top_p < 1.0f) llama_sampler_chain_add(smpl, llama_sampler_init_top_p(sp.top_p, 1));
        if (sp.min_p > 0.0f) llama_sampler_chain_add(smpl, llama_sampler_init_min_p(sp.min_p, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(sp.temperature));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(sp.seed));
    }

    // Feed recent prompt tail to repetition penalty sampler
    const size_t pen_tail = sp.repeat_last_n > 0 ? std::min<size_t>(tokens.size(), (size_t) sp.repeat_last_n) : 0;
    for (size_t i = tokens.size() - pen_tail; i < tokens.size(); ++i) {
        llama_sampler_accept(smpl, tokens[i]);
    }

    std::string utf8_pending;
    const auto t_decode_start = std::chrono::steady_clock::now();

    for (int32_t step = 0; step < max_new; ++step) {
        if (cancel_flag_.load(std::memory_order_acquire)) break;
        if (kv_tokens_.size() + 2 >= n_ctx) break;

        const llama_token next_tok = llama_sampler_sample(smpl, ctx_, -1);
        llama_sampler_accept(smpl, next_tok);

        if (llama_vocab_is_eog(vocab_, next_tok)) {
            break;
        }

        char piece_buf[256];
        int32_t piece_len = llama_token_to_piece(vocab_, next_tok, piece_buf, sizeof(piece_buf), 0, false);
        std::string piece;
        if (piece_len > (int32_t) sizeof(piece_buf)) {
            std::vector<char> big((size_t) piece_len + 8);
            piece_len = llama_token_to_piece(vocab_, next_tok, big.data(), (int32_t) big.size(), 0, false);
            if (piece_len > 0) piece.assign(big.data(), (size_t) piece_len);
        } else if (piece_len > 0) {
            piece.assign(piece_buf, (size_t) piece_len);
        }

        utf8_pending += piece;
        const size_t tail = incomplete_utf8_tail(utf8_pending);
        if (utf8_pending.size() > tail) {
            const std::string valid_chunk = utf8_pending.substr(0, utf8_pending.size() - tail);
            utf8_pending.erase(0, valid_chunk.size());
            full_reply += valid_chunk;
            if (on_token && !on_token(valid_chunk)) {
                cancel_flag_.store(true, std::memory_order_release);
                break;
            }
        }

        ++completion_tokens_out;
        generated_tokens_.fetch_add(1, std::memory_order_relaxed);

        const auto now = std::chrono::steady_clock::now();
        const double dec_ms = std::chrono::duration<double, std::milli>(now - t_decode_start).count();
        if (dec_ms > 1.0 && completion_tokens_out > 0) {
            decode_tps_.store((double) completion_tokens_out * 1000.0 / dec_ms, std::memory_order_relaxed);
        }

        if (step + 1 >= max_new) break;

        // Decode the newly sampled token into KV cache for the next step
        batch.n_tokens     = 1;
        batch.token[0]     = next_tok;
        batch.pos[0]       = (llama_pos) kv_tokens_.size();
        batch.n_seq_id[0]  = 1;
        batch.seq_id[0][0] = 0;
        batch.logits[0]    = 1;

        const int rc = llama_decode(ctx_, batch);
        if (rc != 0) break;
        kv_tokens_.push_back(next_tok);
        kv_used_.store((uint32_t) kv_tokens_.size(), std::memory_order_relaxed);
    }

    if (!utf8_pending.empty()) {
        full_reply += utf8_pending;
        if (on_token) on_token(utf8_pending);
    }

    llama_sampler_free(smpl);
    llama_batch_free(batch);
    generating_.store(false, std::memory_order_release);
    return true;
}

TelemetrySnapshot DualMoeEngine::telemetry() const {
    TelemetrySnapshot t;
    t.model_loaded = loaded_.load(std::memory_order_acquire);
    t.generating   = generating_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        t.model_name             = layout_.summary.model_name;
        t.model_arch             = layout_.summary.arch;
        t.status_text            = status_text_;
        t.kv_max_tokens          = cfg_.n_ctx;
        t.vram_cache_budget_mb   = active_vram_cache_mb_;
        t.auto_split_active      = auto_split_active_;
        t.parallel_gpu_cpu_active = parallel_gpu_cpu_;
        t.active_gpu_layers      = active_gpu_layers_;
        t.active_cpu_share       = active_cpu_share_;
    }
    t.prefill_tps          = prefill_tps_.load(std::memory_order_relaxed);
    t.decode_tps           = decode_tps_.load(std::memory_order_relaxed);
    t.ttft_ms              = ttft_ms_.load(std::memory_order_relaxed);
    t.prompt_tokens        = prompt_tokens_.load(std::memory_order_relaxed);
    t.generated_tokens     = generated_tokens_.load(std::memory_order_relaxed);
    t.kv_used_tokens       = kv_used_.load(std::memory_order_relaxed);

    t.gpu_expert_hits      = gpu_expert_hits_.load(std::memory_order_relaxed);
    t.gpu_expert_fills     = gpu_expert_fills_.load(std::memory_order_relaxed);
    t.cpu_parallel_experts = cpu_parallel_experts_.load(std::memory_order_relaxed);
    t.vram_upload_bytes    = vram_upload_bytes_.load(std::memory_order_relaxed);

    t.ram_cache_hits       = arena_.ram_hits();
    t.ssd_expert_reads     = arena_.ssd_reads();
    t.ssd_bytes_read       = arena_.ssd_bytes_read();
    t.ssd_bandwidth_mbs    = arena_.ssd_bandwidth_mbs();
    const uint64_t comm_b  = arena_.committed_bytes();
    t.ram_arena_used_mb    = comm_b > 0 ? ((comm_b + (1ull << 20) - 1) >> 20) : 0;
    t.ram_arena_budget_mb  = arena_.budget_bytes() >> 20;
    const uint64_t anon_b  = arena_.dense_anon_bytes();
    t.dense_anon_mb        = anon_b > 0 ? ((anon_b + (1ull << 20) - 1) >> 20) : 0;
    t.mmap_released        = arena_.mmap_released();
    return t;
}

} // namespace dualmoe
